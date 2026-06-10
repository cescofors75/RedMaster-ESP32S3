# Auditoría de código — RedMaster_ESP32S3

Auditoría de bugs, concurrencia, correctness y performance del firmware ESP32-S3
(Arduino/PlatformIO, dual-core FreeRTOS) que actúa como master SPI de una Daisy Seed (STM32).

Alcance: `src/` del proyecto `RedMaster_ESP32S3` (~16k líneas propias).
Núcleos: **Core1** = `spiAudioTask` (secuenciador + SPI, prio 24). **Core0** = `systemTask`
(WiFi/web/MIDI/LED/botones, prio 5) + tarea **AsyncTCP** (handlers HTTP/WS). `loop()` corre
en loopTask (Core1, prio 1). Estado de audio compartido además con Core1.

Severidad: CRITICAL / HIGH / MEDIUM / LOW. ✅ = verificado leyendo el código directamente.

---

## Temas transversales (causas raíz)

1. **Locking inconsistente de estado compartido entre núcleos.** El `patternMutex` del
   Sequencer protege solo ~5 mutadores; decenas de getters/setters y varios globales
   (`gMaster*`, `_stateBuf`, arrays de pad/melodic hold, `seqNumber`, `cachedStatus`) se
   tocan desde 2-3 contextos sin protección.
2. **El watchdog no vigila las dos tareas principales** (orden de init incorrecto).
3. **La tarea de red (AsyncTCP) se bloquea** durante uploads / selectPattern / carga de samples.
4. **Parsing de WAV sin validar** (overflow de enteros) en archivos subidos por el usuario.
5. **Framing del protocolo SPI frágil**: colisiones magic↔command-id, semántica de checksum
   inconsistente, lectura half-duplex sin handshake.
6. **Lecturas fuera de límites** (filterPresets, trigger binario WS, getters sin bounds, SysLog).

---

## CRITICAL

### C1 — El watchdog no protege a `spiAudioTask` ni `systemTask` ✅
`main.cpp:1380-1407`, `main.cpp:344`, `main.cpp:439`
Las tareas se crean (prio 24 y 5) **antes** de `esp_task_wdt_init()` (línea 1407). Como tienen
mayor prioridad que `setup()` (loopTask, prio 1), arrancan y llaman `esp_task_wdt_add(NULL)`
antes de que el TWDT exista → `ESP_ERR_INVALID_STATE` ignorado. El núcleo de audio/SPI **no
queda vigilado**; solo `loopTask`. Anula el diseño de recuperación ante cuelgues.
**Fix:** llamar `esp_task_wdt_init()` antes de `xTaskCreatePinnedToCore`, o que cada tarea
espere (vTaskDelay) a un flag "wdt listo" antes de `esp_task_wdt_add`.

### C2 — `dsqUploadPattern` lee el patrón sin lock desde Core1 mientras Core0 lo edita ✅
`main.cpp:303-339` (boot y upload diferido)
Usa `getStep/getStepVelocity/getStepNoteLen/getStepProbability/getStepCutoffLock` que **no**
toman `patternMutex` (`Sequencer.cpp:307,378,399,...`), mientras Core0 (WS) llama
`setStep/clearPattern/setPatternBulk` que sí lockean. Un `clearPattern` (memset PSRAM)
concurrente produce uploads desgarrados → exactamente el síntoma de "tracks parciales /
patrones vacíos" que describen los comentarios en `main.cpp:319-321`.
**Fix:** hacer que los getters por patrón lockeen, o añadir `getPatternSnapshot()` que copie
bajo un solo lock e iterar el snapshot.

### C3 — Buffers scratch compartidos `_stateBuf` / `_patternBuf` con carrera entre tareas ✅
`WebInterface.cpp:116, 2604-2619, 2744-2749` (calls desde `processCommand` en AsyncTCP:
3011, 3157, 3408, 3420, 3454)
`_stateBuf` (8 KB) es serializado por `broadcastSequencerState()` —invocado desde
`processCommand` en la tarea **AsyncTCP**— y por `sendUdpStateSync()` en **systemTask**.
Dos tareas pueden hacer `serializeJson(...,_stateBuf,...)` + `ws->textAll()` a la vez →
JSON entrelazado/corrupto o lectura de buffer a medio reescribir.
**Fix:** mutex, o buffer propio para el camino AsyncTCP, o canalizar todos los broadcasts por
el mecanismo de flags diferidos que ya existe (`_pendingBroadcastStep`).

