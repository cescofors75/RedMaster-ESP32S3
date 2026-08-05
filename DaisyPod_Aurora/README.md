# Aurora v4.1 — DaisySP FX Instrument para Daisy Pod

Aurora v4.1 es un multiefecto estereo para tocar, no una coleccion de filtros
estaticos. Cada color activa una cadena DSP completa que procesa **Line In en
tiempo real**. Freeze es opcional: al pulsar Button 1, los ultimos cuatro pulsos
entran en esa misma cadena y se convierten en una segunda capa.

Al arrancar muestra cuatro colores y después hace **dos pulsos blancos largos**.
Esa firma identifica la revisión estable v4.1.

## Dos knobs

| Control | Funcion |
|---|---|
| Knob 1 — CHARACTER | Transforma varios parametros musicales de la escena |
| Knob 2 — INTENSITY | 0 = limpio; 50 % = efecto claro; 100 % = inmersivo |
| Button 1 — FREEZE | Congela/libera los ultimos cuatro pulsos |
| Button 2 — MORPH | Sin Freeze: tap tempo. Con Freeze: mantener para renovar la memoria |
| Encoder — giro | Selecciona una de cuatro cadenas FX |
| Encoder — clic | Bypass con fundido |

No hay menus, doble clic ni pulsacion larga.

## Las cuatro cadenas FX

1. **Ocean — azul**
   - Chorus estereo con LFO diferentes en izquierda y derecha.
   - Moog Ladder resonante.
   - Reverb oscura y ancha.
   - CHARACTER viaja de ola profunda/resonante a agua brillante y abierta.

2. **Pulse — rosa**
   - Phaser DaisySP de cuatro etapas sobre la componente central estéreo.
   - Conserva la componente lateral para no colapsar la imagen.
   - Filtro SVF que mezcla notch y band-pass con drive interno.
   - CHARACTER barre desde pulso lento y grave hasta barrido nervioso y luminoso.

3. **Halo — ambar**
   - Dos flangers DaisySP con velocidades ligeramente distintas.
   - Filtro SVF high-pass/notch de aire.
   - Reverb larga dedicada.
   - CHARACTER recorre desde una modulación lenta y ancha hasta un barrido aéreo.

4. **Ritual — violeta**
   - Overdrive compensado por canal.
   - Moog Ladder resonante despues de la saturacion.
   - Con Freeze añade granos estereo moderados.
   - CHARACTER pasa de humo oscuro y resonante a distorsion abierta y brillante.

INTENSITY conserva un 22 % de señal directa al maximo. Todos los cambios de
escena tienen fundido; el limitador suave actua solo cerca del clipping.

## Prueba inmediata

1. CHARACTER a las 12 h e INTENSITY a cero: comprueba el sonido limpio.
2. Sube INTENSITY a las 12 h. Todavia **sin Freeze**, gira el encoder.
3. En azul debe abrirse un chorus; en rosa debe moverse el phaser; en ambar debe
   aparecer un flanger estéreo con cola; en violeta debe entrar saturación filtrada.
4. Barre CHARACTER lentamente en cada color.
5. Toca una frase y pulsa FREEZE para añadir memoria; vuelve a pulsarlo para salir.
6. Pulsa el encoder para comparar efecto y bypass.

## LEDs

- LED 1: color de escena; su brillo sigue CHARACTER.
- LED 2 verde: brillo de INTENSITY; pequeños pulsos rojos indican entrada.
- LED 2 ambar: Freeze activo; su brillo sigue INTENSITY.
- LED 2 blanco: Morph activo.
- Ambos rojo oscuro: bypass.

## Compilar

Aurora usa las copias locales de libDaisy, DaisySP y DaisySP-LGPL:

```powershell
cd C:\Users\cesco\Documents\Arduino\XboxBLE\DaisyPod_Aurora
.\build.ps1 -Clean
```

Genera `build/Aurora.bin` para `BOOT_SRAM`. El perfil directo es:

```powershell
.\build.ps1 -Direct -Clean
```

Antes de evaluar los efectos se puede compilar/cargar el passthrough de
diagnóstico, documentado en `DIAGNOSTIC.md`:

```powershell
.\build.ps1 -Diagnostic -Direct -Clean
.\flash.ps1 -Diagnostic -Direct
```

## Cargar

Para una placa que ya usa la aplicación directa:

```powershell
.\flash.ps1 -Direct
```

Cuando el script espere, mantén el pequeño botón **BOOT** de la Daisy Seed,
pulsa y suelta **RESET**, y después suelta BOOT. El script verifica `@Internal
Flash` antes de escribir en `0x08000000`.

El modo directo reemplaza un posible bootloader Daisy. Para una placa con el
bootloader Daisy instalado usa `.\flash.ps1` sin `-Direct`; ese perfil escribe
la aplicación `BOOT_SRAM` en QSPI.

## Audio y memoria

- Entrada/salida estereo a 48 kHz, bloques de 48 muestras.
- Memoria retrospectiva de 12 s en SDRAM.
- DC blocker, rampas anticlic de 1024 muestras y limitador de picos.
- Chorus, phaser, flanger y filtros conservan su estado entre escenas; el fundido
  de 1024 muestras evita cambios secos sin ejecutar reinicios en el callback.
- MIDI Clock, CC, notas y Program Change documentados en `MIDI.md`.

Line Out es nivel de linea. Para auriculares usa Headphones. No conectes una
salida amplificada de altavoz a Line In.

## Estructura

```text
DaisyPod_Aurora/
├── main.cpp                 Hardware, módulos DSP, controles, LED y MIDI
├── src/aurora_engine.h      API y banco de efectos
├── src/aurora_engine.cpp    Memoria, cadenas FX, mezcla y protección
├── Makefile                 libDaisy + DaisySP + DaisySP-LGPL
├── build.ps1                Compilación BOOT_SRAM/BOOT_NONE
├── flash.ps1                Carga DFU con validación de modo
├── MIDI.md                  Mapa MIDI
├── PERFORMANCE.md           Recetas rápidas
├── DIAGNOSTIC.md            Passthrough y prueba de masa/controles
└── THIRD_PARTY.md           Dependencias y licencias
```
