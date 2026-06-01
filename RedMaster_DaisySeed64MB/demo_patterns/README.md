# The Bells (Jeff Mills) → 19 escenas para tocar en el master

Banco de **19 patrones-escena** extraído de `DaisySeed/demo_bells.cpp` (el demo
autónomo "The Bells" de Jeff Mills) y llevado al secuenciador del master para
**interpretarlo en directo**: cambiando de patrón recorres build-up, grooves,
breakdowns y solos (el "truco Mills" de mutear/desmutear bucles).

Funciona end-to-end en los tres equipos: **Daisy** (audio), **ESP32-S3**
(master) y **ESP32-P4** (panel táctil).

## Upgrade de sonido (3 etapas, todas aplicadas)

Para que suene como `demo_bells.cpp` (no solo "las mismas notas"):

| Etapa | Qué | Dónde | Estado |
|---|---|---|---|
| **1 · Timbres** | preset campana ring-mod, kick 909 Mills (drive max), bajo 303 chuffy | Daisy `main.cpp` (presets FM2Op 2 / 909 3 / 303 4) | ✅ |
| **2 · Espacio** | reverb larga (fb 0.82, LP 8500) + delay del clap, sends por track | S3 `WebInterface.cpp` lee bloque `fx` del JSON → comandos SPI | ✅ |
| **3 · Polifonía** | FM2Op mono → **6 voces** (las campanas se solapan, cola 2.6 s) | Daisy `synth/fm2op.h` (clase `Poly`) + `main.cpp` | ✅ |

> **Expectativa honesta:** queda un tributo **muy reconocible (~85-90%)**, no un
> clon bit a bit. El `demo_bells.cpp` standalone tiene su propia mezcla/soft-clip
> exactos; el firmware principal usa otra cadena de FX. El carácter (campana
> metálica, kick sucio, cola, solapamiento) sí se recupera.

> **A vigilar al testear:** (a) **CPU** con 6 voces FM + 909 + 303 — si hay
> underruns, bajar `FM2Op::PolyT<6>` a 4. (b) El S3 manda `NoteOff` entre pasos:
> con la clase `Poly` eso pasa las voces a *release* (no las corta en seco), así
> que la cola se mantiene; si cortara demasiado, se afina el release.

```
P4 7" (UI) ──UDP──► ESP32-S3 (master, secuenciador) ──SPI──► Daisy (audio)
                          ▲ WebSocket (web propia)
```

## Contenido de la carpeta

| Fichero | Qué es | Dónde se aplica |
|---|---|---|
| `19_temas_demo_daisy.json` | Banco de 19 patrones (ritmo + melodía 303 + preset por track) | `data/patterns/` del **S3** |
| `generate_demo_patterns.py` | Generador reproducible del JSON | — |
| `s3_presets_melody.patch` | Lee melodía/engine/preset y los aplica al cambiar de patrón | repo **RedMaster-ESP32S3** |
| `p4_19_patterns.patch` | Sube el tope de patrones 16→19 | repo **BlueSlaveP4** |
| (Daisy) `DSQ_PATTERNS 16→19` | Ya commiteado en este repo | **este repo** |

## Qué hace cada capa (estado tras los parches)

| | Ritmos | Melodías | Presets | Cambio |
|---|---|---|---|---|
| **Daisy** | ✅ | ✅ `0xC7` | ✅ `0xC6` | `DSQ_PATTERNS` 16→19 (este repo) |
| **S3** | ✅ | ✅ loader lee `notes`/`flags` | ✅ preset **por patrón** | `s3_presets_melody.patch` |
| **P4** | ✅ | ✅ | ✅ | `p4_19_patterns.patch` |

## Las 19 escenas (132 BPM, 16 pasos)

Cada escena = qué elementos suenan. Tocas el track avanzando de escena.

| # | escena | elementos | melodía |
|---|---|---|---|
| 0 | KICK | kick | — |
| 1 | + RIDE | kick, ride | — |
| 2 | + OPEN HAT | kick, ride, open-hat | — |
| 3 | + BELL HI | + campanas (voz alta) | ✅ |
| 4 | + CLAP+BELL LO | + clap + campana baja | ✅ |
| 5 | FULL | todo + bajo 303 | ✅ |
| 6–8 | GROOVE A/B/C | full, con bell-lo mute/unmute (truco Mills) | ✅ |
| 9 | BREAKDOWN | kick + campanas + bajo | ✅ |
| 10 | REBUILD | kick, ride, OH, campana hi, bajo | ✅ |
| 11 | PEAK | todo | ✅ |
| 12 | BELLS+BASS | campanas + bajo (sin batería) | ✅ |
| 13 | KICK+BASS | kick + bajo | ✅ |
| 14 | BELLS SOLO | solo campanas | ✅ |
| 15 | DRUMS SOLO | kick, ride, OH, clap | — |
| 16 | BASS SOLO | solo 303 | ✅ |
| 17 | RIDE+BELLS | ride + campanas | ✅ |
| 18 | CLIMAX | todo, máxima energía | ✅ |

**Motores (engine por patrón):**
- **909** (engine 1, preset Industrial) → KICK 4×4, OPEN HAT offbeats, RIDE corcheas, CLAP backbeat.
- **FM2Op** (engine 6, preset Bell) → BELLS, motif La menor en **2 voces** (hi+lo
  suenan juntas en pasos 0 y 11) vía `noteVoices`.
- **303** (engine 3, preset Acid) → BASS A1 en semicorcheas, acento por tiempo, slides.

`songChain` interpreta el tema entero: build-up → grooves → breakdown → climax.

