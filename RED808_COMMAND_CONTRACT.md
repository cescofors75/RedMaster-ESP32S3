# RED808 Command Contract

Este es el contrato unico vigente entre BlueSlaveP4, BlueSlaveV2 y RedMaster ESP32-S3. Daisy Seed no participa en este contrato JSON: Daisy recibe comandos binarios por SPI real desde RedMaster.

## Diagnostico Daisy

`CMD_DIAG_PERF_STRESS` (`0xE5`) controla el modo de stress en Daisy: payload `0` lo apaga, `1` lo enciende y `2` reinicia metricas CPU/SPI/peak. `CMD_GET_STATUS` mantiene compatibles sus primeros 54 bytes y agrega CPU promedio/pico, modo stress, clipping master y drops SPI.

## Comandos Daisy Deprecados

`CMD_PAD_SCRATCH` (`0x78`) y `CMD_PAD_TURNTABLISM` (`0x79`) estan deprecados. RedMaster ESP32-S3 ya no los expone; Daisy los conserva como no-op de protocolo para que clientes antiguos no rompan el flujo SPI.

## Topologia

| Enlace | Transporte | Propietario | Formato |
|---|---|---|---|
| BlueSlaveV2 -> BlueSlaveP4 | USB CDC/UART | V2/P4 | Binario `uart_protocol.h` |
| BlueSlaveP4 -> RedMaster ESP32-S3 | WiFi UDP `8888` | P4 gateway | JSON UTF-8 |
| BlueSlaveV2 -> RedMaster ESP32-S3 | WiFi UDP `8888` opcional | V2 fallback | JSON UTF-8 |
| RedMaster ESP32-S3 -> Daisy Seed | SPI real | RedMaster | Binario `protocol.h` |

La fuente de verdad musical es RedMaster ESP32-S3. P4/S3 pueden editar o pedir estado, pero deben aceptar snapshots autoritativos del Master. En P4, las lecturas pesadas/criticas de UI se hacen por HTTP (`/api/getPattern`, `/api/p4State`) y los controles en tiempo real siguen por UDP JSON.

Para los FX master visibles en P4, la autoridad visual vigente es RedMaster por UDP. P4 puede enviar comandos y SlavePico puede enviar comandos, pero Delay/Reverb/Phaser se pintan en P4 desde `masterFx` y `state_sync.fx` recibidos del Master. La ruta UART/USB-C legacy del AUX/S3 no debe sobrescribir esos visuales salvo que se active explicitamente un modo legacy.

## Reglas JSON

- Campo obligatorio: `cmd`.
- Indices base cero: patrones `0..15`, tracks `0..15`, steps `0..63`.
- Volumen master/seq/live: `0..150`. Volumen por track: `0..150` en UDP; si cruza UART hacia S3 se limita a `0..100` por compatibilidad del paquete binario.
- Velocity: `1..127`; `0` se reserva para apagado cuando el comando lo soporte explicitamente.
- UDP maximo operativo: 4096 bytes para comandos/replies pequenos. P4 no debe depender de UDP para cargar la grid completa; usar HTTP para patron/estado inicial.

## Comandos P4/S3 -> Master

### Patrones

`selectPattern`

```json
{"cmd":"selectPattern","index":0}
```

Selecciona el patron actual en Master. P4 debe refrescar la grid con `GET /api/getPattern?index=N` tras seleccionar.

`get_pattern`

```json
{"cmd":"get_pattern","pattern":0}
```

Solicita el patron indicado. Si `pattern` falta, el Master responde con el actual.

Para P4, el camino preferido es HTTP:

```http
GET http://192.168.4.1/api/getPattern?index=0
GET http://192.168.4.1/api/p4State
```

`setStep`

```json
{"cmd":"setStep","pattern":0,"track":0,"step":0,"active":true,"noteLen":1,"silent":false}
```

`pattern` es opcional; sin el se escribe el patron actual. `silent:true` evita broadcasts WebSocket durante importaciones masivas.

`setStepVelocity`

```json
{"cmd":"setStepVelocity","pattern":0,"track":0,"step":0,"velocity":110,"silent":false}
```

Acepta steps `0..63`.

### Mixer

`mute`

```json
{"cmd":"mute","track":0,"value":true}
```

`solo`

```json
{"cmd":"solo","track":0,"value":true}
```

Alias aceptado por Master: `setTrackSolo`.

`setTrackVolume`

```json
{"cmd":"setTrackVolume","track":0,"volume":100}
```

`getTrackVolumes`

```json
{"cmd":"getTrackVolumes"}
```

### FX Master

Comandos canonicos:

