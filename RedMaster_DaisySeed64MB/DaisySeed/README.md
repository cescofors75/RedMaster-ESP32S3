# RED808 DrumMachine — Daisy Seed Slave

Drum machine de **24 pads** con efectos completos.  
SPI1 slave — protocolo RED808 compatible con **ESP32-S3** master.

## Hardware

| Componente | Descripción |
|---|---|
| **Daisy Seed** | Electro-Smith (STM32H750 + 64MB SDRAM + codec integrado) |
| **Micro SD** | Módulo SPI de 6 pines, 3.3 V |
| **ESP32-S3 N16R8** | Master SPI + Web UI + Sequencer |

## Conexiones

### SPI1: ESP32-S3 (Master) ↔ Daisy Seed (Slave)

| Señal SPI | ESP32-S3 (ejemplo) | Daisy Pin | STM32H750 |
|-----------|---------------------|-----------|-----------|
| SCK       | GPIO 12             | **D8**    | PG11 (SPI1_SCK) |
| MOSI      | GPIO 11             | **D10**   | PB5 (SPI1_MOSI / RX slave) |
| MISO      | GPIO 13             | **D9**    | PB4 (SPI1_MISO / TX slave) |
| CS/NSS    | GPIO 10             | **D7**    | PG10 (SPI1_NSS) |
| GND       | GND                 | GND       | — |

> En ESP32-S3 puedes usar otros GPIO si configuras el bus SPI por software con los mismos roles de señal.

> **SPI Mode 0**, MSB first, 8-bit, Hardware NSS.  
> Bring-up @ 2 MHz → estable @ 20 MHz.

### SD Card (SPI3 master)

| SD Pin | Daisy Pin |
|--------|-----------|
| CS     | D0 / PB12 |
| MISO   | D1 / PC11 |
| SCK    | D2 / PC10 |
| MOSI   | D6 / PC12 |
| VCC    | 3V3       |
| GND    | GND       |

### Audio Output
Codec integrado en la Daisy Seed → salida por **jack de 3.5mm**.

## Especificaciones

| Parámetro | Valor |
|-----------|-------|
| Sample rate | **48000 Hz** |
| Block size | **128 samples** |
| Max pads | **24** (16 seq + 8 XTRA) |
| Max voces | **32** polyphonic |
| DSP | Float 32-bit |
| SDRAM para samples | 24 × 96000 int16 ≈ 4.4 MB |
| Max duración sample | ~2.17 s per pad (SPI) |
| SD card loading | ~0.25 s para 16 samples |

## Protocolo RED808

Paquete SPI: **8 bytes header + payload**

```
[0xA5] [CMD] [LEN_L] [LEN_H] [SEQ_L] [SEQ_H] [CRC_L] [CRC_H] [PAYLOAD...]
```

- Magic: `0xA5` (CMD) / `0x5A` (RESP)
- CRC16-Modbus sobre payload
- Respuesta NUNCA desde ISR — flag `pendingResponse` + main loop

### Comandos implementados

| Grupo | CMDs | Estado |
|-------|------|--------|
| Triggers | 0x01-0x05 | ✅ |
| Volume | 0x10-0x14 | ✅ |
| Global Filter | 0x20-0x26 | ✅ |
| Delay | 0x30-0x33 | ✅ |
| Phaser | 0x34-0x37 | ✅ |
| Flanger | 0x38-0x3C | ✅ |
| Compressor | 0x3D-0x42 | ✅ |
| Reverb | 0x43-0x46 | ✅ |
| Chorus | 0x47-0x4A | ✅ |
| Tremolo | 0x4B-0x4D | ✅ |
| Wavefolder/Limiter | 0x4E-0x4F | ✅ |
| Track FX | 0x50-0x58 | ✅ |
| Track Sends/Pan/Mute | 0x59-0x65 | ✅ |
| Pad FX | 0x70-0x7A | ✅ |
| Sidechain | 0x90-0x91 | ✅ |
| Sample Transfer | 0xA0-0xA4 | ✅ |
| SD Card | 0xB0-0xB9 | ✅ (core) |
| Status/Peaks/Ping | 0xE0-0xEF | ✅ |
| Bulk | 0xF0-0xF1 | ✅ |
| Synth Engines | 0xC0-0xC7 | ✅ (incl. RayDrone, engine 9) |

## Setup del toolchain

```bash
# 1. Instalar arm-none-eabi-gcc
#    https://developer.arm.com/downloads/-/gnu-rm

# 2. Clonar libDaisy y DaisySP
cd DaisySeed/
git clone https://github.com/electro-smith/libDaisy.git libdaisy
git clone https://github.com/electro-smith/DaisySP.git DaisySP

# 3. Compilar libDaisy
cd libdaisy && make -j4 && cd ..

# 4. Compilar DaisySP
cd DaisySP && make -j4 && cd ..

# 5. Compilar el firmware
make -j4
```

## Flash