### C4 — Use-after-free en la descarga chunked `/api/sampledata` ✅(reportado)
`WebInterface.cpp:2021-2085`
El lambda de respuesta chunked captura un `int16_t* buffer = getSampleBuffer(pad)` crudo y
lo va streameando a lo largo de muchos callbacks. Si el pad se descarga/recarga/recorta
(`unloadDaisy`, `loadSample`, `trimSample`, `releaseHostSample` en `pumpPadTransfer`) durante
la transferencia, el puntero queda colgando → lectura de PSRAM liberada / crash.
**Fix:** copiar a un buffer propio de la respuesta, o refcount/lock del sample durante el envío.

### C5 — `getRecentMessages`: índice circular sin sincronizar + carrera del historial MIDI ✅(reportado)
`MIDIController.cpp:221-229, 249-258`
`processMIDIMessage` corre en el callback USB (Core0), mientras `getRecentMessages` y los
getters de stats se llaman desde la tarea web. `messageHistory/historyIndex/historyCount/
totalMessages` se leen/escriben desde dos contextos sin mutex; copia no atómica de
`MIDIMessage` (5 campos) → puede devolver `idx` fuera de rango o datos basura.
**Fix:** snapshot bajo critical section en lector y escritor; simplificar a
`startIdx = (historyIndex + MAX_HISTORY - count) % MAX_HISTORY`.

---

## HIGH

### H1 — MIDI muere para siempre tras un STALL USB ✅
`MIDIController.cpp:16-20`
En STALL no se reenvía el transfer y **no** se limpia `s_transferSubmitted` (queda en `true`)
→ `readMidiData()` (`:459`) retorna temprano de forma permanente y la entrada MIDI se apaga
sin recuperación.
**Fix:** en STALL/CANCELED poner `s_transferSubmitted=false` (e idealmente limpiar el endpoint
halt) para poder reenviar.

### H2 — `s_transferSubmitted` / handle USB tocados desde dos contextos; UAF en desconexión ✅(reportado)
`MIDIController.cpp:5, 271, 429, 459, 470`
`closeMidiDevice` libera `midiTransfer` (`:431-433`) mientras el callback puede referenciarlo
en el camino de reenvío → use-after-free si el device se quita en mitad de un transfer. Bool
sin `volatile`/atomic.
**Fix:** marcar atómico, y cancelar+drenar el transfer completamente antes de
`usb_host_transfer_free`.

### H3 — Lectura half-duplex sin handshake de "respuesta lista" ✅(reportado)
`SPIMaster.cpp:319-337`
La cabecera de respuesta y el payload se clockean en la **misma** ventana CS-low sin garantía
de que la Daisy haya preparado la respuesta. Respuestas mayores que el RXFIFO (16B) —
`StatusResponse` 60B, `SdFileListResponse` ~676B, `EventsResponse` 129B— solo funcionan si la
Daisy las streamea en tiempo real durante una sola aserción CS. Causa probable de respuestas
inestables.
**Fix:** handshake explícito (poll de byte de estado o GPIO DATA_READY) antes de clockear el
payload; leer cabecera y payload en ciclos CS separados.

### H4 — `seqNumber++` sin protección, compartido entre núcleos ✅
`SPIMaster.cpp:222, 281`, `SPIMaster.h:459`
`header.sequence = seqNumber++` se ejecuta **antes** de `xSemaphoreTake`; `seqNumber` es
`uint16_t` no atómico escrito desde Core0 y Core1 → secuencias duplicadas/saltadas que anulan
la verificación de secuencia del slave.
**Fix:** incrementar dentro de la región protegida por mutex, o hacerlo atómico.