```json
{"cmd":"setFilter","type":1}
{"cmd":"setFilterCutoff","value":12000}
{"cmd":"setFilterResonance","value":2.4}
{"cmd":"setDistortion","value":0.35}
{"cmd":"setBitCrush","value":12}
{"cmd":"setSampleRate","value":24000}
{"cmd":"setDelayActive","value":true}
{"cmd":"setDelayMix","value":45}
{"cmd":"setReverbActive","value":true}
{"cmd":"setReverbMix","value":0.35}
{"cmd":"setChorusActive","value":true}
{"cmd":"setChorusMix","value":0.30}
{"cmd":"setTremoloActive","value":true}
{"cmd":"setTremoloRate","value":4.0}
{"cmd":"setTremoloDepth","value":0.45}
{"cmd":"setLimiterActive","value":true}
{"cmd":"setCompressorActive","value":true}
{"cmd":"setCompressorThreshold","value":-18.0}
{"cmd":"setCompressorRatio","value":4.0}
```

Los campos `value` de mix pueden llegar como `0..1` o `0..100`; Master normaliza segun el comando existente.

P4 FX LAB muestra seis macros soportados por Master/Daisy:

| P4 | Transporte | Comandos principales | Rango recomendado |
|---|---|---|---|
| FLANGE | UDP | `setFlangerActive/Rate/Depth/Feedback/Mix` | mix aprox. `0.10..0.52`, feedback aprox. `0.08..0.38` |
| DELAY | UDP | `setDelayActive/Time/Feedback/Mix`, `setDelayStereo` | `60..900 ms`, feedback hasta `0.70`, mix hasta `0.58` |
| REVERB | UDP | `setReverbActive/Feedback/LpFreq/Mix` | feedback `0.28..0.76`, mix hasta `0.48` |
| FOLD | UDP | `setWavefolderGain` | ganancia `1.0..3.25`, sin makeup |
| CRUSH | UDP | `setBitCrush`, `setSampleRate`, `setDistortion` | `16..8 bits`, SR aprox. `32k..9k`, dist suave hasta `0.18` |
| PHASER | UDP | `setPhaserActive/Rate/Depth/Feedback` | sweep aprox. `1..6.5 Hz`, feedback aprox. `0.08..0.42` |

Evitar en P4 como macros principales: resonancia de filtro alta, limiter/compresion como efecto audible, tremolo muy profundo, y distorsion con ganancia alta; pueden clipar o parecer inactivos en directo.

### FX Per-Track

```json
{"cmd":"setTrackReverbSend","track":0,"value":40}
{"cmd":"setTrackDelaySend","track":0,"value":25}
{"cmd":"setTrackChorusSend","track":0,"value":30}
{"cmd":"setTrackPan","track":0,"value":-20}
{"cmd":"setTrackEcho","track":0,"active":true,"time":120,"feedback":35,"mix":30}
{"cmd":"setTrackFlanger","track":0,"active":true,"rate":60,"depth":50,"feedback":25}
{"cmd":"setTrackCompressor","track":0,"active":true,"threshold":-20,"ratio":4}
```

## Comandos Master -> P4/S3

### `pattern_sync`

```json
{
  "cmd":"pattern_sync",
  "pattern":0,
  "stepCount":64,
  "data":[[1,0,0,0],[0,0,1,0]]
}
```

`data` contiene 16 arrays de track. El Master puede enviar 64 steps; P4 renderiza/aplica su vista actual de 16 steps mientras S3 conserva hasta 64.

### `state_sync`

Snapshot compacto del estado autoritativo del Master hacia P4/S3.

```json
{
  "cmd":"state_sync",
  "pattern":0,
  "playing":false,
  "tempo":120.0,
  "stepCount":64,
  "masterVolume":100,
  "sequencerVolume":100,
  "liveVolume":100,
  "mute":[false],
  "solo":[false],
  "trackVolumes":[100],
  "fx":{
    "filterType":1,
    "delayActive":true,
    "delayMix":0.45,
    "reverbActive":false,
    "reverbMix":0.35,
    "phaserActive":true,
    "phaserDepth":0.50,
    "chorusActive":false,
    "trackReverbSend":[0],
    "trackDelaySend":[0],
    "trackChorusSend":[0],
    "trackEcho":[false],
    "trackFlanger":[false],
    "trackCompressor":[false]
  },
  "kit":"RED 808 KARZ",
  "samples":[{"pad":0,"loaded":true,"name":"BD.WAV"}]
}
```

El Master envia `state_sync` al saludar (`hello`), al recibir `get_state`, periodicamente a clientes UDP vivos y tras cambios de patron/mixer/FX/kit relevantes.

### `masterFx`

Evento incremental de baja latencia desde Master hacia P4/S3 para reflejar cambios de FX sin esperar al siguiente snapshot completo. Debe enviarse por UDP a clientes vivos y puede emitirse tambien por WebSocket para la UI web.

```json
{"type":"masterFx","param":"delayMix","value":45}
```

Campos usados por P4 FX LAB para los controles remotos de SlavePico:

| `param` | Tipo/rango aceptado | Destino visual P4 |
|---|---|---|
| `delayActive` | bool | `DELAY` ON/OFF |
| `delayMix` | `0..1` o `0..100` | `DELAY` valor |
| `reverbActive` | bool | `REVERB` ON/OFF |
| `reverbMix` | `0..1` o `0..100` | `REVERB` valor |
| `phaserActive` | bool | `PHASER` ON/OFF |
| `phaserDepth` | `0..1` o `0..100` | `PHASER` valor |