### DFU (USB)
1. Conecta la Daisy por USB
2. Mantén **BOOT** y pulsa **RESET** → modo DFU
3. `make program-dfu`

### ST-Link
```bash
make program
```

## Estructura SD Card (microSD FAT32, ≤32 GB)

```
/data/
  ├── RED 808 KARZ/          ← Kit por defecto (LIVE PADS 0-15 al arrancar)
  │   ├── 808 BD 3-1.wav       Mapeo automático BD→pad0, SD→1, HH→2, etc.
  │   ├── 808 SD 1-5.wav       Duplicados (ej. 2x SD, 3x HC) van a pads libres.
  │   ├── 808 HH.wav
  │   ├── 808 OH 1.wav
  │   ├── 808 CY 3-1.wav
  │   ├── 808 CP.wav
  │   ├── 808 RS.wav
  │   ├── 808 COW.wav
  │   └── ... (16 wavs)
  │
  ├── BD/                     ← Familias de instrumentos (selección desde Master)
  │   ├── BD0000.WAV            Master envía CMD_SD_LIST_FILES("BD") para listar
  │   ├── BD2525.WAV            Master envía CMD_SD_LOAD_SAMPLE("BD","BD2525.WAV",0)
  │   └── ... (25 variantes)
  ├── SD/                     ← 25 variantes de snare
  ├── CH/                     ← 1 closed hihat
  ├── OH/                     ← 5 variantes open hihat
  ├── CY/                     ← 25 variantes cymbal
  ├── CP/  RS/  CB/           ← 1 variante cada uno
  ├── LT/  MT/  HT/           ← 5 variantes toms
  ├── MA/  CL/                ← 1 variante cada uno
  ├── HC/  MC/  LC/           ← 5 variantes congas
  │
  └── xtra/                   ← XTRA PADS (pads 16-23, cargados al arrancar)
      ├── Alesis-Fusion-Bass-C3.wav
      ├── dre-yeah.wav
      ├── fast114bpm.wav
      └── ragefx.wav
```

### Flujo de carga

1. **Boot**: `AutoLoadFromSD()` carga `RED 808 KARZ/` en LIVE PADS 0-15 (mapeo inteligente por nombre de instrumento) y `xtra/` en XTRA PADS 16-23.
2. **Master cambia kit**: `CMD_SD_KIT_LIST` (0xB5) lista kits → `CMD_SD_LOAD_KIT` (0xB4) carga uno.
3. **Master cambia sample individual**: `CMD_SD_LIST_FILES` (0xB1) lista .wav de una familia → `CMD_SD_LOAD_SAMPLE` (0xB3) carga uno en un pad concreto.
4. **Master consulta info**: `CMD_SD_LIST_FOLDERS` (0xB0) lista todas las carpetas, `CMD_SD_FILE_INFO` (0xB2) devuelve tamaño/sr/bps/duración.

**Formatos WAV soportados:** PCM 8/16/24-bit, mono o estéreo, 1–384 kHz. El motor conserva el sample rate de origen y remuestrea linealmente a 48 kHz durante la reproducción.

### TR-505 PCM

El preset de sintetizador **505 / preset 5** enlaza los pads canónicos 0–15 cargados actualmente como banco PCM de la TR-505. Cada slot ausente conserva la voz procedural como fallback. Los presets 0–4 siguen usando el motor procedural y no confunden el kit 808 por defecto con una ROM 505.

### TR-909 híbrida

El preset de sintetizador **909 / preset 5** mantiene kick, snare, clap, toms y rimshot procedurales, y enlaza PCM para los instrumentos digitales de la 909: closed hat desde pad 2, open hat desde pad 3, crash desde pad 4 y ride desde pad 7. Conserva choke CH→OH, resampling según el WAV y fallback procedural individual si falta un archivo.

## Módulos DaisySP utilizados

| Efecto | Módulo |
|--------|--------|
| Delay | `DelayLine<float, 88200>` |
| Reverb | `ReverbSc` |
| Chorus | `Chorus` |
| Tremolo | `Tremolo` |
| Compressor | `Compressor` |
| Wavefolder | `Fold` |
| Phaser | `Phaser` |
| Filters | `Biquad` custom (LP/HP/BP/Notch/Peak/Shelf) |

## Motor RayDrone (engine 9)