### H5 — Lockeo inconsistente de getters/setters del Sequencer ✅
`Sequencer.cpp`: sin lock en `getStep` (300/307), `getStepVelocity`/`setStepVelocity`
(358/364/371/378), `setStepNoteLen` (386), `setStepNote`/`setStepNoteVoice` (408/415/440),
`setStepFlags` (473/479), `setStepProbability` (694), `setStepRatchet` (720),
`setStepCutoffLock` (622/630), `setStepReverbSendLock` (658), `setStepVolumeLock` (584),
`copyPattern` (558-578). Solo lockean `setStep/clearPattern/clearTrack/setPatternBulk/
selectPattern`. Campos multibyte (`stepCutoffLockHz` uint16, arrays de voces) pueden leerse a
medio actualizar desde Core1.
**Fix:** envolver todo acceso a `pd->` en `lockPattern()/unlockPattern()` de forma uniforme.

### H6 — Escalares compartidos del Sequencer sin `volatile`/atómico ✅(reportado)
`Sequencer.cpp`: `currentPattern` (escrito 524/795/910 y 152/165), `currentStep`,
`playing` (116 vs start/stop), `stepInterval`/`nextStepInterval`. No `volatile` → el compilador
puede cachearlos en registro dentro del `while(true)`; cambios de tempo/patrón pueden no verse.
**Fix:** marcar `volatile` (o proteger con el mutex), igual que el patrón ya usado con
`gTrackSynthEngine`.

### H7 — Arrays de melodic-hold y pad-auto-off mutados desde 2 núcleos sin sincronizar ✅(reportado)
`main.cpp:140-159` (`gPadTrigOff*`), `171-183`/`799-814` (`gSeqMelodicHeld*`)
`gPadTrigOff*` se modifican desde `triggerPadWithLED` (Core0) y se drenan en
`drainPadTriggerAutoOff()` desde `loop()` (Core1) → read-modify-write no atómico puede mandar
un NoteOff espurio o dejar una voz colgada (nota pegada). Igual para los held melódicos si el
engine cambia a mitad de tick vía `gTrackSynthEngine` (escrito desde Core0).
**Fix:** proteger con `portMUX`/critical section, o mover scheduling+drain a un solo núcleo.

### H8 — Bloqueo de la tarea AsyncTCP durante uploads ✅(reportado)
`WebInterface.cpp:6261-6279, 6373-6424` (vía `handleDaisyUpload:6472`)
`processDaisyUploadPcm → flushPcm → pushDaisyUploadBlock` hace `while(!pushed){delay(1);yield();}`
y `flushPcm` hace `vTaskDelay(2ms)` hasta 3000 veces (≈6 s) **en la tarea de red**. Congela
todos los demás clientes HTTP/WS y puede tirar WiFi durante uploads grandes.
**Fix:** retornar del callback cuando la cola esté llena y dejar que AsyncWebServer aplique
backpressure TCP real; mover la conversión PCM fuera de la tarea TCP.

### H9 — Colisiones magic-byte ↔ command-id en el protocolo SPI ✅
`protocol.h`: `SPI_MAGIC_CMD 0xA5` == `CMD_AUTOWAH_ACTIVE 0xA5` (210);
`SPI_MAGIC_RESP 0x5A` == `CMD_TRACK_DELAY_SEND 0x5A` (148);
`SPI_MAGIC_SAMPLE 0xDA` == `CMD_DSQ_SET_TRACK_SWING 0xDA` (376).
Posicionalmente magic=byte0 y cmd=byte1 no se solapan, pero cualquier slave que reseincronice
escaneando el stream por el magic se desincronizará en esos paquetes. Footgun latente.
**Fix:** reasignar las constantes magic a valores fuera del espacio de command-ids.

### H10 — Overflow de entero al recorrer chunks del WAV (entrada de usuario) ✅(reportado)
`SampleManager.cpp:152, 445`; `WebInterface.cpp:338, 6345`
`pos += 8 + chunkSize + (chunkSize & 1)` con `chunkSize` (uint32) controlado por el archivo:
un valor cercano a `0xFFFFFFFF` desborda `pos` y hace que el guard `pos + 8 <= fileSize`
envuelva → bucle infinito o saltos de bounds.
**Fix:** validar `chunkSize <= fileSize - pos - 8` antes de avanzar; usar aritmética de 64 bits.

---

## MEDIUM

### M1 — Semántica inconsistente de `SampleEndPayload.checksum` ✅(reportado)
`SPIMaster.cpp:1208` (CRC16 truncado a 65535B en un campo "CRC32"), `:1752` (mete
`totalSamples`), `:1254` (pone 0). El slave no puede interpretar el campo de forma fiable; para
samples >64KB solo valida un prefijo y además es CRC16, no CRC32.
**Fix:** un único significado; CRC32 real sobre todo el buffer, consistente en los 3 caminos.

