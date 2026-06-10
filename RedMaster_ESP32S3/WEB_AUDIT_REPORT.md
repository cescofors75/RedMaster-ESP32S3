# Auditoría Web — RED808 ESP32-S3

Auditoría del frontend completo servido por el ESP32-S3 (7 páginas + módulos JS,
~48k líneas): bugs, mejoras y propuestas de cambios visuales.

**Páginas:** `index.html` (app principal, app.js 8.8k líneas), `mobile.html`,
`admin.html`, `patchbay.html`, `gesture.html` / `gesture-pro.html`, `multiview.html`.
**Módulos compartidos:** synth-editor, sample-editor, melody-editor, midi-import,
export-pattern, chat-agent, waveform-visualizer, keyboard-controls.

**Metodología:** verificación mecánica cruzada (comandos JS ↔ handlers del servidor,
tipos de mensaje servidor ↔ handlers JS, IDs duplicados, referencias DOM huérfanas,
balance setInterval/clearInterval, métricas CSS) + lectura dirigida de los puntos
calientes (onmessage, reconexión WS, animación de step, manejo táctil, XSS).
Las rutas/líneas se refieren al contenido **realmente flasheado** (`data_gz/web/`,
descomprimido).

---

## 0. Hallazgo de pipeline (previo a todo)

**`data/web/` (fuentes) está desincronizado con `data_gz/` (lo que se flashea).**
`platformio.ini` fija `data_dir = data_gz`. Difieren: `app.js`, `index.html`,
`mobile.js`, `style.css`, `patchbay.js` (sí coincide `admin.js`). Riesgo real de
editar la fuente equivocada y perder cambios al regenerar.
**Fix:** un script de build (`gzip -9 -kf data/web/* → data_gz/web/`) como paso
previo a `uploadfs`, y tratar `data/web` como única fuente de verdad.

---

## BUGS

### B1 (HIGH) — IDs duplicados: la sección "clean tracks" fantasma
`index.html:175-176` y `index.html:1462-1463`
`cleanTracksGrid` y `cleanTracksSummary` existen **dos veces**. `getElementById`
devuelve siempre el primero, así que la segunda sección (línea 1462, con su texto
placeholder "0/4 ocupadas…") **nunca se actualiza** y queda como UI muerta visible.
**Fix:** eliminar el bloque duplicado o renombrar IDs y decidir cuál es el real.

### B2 (HIGH) — `setScratch`: función del chat rota silenciosamente
`chat-agent.js:726-727`
El chat-agent envía `{cmd:'setScratch', track, value, rate}` pero **no existe ningún
handler** en `processCommand` del servidor (verificado contra los 172 comandos de
`WebInterface.cpp`). El comando se descarta sin error: el usuario pide "scratch" al
agente, este informa éxito y no pasa nada.
**Fix:** implementar el comando en el servidor o eliminar la tool del catálogo del agente.

### B3 (MEDIUM) — `JSON.parse` sin try/catch en el onmessage principal
`app.js:418`
`const data = JSON.parse(event.data)` sin protección: un frame malformado (p.ej.
truncado bajo presión de heap del servidor) lanza una excepción no capturada en cada
mensaje afectado. Mismo patrón a revisar en mobile.js/patchbay.js.
**Fix:** envolver en try/catch y loggear el frame descartado.

### B4 (MEDIUM) — El error `low_heap` del servidor no se muestra al usuario
Ningún JS tiene `case 'error'` en su dispatch. El servidor (tras la auditoría de
firmware) emite `{"type":"error","msg":"low_heap"}` cuando descarta comandos; la UI
lo ignora y el usuario percibe "no responde".
**Fix:** añadir `case 'error':` → toast rojo con el msg (en app.js, mobile.js, patchbay.js).