Port del motor granular [RayDrone](https://github.com/cescofors75/RayDrone)
(Rust/WASM) al firmware, ampliado para drones y shimmer.

El sonido no se sintetiza desde un oscilador: se **renderiza**. Se lanzan N
rayos por segundo hacia un punto de foco dentro de una fuente, y cada rayo lee
un grano con dispersión triangular alrededor de ese foco — la profundidad de
campo de una lente. La convergencia estadística de la nube **es** el drone.

- Apertura estrecha → lectura casi tonal, el sample se reconoce.
- Apertura amplia → desenfoque profundo, textura atmosférica.

### Qué añade sobre el motor WASM original

| | WASM original | Este port |
|---|---|---|
| **Materiales** | Un filtro de un polo por grano (5 materiales) | Ese shaper **más** un banco modal de 8 resonadores estéreo afinado a la nota, con ratios y Q del cuerpo físico real. 10 materiales |
| **Shimmer** | `OCT`: probabilidad de que un grano lea a 2× | Lazo de difusión (4 allpass + 2 comb por canal) con **dos pitch-shifters, +12 y +19, dentro de la realimentación**: la octava se re-inyecta y la cola asciende sola |
| **Variación** | Barrido triangular de un foco | 4 LFOs a frecuencias mutuamente irracionales + motor de mutación con glide |
| **Ventana** | Hann fija | Tres tablas (percusiva / Hann / meseta) con crossfade continuo |
| **Fuente** | Requiere un sample | Si no hay pad cargado, genera su propia fuente armónica afinada (A1, 12 parciales) |

Se conserva del original: dispersión triangular inversa, muestreo QMC (áureo /
estratificado / random), **constelación recursiva de focos** (semillas
inmortales que engendran hijos auto-similares), rebotes de grano, voicings en
entonación justa, aberración cromática, rayos inteligentes por importancia y
autoevolución del foco realimentada por la envolvente.

### Materiales

`METAL` (barra libre-libre) · `WOOD` (barra de marimba) · `GLASS` (copa, Q alto)
· `WATER` (cuasi-armónico a la deriva) · `PLASMA` (inarmónico + no linealidad) ·
`STONE` (denso, Q bajo) · `SKIN` (membrana, ceros de Bessel) · `STRING`
(armónico puro) · `ICE` (agudo, Q extremo).

### Presets

`0` Glass Cathedral · `1` Iron Bloom · `2` Ice Field · `3` Wood Chapel ·
`4` Deep Membrane · `5` Bowed String · `6` Plasma Storm · `7` Stone Well

Igualados de nivel entre sí (RMS 0,14–0,20 medido).

### Control

Notas por `CMD_SYNTH_NOTE_ON_EX` / `CMD_SYNTH_TRIGGER` con `engine=9`. Es un
drone sostenido: `NoteOff` deja de sembrar granos y la nube y la cola se apagan
solas.

Parámetros por `CMD_SYNTH_PARAM` con `engine=9`; los ids 0–32 están en
`synth/raydrone.h`. El id **100** elige la escena: `-1` (o un pad sin cargar)
usa la fuente interna, `0..23` engancha ese pad del pool SDRAM.

Presets por `CMD_SYNTH_PRESET` con `engine=9`, preset `0..7`.

### Coste y límites

Es el motor más caro del firmware: el coste crece con los granos vivos. Medido
en host, el coste fijo es ~0,21 µs/muestra y cada grano añade ~11 ns. El
firmware limita la nube a **48 granos** y la baja a **20** cuando el bus entra
en modo shed de CPU; el motor sigue sonando, solo adelgaza la nube.

La instancia ocupa ~334 KB y vive en SDRAM. No tiene inicializadores de miembro
a propósito: así es trivialmente construible, no genera un constructor que
correría antes de que `hw.Init()` levante la SDRAM, y `Init()` deja fijado todo
el estado (la SDRAM de la Daisy no se pone a cero al arrancar).

Es el único motor estéreo. Como la cadena de FX de track es mono, se descompone
en M/S: el medio pasa por los FX del track y el lado va directo al bus, así
conserva la anchura sin duplicar el coste de los efectos.

## Demo autónoma para presentación

Compila una versión que arranca con un arreglo original de ocho escenas y 32
compases:

```bash
make RED808_STARTUP_SHOWCASE_DEMO=1 -j4
```

La demo usa el secuenciador sample-accurate de Daisy: los samples cargados desde
el kit SD forman la batería principal, dos capas 808/909 aportan detalles y un
bajo SH-101 con pad wavetable aparecen de forma contenida. Si no hay samples,
los motores generados actúan como fallback. El arreglo mantiene una identidad
única (AIR, PULSE, HOOK, PRESSURE, SPACE, RETURN, PEAK y RELEASE), se repite
continuamente y bloquea comandos de transporte/notas del Master para impedir
que dos secuenciadores suenen simultáneamente. Las cargas SD, diagnósticos y
consultas continúan disponibles.
El antiguo `RED808_STARTUP_808_SELF_TEST=1` se mantiene como diagnóstico de
instrumentos y no debe usarse para una presentación.

## Debug USB

Monitor serie (115200 baud) muestra:
- Carga de samples al arrancar
- Estado SD card
- SPI3 ready
- "RED808 DRUM MACHINE READY"

## Criterio de éxito

1. **PING OK** → ESP32 muestra `STM32 connected! RTT: ~300us`
2. **Samples cargados** → 16/16 via SPI o SD
3. **Audio** → Triggers suenan a 44100 Hz estéreo
4. **FX** → Delay (0x30), Reverb (0x43), Comp (0x3D) audibles
5. **SD** → Kit list + load desde web UI