### M2 — Lectura fuera de límites en `getFilterPreset` ✅
`SPIMaster.cpp:1804-1808` (array de 13 entradas, índices 0-12), `SPIMaster.h:54-72`
El enum `FilterType` tiene huecos (LADDER=10, SVF_LP=11, SVF_HP=12, SVF_BP=13, COMB=14,
REVERSE=17, HALFSPEED=18, STUTTER=19). El check `type <= FILTER_STUTTER(19)` indexa
`filterPresets[type]`: tipos 13-19 → **OOB read**; 10-12 devuelven el preset equivocado.
**Fix:** usar un `switch`/lookup por valor de enum y acotar al tamaño real del array (13).

### M3 — Drenado de la cola SPI estrangulado (8/tick) frente a transfers grandes ✅(reportado)
`SPIMaster.cpp:1140-1148` (retry hasta 1500× con `vTaskDelay(2ms)` ≈3s por chunk),
`drainCmdQueue:182` (solo 8 por `process()`). Un sample de 2MB (~4000 chunks) serializa
productor y consumidor por una ventana mínima → transferencias muy lentas.
**Fix:** drenar toda la cola (o ráfagas mucho mayores) en Core1, o `sendCommandDirect` si ya
se está en Core1.

### M4 — Polling SPI (peaks/status/SD/events) compite con uploads ✅(reportado)
`SPIMaster.cpp:374-404` (ping 2s, status+SD+events 3s, peaks 200ms)
Cada round-trip toma `spiMutex` hasta 8×800µs e interleava con `drainCmdQueue` durante un
upload de sample.
**Fix:** suspender el polling de peaks/status mientras hay stream/transfer activo, o gatear el
polling a cola vacía.

### M5 — Escrituras NVS redundantes por cada edición de mapeo MIDI (desgaste de flash) ✅(reportado)
`MIDIController.cpp:486, 499, 510, 535` → `saveMappings():598-613`
Cada `setNoteMapping/setPadMapping/resetToDefaultMapping` reescribe NVS entero. Edición pad a
pad desde la UI desgasta la flash.
**Fix:** marcar dirty y flushear una vez tras un periodo de calma, o `commit()` explícito.

### M6 — Carga de samples por SPI (bloqueante) en la tarea AsyncTCP ✅(reportado)
`WebInterface.cpp:3610 (loadSample), 3709 (loadXtraSample)`
`sampleManager.loadSample(...)` streamea el sample a la Daisy por SPI desde `processCommand`
en el camino WS, bloqueando la red — justo lo que otros caminos ya evitaron.
**Fix:** rutear por el mecanismo diferido `_pendingLoadPad`/pump.

### M7 — Globales `gMaster*` FX sin `volatile` leídos/escritos desde varias tareas ✅(reportado)
`WebInterface.cpp:518-531`
`gMasterFilterType/gMasterDelayMix/...` son `static` planos, escritos por `processCommand`
(AsyncTCP y systemTask) y leídos por los builders de estado (systemTask). Valores stale/torn.
**Fix:** marcar `volatile` como mínimo; para estado multi-campo, lock.

### M8 — SysLog: I/O en LittleFS desde varios núcleos sin lock + carrera de rotación ✅(reportado)
`SysLog.cpp:46-61`
Cada `syslog()` hace open-read-size-close + rename condicional + open-append-close. LittleFS no
es reentrante; llamadas concurrentes (y `syslogPanic` desde el crash handler) corrompen el FS
o pierden el log vivo en el rename.
**Fix:** serializar las escrituras tras un mutex; en `syslogPanic` aceptar que solo corre al
apagar.

### M9 — `random()` en el path de audio de Core1 (no thread-safe + costoso) ✅(reportado)
`Sequencer.cpp:230, 271, 881`, `LFOEngine.cpp:206`
PRNG global compartido; también se llama probablemente desde Core0 → bias/corrupción. Además se
llama hasta `MAX_TRACKS×ratchet` veces por step en el hot path.
**Fix:** PRNG por núcleo (xorshift local / `esp_random()`).