### B5 (MEDIUM) — Desincronización multi-cliente: tipos emitidos que nadie maneja
20 tipos de mensaje del servidor no tienen handler en ningún JS. La mayoría son ACKs
inofensivos (`stepProbabilitySet`, `synthPresetAck`, …) pero **`trackSolo`** y
**`patternSelected`** sí pierden estado: si un segundo cliente hace solo de un track
o cambia de patrón, los demás clientes no lo reflejan hasta el siguiente `state` completo.
**Fix:** manejar al menos `trackSolo` y `patternSelected` en app.js.

### B6 (MEDIUM) — Sliders sin debounce → flooding del WebSocket
`app.js` tiene 17 listeners `'input'`, 6 de ellos envían directamente por WS en cada
evento (decenas de mensajes por segundo al arrastrar). El servidor rate-limitea algunos
FX, pero el resto satura la cola SPI compartida. `patchbay.js` sí tiene throttle (6 usos);
app.js no tiene ninguno.
**Fix:** throttle de ~50-80ms en los sliders de app.js (igual que patchbay).

### B7 (LOW) — 19 referencias DOM muertas en app.js
`barPadsVolume`, `filterCutoff`, `filterCutoffValue`, `filterType`, `sampleLoadStatus`,
`trkRevBtn`, `trkStutBtn`, … — `getElementById` de IDs que no existen ni en el HTML ni
se crean dinámicamente. Todas tienen null-guard (no crashean) pero son features muertas
que confunden el mantenimiento.
**Fix:** eliminar los bloques muertos.

### B8 (LOW) — Interval sin liberar en gesture.js
`gesture.js:631` — monitor de estado WS con `setInterval` nunca limpiado. Fuga menor
(scoped a la página).

### B9 (LOW) — URL externa hardcodeada en mobile.js
`mobile.js:1464` — `location.href = \`https://${ip}:8443/mobile\`` asume un bridge
externo en el puerto 8443. Si no existe, el botón lleva a un error de conexión sin aviso.

---

## MEJORAS (robustez / performance / mantenibilidad)

### M1 — Polling en pestañas ocultas
`admin.js:144` refresca `/api/sysinfo` cada 3s y gesture.js cada segundo, **también con
la pestaña en background** (no hay `visibilitychange` en ningún archivo). Pestañas
olvidadas cargan al ESP32 indefinidamente.
**Fix:** pausar polling con `document.hidden` y reanudar al volver.

### M2 — style.css: 13.7k líneas con duplicación significativa
Bloques enteros repetidos ×4 (`.seq-step`, `.step-count-btn*`, `.sequencer-grid-wrapper`)
y ×3 (`.status`, `.logo`, `.header-controls`) — capas históricas que se pisan entre sí;
168 `!important` como síntoma. Riesgo de "arreglo en un sitio, roto en otro".
**Fix:** pasada de consolidación (mantener la última definición de cada bloque duplicado).

### M3 — Tokenización del tema incompleta en la página principal
`theme-vars.css` está bien diseñado y las páginas secundarias lo usan correctamente
(admin: 92 var vs 11 hex; multiview: 61 vs 3). Pero **style.css** (la principal) tiene
**979 colores hex hardcodeados vs 410 var()** — la página más grande es la menos
tematizable. `gesture*` ni siquiera enlaza `theme-vars.css`.
**Fix:** migración incremental a tokens; enlazar theme-vars en gesture.

### M4 — onclick inline masivo
40 en index.html, 69 en patchbay.html. Impide endurecer con CSP y dificulta el refactor.
**Fix:** migración gradual a `addEventListener` (no urgente).

### M5 — Sin wake lock en las páginas de performance
mobile/gesture no usan `navigator.wakeLock`: la pantalla se apaga en plena sesión táctil.
**Fix:** `wakeLock.request('screen')` con guard de soporte + reacquire en `visibilitychange`.

### Lo que está BIEN (no tocar)
- Reconexión WS con backoff exponencial y cap (`app.js:54-63`), contador reseteado en onopen.
- Animación de step con `requestAnimationFrame` + caché de columnas (`app.js:4166-4186`) —
  el hot path está bien diseñado.