> **Polifonía de campanas:** se usa el campo `noteVoices` (2 voces/paso). El
> loader del S3 lo lee (parche). Verifica en hardware que el motor FM2Op suena
> polifónico; si fuera monofónico, en los pasos 0/11 sonaría solo una campana.

## Esquema JSON (extendido)

```jsonc
{
  "name": "...", "tempo": 124, "stepCount": 16, "selectPattern": 0,
  "trackEngines": [0,1,2,1,1,0,2,3,0,0,0,-1,-1,-1,-1,-1],  // mapa de referencia
  "patterns": [
    { "slot": 0, "name": "TECHNO FULL", "tracks": [
        { "track": 0, "engine": 0, "preset": 2, "steps": [...16], "velocities": [...16] },
        { "track": 7, "engine": 3, "preset": 0, "steps": [...], "velocities": [...],
          "notes": [36,36,43,0,...],   // MIDI por paso, 0 = silencio
          "flags": [1,0,0,0,...] }     // bit0=accent, bit1=slide
    ]}
  ],
  "songChain": [ {"pattern":0,"repeats":4}, ... ]
}
```

Campos nuevos respecto a `10_temas_referencia_808.json`:
`engine`, `preset`, `notes`, `flags` por track.

## Presets por patrón

Cada track recuerda su **engine** + **preset de fábrica**; el S3 los reaplica al
activar el patrón (`CMD_SYNTH_PRESET 0xC6` + `dsqSetTrackEngine`). Presets por
estilo (ver generador): Techno→808 *Techno*/909 *Techno*/303 *Acid*; Electro→909
*HousePound*/303 *Squelch*; Ambient→909 *Industrial*/303 *Sub Bass*.

> ⚠️ **El engine global se sobreescribe al cambiar de patrón.** `gTrackSynthEngine[]`
> del S3 es global; el recall por patrón lo actualiza en cada switch. Es el
> comportamiento deseado para este banco.

## ⚠️ Por qué solo se veían 6 patrones (resuelto)

Dos cosas tapaban el banco, ya arregladas en `s3_presets_melody.patch`:

1. **Autoload desactivado.** El S3 arrancaba con 6 patrones *inline* hardcodeados
   (HIP HOP, TECHNO, DnB, BREAK, HOUSE, TRAP); el autoload de bancos estaba
   apagado a propósito. El parche lo **activa** → al boot carga
   `19_temas_demo_daisy.json` (con fallback a los inline si el JSON no está).
2. **Nombres del selector hardcodeados.** La web fijaba el nº de patrones en
   `PATTERN_NAMES` (6) → `totalPatterns = max(len, 6)` = 6. El parche pone los
   **19 nombres del demo** en `data/web/app.js`.

## Instalación (3 pasos)

```bash
# 1) S3: parche (autoload + nombres web + melodia + presets) y JSON
cd RedMaster-ESP32S3 && git apply /ruta/s3_presets_melody.patch
copy /ruta/19_temas_demo_daisy.json data/patterns/
pio run -t upload          # firmware
pio run -t uploadfs        # JSON + web nuevo  ← IMPRESCINDIBLE

# 2) P4: bump de patrones 16->19
cd BlueSlaveP4 && git apply /ruta/p4_19_patterns.patch
pio run -t upload

# 3) Daisy: ya en este repo (DSQ_PATTERNS=19). build + flash.
```

Al reiniciar el master, el log mostrará `Banco demo '19_temas_demo_daisy.json'
cargado (19 patrones)` y la web/P4 enseñarán los 19 temas del demo.

### Carga manual (sin recompilar)

Con el JSON ya en LittleFS puedes cargarlo por URL, aunque la web seguirá
mostrando los 6 nombres viejos si no aplicas el cambio de `app.js`:
```
http://<IP-master>/api/patternBanks
http://<IP-master>/api/patternBank/load?file=19_temas_demo_daisy.json
```

## Detalle de los parches del S3

`s3_presets_melody.patch` toca 5 ficheros (≈98 líneas):

- **`Sequencer.h`**: `PatternData` gana `trackEngine[][]` y `trackPreset[][]`; 4 setters/getters.
- **`Sequencer.cpp`**: init a `-1`/`0` en constructor y `clearPattern`; implementación de los métodos.
- **`WebInterface.cpp`**: `loadPatternBankFromFs` hace una 2ª pasada que lee
  `engine`/`preset`/`notes`/`flags` por track (tras `setPatternBulk`, que limpia notas).
- **`main.cpp`**: **autoload del banco al boot** + al activar patrón reaplica engine + preset.
- **`data/web/app.js`**: `PATTERN_NAMES` con los 19 nombres del demo.

## ⚠️ Notas de integración (verificar en hardware)

1. **Sin compilar contra el build real.** Los parches se han escrito sobre el
   HEAD clonado pero no se han compilado/flasheado. Revisa que compilan en tu
   toolchain antes de subir.
2. **Doble disparo melódico.** La melodía 303 la dispara el `stepCallback` del S3
   (`synthNoteOnEx`). Si la Daisy también dispara ese track desde su secuenciador
   interno, podría sonar doble. Es el **mismo flujo** que el "melodyAssign" del P4
   ya en producción, así que debería comportarse igual — pero conviene oírlo.
3. **P4 UI.** El bump deja seleccionar P01–P19; mostrar el preset activo por
   patrón en pantalla es una mejora opcional aún no incluida.

## Compatibilidad sin parchear el S3

El JSON sigue cargando sin el parche: sonarían solo las baterías
(`steps`+`velocities`); melodía y presets se ignorarían sin error.