### M10 — Overflow en cálculo de tamaño de buffer del sample ✅(reportado)
`SampleManager.cpp:177, 257-259, 464`
`numSamples = dataSize/(bytesPerSample*numChannels)`; `numSamples*2` puede desbordar `size_t`
y pasar el check de `MAX_SAMPLE_SIZE` con `bytes` minúsculo → overrun en la copia.
**Fix:** comprobar `size > MAX_SAMPLE_SIZE/2` antes de multiplicar.

### M11 — Getters WS sin validar rango (`getStepVelocity`/`getTrackVolume`/`getStepNote`) ✅(reportado)
`WebInterface.cpp:4936-4940, 5300-5303`
Leen `track`/`step` del JSON y llaman al sequencer sin validar (a diferencia de los setters).
Si el sequencer indexa sin clamp → OOB read.
**Fix:** validar rangos como hacen los setters.

### M12 — Trigger binario WS con índice de pad sin acotar ✅(reportado)
`WebInterface.cpp:2243-2249`
`{0x90, PAD, VEL}` pasa `data[1]` (0-255) directo a `triggerPadWithLED(pad,vel)` sin chequear
`pad < 24` (los caminos JSON sí lo hacen). Indexa arrays de LED/track → posible OOB.
**Fix:** validar `pad < 24` antes de llamar.

### M13 — `vsnprintf` de SysLog puede escribir más que el buffer ✅(reportado)
`SysLog.cpp:33, 59`
`offset += vsnprintf(...)` suma la longitud "que se habría escrito"; en truncamiento
`offset > sizeof(line)` y `f.write(line, offset)` lee OOB del buffer de 256B.
**Fix:** `offset = min(offset, sizeof(line)-1)` tras vsnprintf.

### M14 — `getWaveformPeaks` escribe 2× los puntos que retorna ✅(reportado)
`SampleManager.cpp:570, 588-589`
Retorna `points` pero escribe `2*points` int8 (hasta 400B con cap 200). Si el caller dimensiona
por el valor de retorno, overrun 2×.
**Fix:** `points = min(maxPoints/2, 200)`.

### M15 — Detección de "enabled" de param-locks por valor (heurística frágil) ✅
`main.cpp:326-330`
`bool ce = (ch != 0 && ch != 1000)` descarta un lock real de 1000 Hz; `bool ve = (vl != 0)`
confunde un volume-lock legítimo de 0 con "deshabilitado".
**Fix:** usar los accessors explícitos `hasStep*Lock()`.

### M16 — `begin()` de SPIMaster siempre retorna `true` ✅(reportado)
`SPIMaster.cpp:145-153`
Tras 10 pings fallidos cae a `return true`; el caller nunca detecta "Daisy ausente al boot".
**Fix:** `return false` tras el bucle, o documentar que hay que comprobar `isConnected()`.

### M17 — Patrón `doc["a"] | doc["b"] | default` frágil/ambiguo ✅
`WebInterface.cpp:3479` (y otros usos de `doc["x"] | doc["y"]`)
El operador `|` de ArduinoJson está pensado como `variant | default`; encadenar dos variants es
dependiente de versión y puede no respetar la presencia/valor 0 esperados.
**Fix:** usar `containsKey()` explícito.

---

## LOW

- **L1** `LFOEngine` S&H: detección de nuevo ciclo por umbral `phase < hz*dt` puede latch/saltar
  a divisiones altas. Detectar el wrap explícitamente. `LFOEngine.cpp:205`.
- **L2** `LFOEngine`/`cachedStatus`/`stm32Connected` leídos desde Core0 mientras Core1 escribe
  (telemetría; floats/bool word-atómicos pero RMW de fase puede perder retrigger).
  `LFOEngine.cpp:311-324`, `SPIMaster.cpp:1707/1769/1353/1374`.
- **L3** `KitManager::scanKits` usa `snprintf(...,127,...)` (no `sizeof`) y no verifica
  semántica de `file.name()` (basename vs path) ni truncamiento. `KitManager.cpp:66-72`.
- **L4** `loadMappings` no valida rango de `note`(0-127)/`pad`(-1..15) por entrada.
  `MIDIController.cpp:621-638`.
- **L5** `setNoteMapping` descarta silenciosamente cuando la tabla está llena (sin feedback).
  `MIDIController.cpp:493-513`.