- mobile.js usa el **trigger binario** `[0x90, pad, vel]` (baja latencia) en vez de JSON.
- chat-agent **escapa HTML** antes de cada `innerHTML` (sin XSS) y no embebe API keys
  (bridge configurable por el usuario).
- Carga diferida de módulos (`loadDeferredModules`) tras el core UI.
- admin.js maneja errores de fetch con badge + log visible.

---

## PROPUESTAS VISUALES

1. **Barra de navegación común.** Hoy **ninguna página enlaza a las demás** (solo
   app.js→multiview y patchbay→`/`); se navega escribiendo URLs. Añadir un mini-header
   compartido (o un menú ⋮) con: Sequencer · Patchbay · Multiview · Gesture · Admin ·
   Mobile. Es el cambio de mayor impacto en usabilidad.
2. **Indicador de conexión unificado.** Badge de estado WS (verde/ámbar/rojo) con el
   mismo diseño en las 7 páginas + toast "Reconectando…" durante el backoff. Hoy cada
   página lo resuelve distinto (admin usa badge HTTP, gesture un texto, multiview nada).
3. **Toast handler para errores del servidor** (enlaza con B4): rojo para `error`,
   ámbar para drops, con auto-dismiss a 4s.
4. **Escala de diseño en theme-vars.css:** añadir `--r808-radius-sm/md/lg: 4/8/12px`
   y `--r808-gap-sm/md/lg: 4/8/16px`, y aplicarlos donde hoy hay valores ad-hoc
   (se ven radios de 3, 4, 5, 6, 8, 10, 12px mezclados).
5. **Animaciones GPU-friendly.** Hay ~15 keyframes animando `box-shadow` (glow de pads
   /steps). En móviles baratos esto repinta toda la capa. Sustituir por
   `transform: scale()` + `opacity` sobre un pseudo-elemento con el glow pre-renderizado.
6. **`:focus-visible` global:** `outline: 2px solid var(--r808-accent-cyan)` — hoy no
   hay estilos de foco y la navegación por teclado es invisible.
7. **Scrollbar oscura coherente** (`::-webkit-scrollbar` + `scrollbar-color`) en
   index/patchbay, que tienen paneles con scroll y muestran la scrollbar clara del SO.
8. **Quitar `user-scalable=no` / `maximum-scale=1`** de index/admin/patchbay
   (accesibilidad: impide zoom a usuarios con baja visión; iOS ya lo ignora). Mantenerlo
   solo en las páginas de performance táctil (mobile/gesture) donde el zoom accidental
   rompe la interacción.
9. **Estados hover/active consistentes:** definir un patrón único (elevación +
   borde acento) para todos los botones; varios controles del panel FX no dan feedback.
10. **Pantalla "desconectado" en multiview:** hoy si el WS cae, el multiview queda
    congelado sin aviso; un overlay semitransparente "⚡ Reconectando" evita decisiones
    sobre datos stale en directo.

---

## Priorización sugerida

| Orden | Item | Esfuerzo | Impacto |
|---|---|---|---|
| 1 | B1 (IDs duplicados) + B3 (try/catch) + B4/V3 (toast error) | Bajo | Alto |
| 2 | V1 (navegación común) + V2 (indicador conexión) | Medio | Alto |
| 3 | B6 (throttle sliders) + M1 (visibilitychange) | Bajo | Medio (alivia al ESP32) |
| 4 | B2 (setScratch) + B5 (trackSolo/patternSelected) | Bajo | Medio |
| 5 | Pipeline data/web↔data_gz (hallazgo 0) | Bajo | Alto (evita pérdidas) |
| 6 | V5/V6/V7/V8 (CSS polish) | Medio | Medio |
| 7 | M2/M3 (consolidación style.css + tokens) | Alto | Medio (largo plazo) |
