# RED808 Master — ESP32-S3 + Daisy

Firmware master de una drum machine RED808. El ESP32-S3 gestiona Wi‑Fi, la web,
MIDI USB, secuenciación, almacenamiento LittleFS y controles; una Daisy Seed
actúa como motor de audio y recibe órdenes por SPI.

## Arquitectura actual

```text
Navegador / móvil / control UDP
              │
      HTTP + WebSocket + UDP
              │
        ESP32-S3 N16R8
   Web · MIDI · Sequencer · FS
              │ SPI
              ▼
          Daisy Seed
   samples · synths · FX · audio
```

- ESP32-S3: 16 MB flash, 8 MB PSRAM.
- Core 1: tarea de secuenciador/audio-SPI de alta prioridad.
- Core 0: Wi‑Fi, web, MIDI, LEDs, uploads y tareas de sistema.
- Daisy: reproducción de samples, motores 808/909/505/303/WT/SH‑101/FM,
  mezcla, filtros y efectos.
- LittleFS: assets web comprimidos, MIDI, patterns y staging temporal de WAV.

El firmware ya no usa el antiguo `AudioEngine` I2S, PCM5102A, ST7789 ni
`DisplayManager`. La Daisy es la única salida de audio.

## Compilación

Requiere PlatformIO y Node.js. La primera vez, instala el minificador fijado
por `package-lock.json`:

```powershell
npm install
```

El firmware se compila con:

```powershell
pio run
```

Entorno por defecto: `redmaster-s3-r8n16` en [platformio.ini](platformio.ini).
La configuración de placa está en
[boards/redmaster_esp32s3_r8n16.json](boards/redmaster_esp32s3_r8n16.json).

Para flashear firmware y filesystem se puede usar:

```powershell
.\flash_esp32.ps1
```

El puerto configurado por defecto es `COM11`; ajústalo a la máquina real antes
de subir. El filesystem de producción es `data_gz`.

## Web

| Ruta | Función |
|---|---|
| `/` | Secuenciador y panel principal |
| `/mobile` | Interfaz táctil simplificada |
| `/patchbay` | Routing y edición avanzada de FX |
| `/multiview` | Vista multipanel |
| `/gesture` | Control gestual clásico |
| `/gesture-pro` | Performance gestual |
| `/adm` | Diagnóstico y administración |
| `/ws` | WebSocket de control/estado |

`data/web` es la fuente legible. `data_gz/web` contiene lo que se flashea. Antes
de `buildfs`/`uploadfs`, `tools/prepare_data_gz.py` crea bundles por pantalla,
extrae CSS/JS inline, minifica JS/CSS/HTML, añade hashes a las URLs, genera un
`build.id` para ETag y comprime los assets con gzip nivel 9. El proceso se
detiene con un error claro si falta `npm install`.

La app shell incluye Service Worker con precache selectivo y caché progresiva
cuando se abre desde HTTPS o desde un bridge en localhost. Los navegadores no
permiten registrarlo desde el AP directo `http://192.168.4.1` porque una IP
privada HTTP no es un contexto seguro; en ese modo siguen aplicándose ETag y
caché HTTP del firmware.

### Red y seguridad

Por defecto se crea el AP:

- SSID: `RED808`
- contraseña: `red808esp32`
- dirección habitual: `http://192.168.4.1`

`HOME_WIFI_SSID` vacío significa AP-only. Si se habilita STA en `src/main.cpp`,
la web exige autenticación HTTP Basic:

- usuario: `red808`
- contraseña: la misma configurada para el AP

Las peticiones de navegador y el handshake WebSocket también validan `Origin`
para impedir control cross-site desde otra página de la LAN.

## Samples y uploads

- Entrada: WAV PCM o WAVE_FORMAT_EXTENSIBLE PCM.
- Formatos admitidos: mono/estéreo, 16/24 bits.
- Tamaño máximo HTTP: 8 MB.
- El master convierte a mono 16-bit antes de enviar a la Daisy.
- Los chunks RIFF se validan contra el tamaño real; un chunk truncado o con
  overflow se rechaza.
- Solo se admite un upload de pad y un upload de clean track en recepción; una
  segunda petición recibe HTTP `409 upload_busy`.

Los uploads de pads se guardan primero en `/.pad-upload.tmp`. El callback HTTP
solo escribe el archivo y responde; `systemTask` lo procesa en bloques de hasta
256 frames, evitando bloquear AsyncTCP mientras la Daisy drena SPI.

## Secuenciador

- 128 patterns.
- 16 tracks × 16/32/64 steps.
- Velocity, note length, probability, ratchet y parameter locks.
- Song Chain de hasta 32 entradas con repeticiones.
- Melody polyphonic steps y selección de motor synth por track.
- Estado de transporte, loops, mute/volume y Song Chain sincronizado entre
  Core0 y Core1 mediante snapshots cortos; los callbacks SPI se ejecutan fuera
  de los locks.

## Protocolo y documentación hardware

- [protocol.h](src/protocol.h): estructuras y command IDs compartidos.
- [UDP_PROTOCOL_MASTER_SLAVE.md](UDP_PROTOCOL_MASTER_SLAVE.md): control UDP.
- [WEBSOCKET_MESSAGES_COMPLETE.md](WEBSOCKET_MESSAGES_COMPLETE.md): mensajes web.
- [DAISY_SLAVE_GUIDE.md](DAISY_SLAVE_GUIDE.md): integración con Daisy.
- [PINOUT.md](PINOUT.md): cableado vigente.

Los cambios incompatibles del framing SPI, magic bytes o checksum de samples
deben aplicarse simultáneamente en el firmware Daisy y en este master.

## Pruebas

Runner host sin hardware:

```powershell
node --test tests/run-tests.mjs
```

Incluye:

- fuzzing de chunks WAV, frames WebSocket grandes y tamaños con overflow;
- contrato comandos frontend ↔ handlers del firmware;
- Song Chain de 32 entradas;
- aislamiento de uploads simultáneos y ausencia de esperas en AsyncTCP;
- encoding UTF‑8, IDs HTML y assets básicos;
- regresión del menú táctil.

La validación completa antes de flashear es:

```powershell
node --test tests/run-tests.mjs
pio run
```

Las pruebas host no sustituyen una sesión con la Daisy conectada para audio,
timing SPI, hot-unplug MIDI y carga sostenida Wi‑Fi.

## Estructura relevante

```text
src/
  main.cpp                 tareas y arranque
  Sequencer.*              patterns, transporte y Song Chain
  SPIMaster.*              protocolo y colas SPI
  MIDIController.*         USB MIDI host
  SampleManager.*          samples y WAV
  WebInterface.*           HTTP, WS y UDP
  WebSecurity.h            origen, autenticación y ACK/error
  WebUploadPipeline.h      estado y límites de upload
  web/WebUploadPipeline.inc pumps y endpoints de upload
data/web/
  app.js                   núcleo de la interfaz
  midi-ui.js               dashboard y mapping MIDI
  sd-browser.js            navegador SD de Daisy
  style.css                estilos base
  daisy-controls.css       SD y controles de motor Daisy
  responsive.css           correcciones responsive compartidas
data_gz/web/               assets flasheados comprimidos
tests/run-tests.mjs        fuzzing y contratos host
tools/prepare_data_gz.py   pipeline web → LittleFS
```

## Licencia

MIT — Cesco, 2025–2026.