Regla de autoridad P4: los visuales de Delay/Reverb/Phaser se actualizan desde `masterFx` y, como respaldo de reconexion, desde `state_sync.fx`. Los mensajes binarios UART/USB-C legacy (`MSG_ENCODER`, `MSG_POT`, `MSG_FX`) no deben escribir esos mismos visuales cuando `P4_ENABLE_LEGACY_UART_FX_CONTROLS` esta desactivado.

## Samples desde P4 SD

El navegador SD de P4 lee la SD propia del P4 con `SD_MMC`. No debe pedir al S3 que cargue esos WAV.

Flujo correcto para WAV local de P4:

```http
POST http://192.168.4.1/api/uploadDaisy?pad=0
multipart/form-data file=<WAV de SD_MMC del P4>
```

RedMaster actua como puente/conversor: parsea WAV 16/24-bit mono/stereo, lo convierte a PCM16 mono y lo envia a Daisy por SPI con `CMD_SAMPLE_BEGIN`, `CMD_SAMPLE_DATA` y `CMD_SAMPLE_END`. Evitar guardar el WAV completo en PSRAM del Master en este flujo; Daisy es el destino de memoria de audio.

Para restaurar el sampler original de un pad sin reiniciar Daisy:

```http
POST http://192.168.4.1/api/unloadDaisy?pad=0
```

RedMaster envia `CMD_SAMPLE_UNLOAD` a Daisy; Daisy desmarca el sample RAM cargado en ese pad y vuelve a usar el sonido por defecto del motor/kit.

## Daisy Seed

Daisy no usa UART legacy para el enlace RedMaster. El enlace operativo es SPI real:

- Master: `RedMaster_ESP32S3/src/SPIMaster.*` y `RedMaster_ESP32S3/src/protocol.h`.
- Slave: `RedMaster_DaisySeed64MB/DaisySeed/main.cpp`.
- Paquete: header binario con magic/command/length/seq/crc y payload DSQ/audio.

No documentar ni implementar nuevos comandos hacia Daisy como JSON UDP o UART. Si un control viene de P4/S3, primero entra al contrato JSON del Master y luego RedMaster traduce a SPI.

## Melody / Piano Sync (v2.9)

Modelo master-autoritativo. El Master es la única fuente de verdad para el estado melody. Ningún slave envía directamente al otro.

### Topología

```
P4 piano → melodySetEngine/Octave/RecToggle/RecNote/Assign → Master
S3 melody → melodySetEngine/Octave/RecToggle/Assign/Clear → Master
Web UI   → mismos comandos                                 → Master
Master → melody_sync → P4 piano + S3 melody (broadcast a todos los udpClients)
```

### Comandos P4/S3/Web → Master

`melodySetEngine`
```json
{"cmd":"melodySetEngine","engine":3}
```
`engine`: 3=303, 4=WTosc, 5=SH101, 6=FM2. Master actualiza y broadcast.

`melodySetOctave`
```json
{"cmd":"melodySetOctave","octave":4}
```
`octave`: 1..7.

`melodyRecToggle`
```json
{"cmd":"melodyRecToggle","active":true,"engine":3,"octave":4}
```
Si `active=true` el Master limpia la grid y resetea el step cursor.

`melodyRecNote`
```json
{"cmd":"melodyRecNote","engine":3,"note":60}
```
Solo se procesa si `melodyRecActive=true` en el Master. Escribe en `melodyGrid[step][11-pc]` y avanza step.

`melodySetPad`
```json
{"cmd":"melodySetPad","pad":0}
```
Selecciona el pad destino del assign (0..15).

`melodyAssign`
```json
{"cmd":"melodyAssign","pad":0,"engine":3,"octave":4}
```
Master copia `melodyGrid` → `melodyPadGrid[pad]` y broadcast.

`melodyClear`
```json
{"cmd":"melodyClear"}
```
Limpia la grid activa en el Master y broadcast.

### Comando Master → P4/S3 (`melody_sync`)

Enviado: en cada cambio de estado melody, periodicamente cada 3 s, y en el hello inicial de cada slave.

```json
{
  "cmd": "melody_sync",
  "engine": 3,
  "octave": 4,
  "rec": 0,
  "step": 0,
  "pad": 0,
  "grid": [
    [0,0,0,0,0,0,0,0,0,0,0,0],
    ...
  ]
}
```

- `engine`: 3=303, 4=WT, 5=SH101, 6=FM2
- `octave`: 1..7
- `rec`: 0 o 1
- `step`: 0..15 (cursor de grabación actual)
- `pad`: 0..15 (pad destino actual)
- `grid`: array 16 columnas × 12 filas. Fila 0 = B más alto, fila 11 = C más bajo. `1` = celda activa.

**P4** aplica en `piano_apply_melody_sync()` bajo `lvgl_port_lock(50)`.  
**S3** aplica en `melody_apply_sync_payload()` bajo `lvgl_port_lock(200)`.