- **L6** `processCommand` con heap<20000 dropea el comando sin responder → UI colgada esperando
  respuesta. Enviar `{"type":"error","msg":"low_heap"}`. `WebInterface.cpp:3250-3253`.
- **L7** `/midi/*` sirve `request->url()` desde LittleFS sin rechazar `..` (inconsistente con
  `/api/waveform`). `WebInterface.cpp:1455-1462`.
- **L8** `/api/buttons` POST asume body en un solo chunk (`index`/`total` ignorados); configs
  grandes se truncan. `WebInterface.cpp:1754-1755`.
- **L9** Sin autenticación en ningún endpoint HTTP/WS/UDP (aceptable en modo AP; expuesto en LAN
  con STA).
- **L10** `PhysControlButtons`: comentario de cabecera dice PULLUP/LOW pero el código usa
  PULLDOWN/HIGH; mezcla `CTRL_LED_COUNT` con literal `4`. `PhysControlButtons.cpp:50,73-79`,
  `.h:12-13`.
- **L11** Stacks grandes: `StaticJsonDocument<4096>` en `handleUdp`/`/api/p4State`, `<3072>` en
  sysinfo, `inBuf[1024]+parseBuf[1032]+pcm[512]` en `pumpCleanTrackStream`. Riesgo en stack
  pequeño de AsyncTCP. `WebInterface.cpp:6034,1355,1521,6548-6550`.
- **L12** `WavHeader` (`SampleManager.h:18-32`) muerto y engañoso; eliminar.

---

## Aspectos correctos observados
- Endianness coherente (ambos little-endian, sin byte-swap necesario).
- `crc16` (CCITT reflejado) implementado y aplicado consistentemente sobre payload.
- Asignación PSRAM para JSON grandes; flags diferidos Core1→Core0 para broadcasts; ring buffer
  de upload Daisy protegido con `portMUX`; máquinas de estado no bloqueantes para pumps; guards
  de heap antes de asignaciones pesadas; el manejo de overflow de `millis()` en
  `drainPadTriggerAutoOff` es correcto (resta con signo).

## Estado de correcciones (lote seguro aplicado)

Aplicadas en esta rama (cambios de bajo riesgo, sin tocar el firmware de la Daisy):

| ID | Fix | Archivo |
|----|-----|---------|
| C1 | `esp_task_wdt_init()` movido antes de crear las tasks → el núcleo audio/SPI queda vigilado | `main.cpp` |
| H6 | `playing/currentPattern/currentStep/stepInterval/nextStepInterval` marcados `volatile` | `Sequencer.h` |
| C5 | Historial/contadores MIDI protegidos con `portMUX` + fórmula de índice robusta | `MIDIController.cpp` |
| H1 | Recuperación tras STALL USB (`s_transferSubmitted=false` cuando no se reenvía) | `MIDIController.cpp` |
| H2 (parcial) | `s_transferSubmitted` → `volatile` (teardown USB endpoint queda como follow-up) | `MIDIController.cpp` |
| H10 | Overflow de enteros al recorrer chunks WAV + clamp de `dataSize` (file y buffer) | `SampleManager.cpp` |
| M10 | Check de límite antes de `size*2` en `allocateSampleBuffer` | `SampleManager.cpp` |
| M2 | `getFilterPreset` busca por `.type` en vez de indexar (no más OOB read) | `SPIMaster.cpp` |
| M5 | Debounce de escritura NVS de mapeos MIDI (1.5s) + persiste `clearMapping` | `MIDIController.cpp/.h` |
| M13 | Clamp de `offset` tras snprintf/vsnprintf en SysLog (no más OOB read) | `SysLog.cpp` |
| M14 | Contrato de `getWaveformPeaks` documentado (no era bug activo) | `SampleManager.h` |
| C2 | `dsqUploadPattern` usa `snapshotTrackForUpload()`: copia la pista bajo un solo lock y hace el SPI fuera del mutex → fin de los uploads desgarrados | `main.cpp`, `Sequencer.h/.cpp` |
| H5 | Locking uniforme: TODOS los getters/setters que tocan `pd->` toman `patternMutex` (ahora recursivo, para los accessors que se delegan entre sí) | `Sequencer.cpp/.h` |
| M15 | Param-locks usan las flags `enabled` reales del snapshot en vez de inferirlas del valor (ya no se pierde cutoff=1000Hz ni volumen=0) | `main.cpp` |
| C3 | Mutex `_jsonBufMutex` serializa `_stateBuf`/`_patternBuf` entre AsyncTCP y systemTask (4 sitios) | `WebInterface.cpp` |
| C4 | `/api/sampledata`: snapshot PSRAM con `shared_ptr` propiedad de la respuesta — fin del use-after-free si el pad se recarga durante la descarga | `WebInterface.cpp` |
| H4 | `seqNumber++` movido dentro de `spiMutex` en `sendCommandDirect`/`sendAndReceive` | `SPIMaster.cpp` |
| H7 | `gPadTrigOff*`/`gSeqMelodicHeld*` protegidos con `portMUX` (reclamo atómico; SPI siempre fuera de la sección crítica) | `main.cpp` |
| M4 | Polling de ping/peaks/status se salta cuando hay backlog en la cola SPI (transfer bulk en curso) → menos contención de `spiMutex` y uploads más rápidos | `SPIMaster.cpp` |
| M7 | Globales `gMaster*` marcados `volatile` | `WebInterface.cpp` |
| M9 | `random()` sustituido por PRNG local xorshift32 en el hot path de audio (Sequencer + LFO S&H) | `Sequencer.cpp`, `LFOEngine.cpp` |
| M12 | Trigger binario WS valida `pad < MAX_PADS` | `WebInterface.cpp` |
| M16 | `begin()` documenta el `return true` intencional + warning por Serial si la Daisy no responde al boot | `SPIMaster.cpp` |
| M17 | `selectPattern` resuelve `index`/`pattern` con `isNull()` explícito (el encadenado `\|` hacía OR bitwise) | `WebInterface.cpp` |
| L2 | `stm32Connected` → `volatile` | `SPIMaster.h` |
| L3 | `KitManager::scanKits` usa `sizeof` y descarta paths truncados | `KitManager.cpp` |
| L4 | `loadMappings` valida `note` (0-127) y `pad` (-1..15) por entrada | `MIDIController.cpp` |
| L6 | Heap guard responde `{"type":"error","msg":"low_heap"}` en vez de dejar la UI colgada | `WebInterface.cpp` |
| L7 | `/midi/*` rechaza traversal (`..`) y exige prefijo `/midi/` | `WebInterface.cpp` |
| L10 | Comentario PULLUP/PULLDOWN corregido | `PhysControlButtons.h` |
| M8 | Escrituras de SysLog (rotación + append) serializadas con mutex; `syslogPanic` sigue sin lockear (shutdown handler) | `SysLog.cpp` |

**Notas sobre lo no aplicado:**
- **M3** (drain 8/tick): se mantiene a propósito — el cap está ajustado para no acaparar
  Core1 (documentado en el código); M4 elimina la contención real.
- **M11**: no-bug — los getters del Sequencer ya validan rangos internamente.
- **H8/M6** (busy-wait en uploads / loadSample síncrono en AsyncTCP): requieren rediseñar
  el flujo de upload y probarse con hardware; no tocar a ciegas.

**Pendiente (requieren coordinación con firmware Daisy o pruebas con hardware):**
H3/H9/M1 (protocolo SPI: handshake, colisiones magic, checksum), H2-completo (teardown
USB), H8/M6 (flujo de upload) y LOW menores (L11/L12).
H2 completo (secuencia `usb_host_endpoint_halt/flush` antes de `usb_host_transfer_free`)
no se aplicó por no poder compilar-verificar las APIs USB en este entorno.

## Prioridad de corrección sugerida
1. **C1** (watchdog) — cambio pequeño, alto impacto en fiabilidad.
2. **C2/H5/H6** (locking uniforme del Sequencer + `volatile`) — raíz de "patrones parciales".
3. **C3** (buffer `_stateBuf` compartido) y **C4** (UAF en sampledata).
4. **C5/H1/H2** (carrera + muerte de MIDI tras STALL + UAF en desconexión).
5. **H10/M10/M13/M14** (overflows/OOB en parsing de entrada de usuario).
6. **H3/H4/H9/M1/M2** (robustez del protocolo SPI).
