# RayDrone · versión WebAssembly (Rust)

Copia de la versión sencilla de RayDrone, pero con el **motor de audio escrito en
Rust y compilado a WebAssembly**, corriendo dentro de un **AudioWorklet**.

## Por qué esta arquitectura

A diferencia de la versión JS (que crea un nodo de Web Audio por grano), aquí el
motor **mezcla cada muestra** en un bucle Rust en el hilo de audio. Eso elimina de
raíz los problemas de la versión JS:

- **Sin tope de voces / sin pulsos por inanición** — los granos se mezclan en un
  buffer propio, no hay `MAX_SIMULTANEOUS`.
- **Timing perfecto** — sin `setTimeout`; el worklet trabaja muestra a muestra.
- **Nube continua** — los granos nacen como un flujo (no en *bursts*), así que no
  hay pulso rítmico audible.
- **Soft-clip integrado** — saturación suave en Rust, sin distorsión dura.

Es además **sin dependencias**: no usa `wasm-bindgen` ni crates, así que **no
necesita acceso a crates.io**. Solo `rustc` + el target `wasm32-unknown-unknown`.

## Archivos

| Archivo | Qué es |
|---|---|
| `raydrone.rs` | Motor granular en Rust (`no_std`, sin deps). Exporta `set_sample`, `set_params`, `process`, punteros de memoria. |
| `../core/` | Kernel DSP compartido (`raydrone_core`, `no_std`, sin deps): `clampf`, `soft`, `sample_at`, `win_at`, RNG. **El mismo código que enlaza el VST** — una sola fuente de verdad. |
| `processor.js` | `AudioWorkletProcessor` que instancia el wasm y rellena la salida cada bloque. |
| `lab-worker.js` | Web Worker del Convergence Lab: otra instancia del mismo wasm para medir convergencia sin congelar la UI. |
| `index.html` | Estudio web con tres flujos: Básico por macros, Medio por tareas y Profesional por inspectores especializados. |
| `build.sh` | Compila `../core` → `libraydrone_core.rlib` y luego `raydrone.rs` → `raydrone.wasm` (lo enlaza con `--extern`). |
| `vendor/` | Three.js r185 vendorizado (motor 3D del Nivel 2 de RayRunner) + `three-addons/` (postprocesado: `EffectComposer`/`RenderPass`/`UnrealBloomPass`/`OutputPass`, extraídos del paquete oficial). Sin CDN, offline. El motor de audio sigue sin depender de nada de esto. |

## Compilar

```bash
cd wasm
./build.sh
```

Si es la primera vez y falta el target wasm:

```bash
rustup target add wasm32-unknown-unknown
```

(Requiere `rustup`. Si no lo tienes: instálalo desde https://rustup.rs — una vez
añadido el target, la compilación es **offline**, no descarga crates.)

El comando real que ejecuta `build.sh` son dos pasos (ambos con `rustc` crudo,
sin Cargo ni crates.io): primero el kernel compartido, luego el motor enlazándolo.

```bash
# 1) kernel DSP compartido (no_std, sin deps) → rlib
rustc --edition 2021 --target wasm32-unknown-unknown -O -C panic=abort -C lto=fat \
      --crate-name raydrone_core --crate-type=lib ../core/src/lib.rs -o libraydrone_core.rlib
# 2) motor → wasm, enlazando el kernel
rustc --edition 2021 --target wasm32-unknown-unknown -O -C panic=abort -C lto=fat \
      --extern raydrone_core=libraydrone_core.rlib \
      --crate-type=cdylib raydrone.rs -o raydrone.wasm
```

## Ejecutar

Los AudioWorklets y la carga del `.wasm` necesitan servirse por **HTTP** (no vale
abrir el archivo con `file://`). Desde la raíz del repo:

```bash
python3 -m http.server 8080
```

y abre **http://localhost:8080/wasm/**.

1. Carga un WAV/MP3.
2. Haz click en la onda para mover el **foco**.
3. **Original · Ray direct** = un único rayo recorre tu sample sin granularizar,
   pero atraviesa el motor (materiales, filtro, movimiento y espacio) ·
   **Drone** / **Shimmer** = nube de muchos rayos.
4. Elige el entorno según la tarea: **Básico** (material + cuatro macros, zoom
   y monitor de salida),
   **Medio** (Material / Espacio / Rayos) o **Profesional** (añade Afinación,
   Análisis y Directo). Cambiar de entorno no reinicia ni modifica la escena.

## Estado

Motor con paridad casi completa con la versión JS:

- ✅ Nube granular continua (foco, apertura, grano, densidad).
- ✅ Muestreo **Random / Stratified / Quasi-MC** (golden ratio) — checkbox para A/B.
- ✅ **Aberración cromática**: bandas grave/medio/agudo con apertura escalada por
  banda (graves abren, agudos enfocan) y filtro one-pole por voz.
- ✅ **Rebotes (Russian roulette)**: al morir un grano, con probabilidad = reflexión
  nace un grano hijo (cola/transporte), con tope de profundidad.
- ✅ **Autoevolución recursiva**: la envolvente de la salida realimenta foco y
  apertura → el drone se modula a sí mismo.
- ✅ **Ambient · focos recursivos**: en vez de un único foco, una constelación.
  Las semillas se reparten en baja discrepancia (golden ratio) sobre el sample y
  derivan solas (paseo aleatorio lento, independiente por foco). Recursión: cada
  foco engendra focos hijo más breves y cercanos, con offset/apertura/vida que
  encogen por nivel → estructura **auto-similar** (la misma ley QMC a escala de
  sección, frase y grano). Controles: semillas, niveles del árbol, dispersión,
  deriva, nacimientos/seg. La constelación se dibuja sobre la onda (un bloom y un
  eje por foco, opacidad ∝ peso) y sobre el minimapa. Coste: O(focos) por bloque
  (≤48 focos) + O(focos) por grano; el bucle de audio sigue siendo O(voces).
- ✅ **Estéreo (Width)**: paneo equal-power por grano → nube ancha e inmersiva.
- ✅ **Octava / Shimmer**: probabilidad de que cada grano suene una octava arriba.
- ✅ **Pitch (transposición)**: ±12 semitonos (multiplicador de velocidad de lectura).
- ✅ **Play · piano por teclado/ratón/táctil**: teclado A W S E D F T G Y U J K O L
  (desde C4, D/F/A solo por clic/táctil al coincidir con atajos de la página), o
  clic/toque directo sobre el piano en pantalla (multi-touch → acordes con varios
  dedos). Mientras haya alguna nota sostenida, cada grano nuevo coge una de esas
  notas en vez del grado de Microtonal/Voicing — mismo mecanismo de muestreo por
  grado (estratificado/QMC/random) que ya usan esos dos sistemas.
- ✅ **Escala (textura microtonal)**: cada grano coge un grado de una tabla de ratios
  (12/19/22/24-EDO, justa entonación, Bohlen–Pierce). El grado se muestrea con la
  misma maquinaria de reducción de varianza que el eje temporal: estratificado puro
  (cada grado = un estrato → cobertura homogénea de la retícula) o Kronecker R2
  (constante plástica, decorrelada de la áurea del tiempo). El micro-detune ±4 cents
  convierte la retícula exacta en enjambre. Coste extra: ~0 (solo cambia la
  distribución del multiplicador de lectura). Verificado espectralmente: contraste
  grados/huecos ≈ 55×; el grado peor cubierto queda 22× sobre el fondo con QMC
  frente a ~10× con random.
- ✅ **Acorde (grados activos)**: subconjunto de la escala — tónica, quinta, tríada
  4:5:6, tétrada 4:5:6:7 (en BP, el acorde canónico 3:5:7) o pentatónica. Para cada
  ratio objetivo se usa el grado más cercano de la escala, así el mismo acorde queda
  afinado distinto en cada temperamento y la diferencia entre escalas se vuelve
  audible (con todos los grados activos, cualquier EDO denso suena a cluster).
- ✅ **Espacio por trayectorias**: cuatro trayectorias de reflexión estéreo con
  longitudes distintas, pérdida de energía y absorción según material. El control
  Reverb mezcla ese campo de rayos (no convolución ni un algoritmo de reverb
  clásico); Delay y Chorus reutilizan taps del mismo espacio.
- ✅ **Materiales sonoros**: Vacío, Metal, Madera, Cristal, Agua y Plasma
  alteran los rayos y sus reflexiones. Véase `SDK.md` para empaquetar escenas y
  materiales compuestos para otros desarrolladores.
- ✅ **Interfaz por intención**: Básico no expone parámetros técnicos; sus macros
  escriben sobre los mismos controles del motor. Medio enseña un único bloque
  de trabajo con parámetros primarios y Profesional revela los secundarios sin
  apilar todos los paneles a la vez.
- ✅ **Original · Ray direct**: la fuente directa se lee dentro del WASM, no por
  un bypass paralelo; comparte material, filtro, modulación y efectos con Drone.
- ✅ **Trazado inverso (opcional)**: precalcula la energía del sample y lanza los rayos
  hacia donde hay señal (importance desde la fuente) → menos rayos malgastados, más
  lleno y limpio. Brilla con material disperso; con notas sostenidas la mejora es leve.
- ✅ **Visual**: cono de dispersión, rayos coloreados por banda (grave/medio/agudo),
  medidor de salida y glow reactivo al nivel.
- ✅ **Zoom integrado en la onda principal**: un solo canvas. Rueda, pellizco
  (móvil) o slider (×1–×64); con zoom aparece una tira-minimapa arriba con el
  archivo entero y la ventana visible marcada (click en ella = salto global;
  click en la onda = foco fino). Los rayos se dibujan dentro de la ventana.
- ✅ **División tonal**: visor junto a los selectores de escala/acorde — la retícula
  de grados en cents (activos resaltados) y cada rayo viajando sobre la línea del
  grado que le tocó (el motor registra el ratio por rayo).
- ✅ **Convergence Lab en wasm**: las curvas las calcula el MISMO motor Rust
  (`lab_target` / `lab_estimate` / `lab_rms` en `raydrone.rs`) corriendo en una
  instancia aparte dentro de un Web Worker (`lab-worker.js`) — la UI no se congela
  y se mide el código que suena, no una simulación JS. Semilla fija → CSV
  reproducible bit a bit. N = 1…8192, 12 tiradas, 5 estrategias; acumuladores f64.
  Pendientes medidas: random −0.50 (teoría −0.5), stratified/QMC ≈ −0.58.
  El estimador JS se conserva solo para el A/B audible.
- ✅ **RayRunner (`game.html`)**: arcade cuya banda sonora la renderiza
  RayDrone en vivo — la demo de "audio adaptativo para videojuegos". El flujo
  es Nivel 1 (espacio, estilo R-Type) → Nivel 2 (circuito neón, de noche) →
  **Nivel 3 (el mismo circuito, pero de día y en sentido inverso)** →
  **Nivel 4 (Mazmorra, el nivel final)**. El pinball que ocupaba antes el
  nivel 4 se retiró por completo (física, mesa 3D y render 2D muerto): la
  mazmorra con sprites lo sustituye en el flujo principal.
  Nivel 1 (nave espacial, lateral), Nivel 2 (**Circuito Neón**: un circuito
  cerrado **3D de verdad** — WebGL con **Three.js vendorizado** en
  `vendor/` (sin CDN, sigue funcionando offline; el motor de audio continúa
  sin dependencias) — trazado como spline Catmull-Rom cerrada con curvas,
  chicanes, un túnel con luces, cambios de rasante, asfalto y tierra
  (texturas procedurales por canvas), arco de meta, contador de vueltas,
  fuerza centrífuga desde la curvatura real de la spline, sol de rayas,
  skyline y pilones neón), cada nivel con su propio sample sintetizado de
  24 s y 4 zonas — el espacio (pad → campanas → tormenta → coro) y el
  circuito (amanecer → calipso → niebla → faro 2077, en tono mayor y
  alegre). En el circuito, **cada sector de pista es una zona musical**: una
  vuelta = una pasada entera por el sample (el túnel suena a niebla estática,
  el tramo de tierra a faro 2077). Y **cómo juegas transforma la música**: el combo
  controla N (60→400 rayos/s) y la apertura — jugar bien = la textura converge
  densa y nítida (error ∝ 1/√N); un impacto desploma el combo (polvo
  granular), hunde el tono un instante y dispara una cola de rebotes; cada
  cristal enciende un destello de shimmer. **Power-ups** en ambos niveles:
  🛡 escudo (absorbe un golpe — y la música ni se entera), 🚀 misil (X),
  💣 bomba (C), más un **bláster ilimitado** con cadencia que se dispara con
  **clic de ratón, Z/espacio o un toque corto**. **Escuadrones drone**
  (R-Type): cada ~10 s entra una cadena de 5 naves en formación de onda
  senoidal — derribar el escuadrón entero antes de que escape paga +150 y
  sube el combo. **Enemigos fantasma** en los
  dos niveles: naves translúcidas que persiguen tu altura en el espacio y
  tres coches GT espectrales (material aditivo parpadeante) que dan vueltas
  solos al circuito — alcanzarlos es un golpe (si te adelantan ellos, te
  atraviesan: el barrido de colisión usa el mismo arco recorrido en el
  frame que las barreras, así un bajón de fps nunca "salta" el golpe),
  disiparlos con un arma da +80 y un destello shimmer, y reaparecen más
  adelante. El vehículo del jugador en el circuito es una **moto Tron**:
  lightcycle azul neón (casco bajo extruido, aleta dorsal, visor, piloto
  agachado, una sola vía de ruedas con llanta de disco y buje blanco,
  manillar, faro/piloto) con **doble muro de luz** trasero (de pie, no un
  ribbon plano) cuya longitud es la misma visualización de N que antes —
  **sombra propia** (luz direccional que sigue a la moto, mapa 512²
  ajustado) y **bloom** (Three.js `UnrealBloomPass`, resolución interna fija
  y barata para no penalizar el móvil) sobre el neón. **Conducción con
  inercia**: el mando/dedo acelera el carril (no lo teletransporta), la
  tierra agarra menos (acelera menos y frena más despacio → derrape), la
  curva empuja como una centrífuga real sobre la velocidad lateral (no
  sobre la posición) y el muro de la pista da un rebote suave en vez de un
  tope duro; la física corre en un **paso fijo con acumulador** (hasta 8
  subpasos por frame) para que un renderizado lento (móvil flojo) nunca
  meta el juego en cámara lenta — solo se pintan menos fotogramas.
  **Acelerador y freno de verdad** (↑/↓; en táctil el eje vertical del
  dedo), con crucero si no tocas nada: la velocidad alta multiplica los
  puntos y la centrífuga — arriesgar paga. **Turbo pads** (flechas neón
  fijas en la calzada): pisarlas dispara un subidón con FOV extra,
  vibración de cámara, muro de luz alargado y destello shimmer. La cámara
  es **viva**: baja, se aleja y abre el FOV con la velocidad. Un golpe sin
  escudo = **trompo** (giro de 360°, velocidad clavada, muro de luz
  apagado) + escombros 3D; pasar **rozando** una barrera da +25. Pista
  ancha (semiancho 9.5u) con **bordes Tron continuos** (cintas de neón
  cyan/magenta con bloom a lo largo de toda la vuelta — la referencia
  visual de noche y dentro del túnel), farolas cada ~20u (InstancedMesh,
  1 draw call por color) para que la velocidad se vea pasar, asfalto más
  claro con 3 carriles marcados, niebla más lejana en abierto y el
  **ambiente teñido por la zona musical** (la niebla vira hacia el color
  del sector — el circuito cuenta por dónde va el sample). Los obstáculos
  vienen en **patrones** (suelto, pareja, muro con hueco, eslálon) que se
  endurecen por vuelta. **Minimapa** abajo-izquierda: contorno real del
  trazado coloreado por superficie (asfalto/tierra/túnel), meta, rivales
  fantasma y tu posición con pulso. **Estilo ciudad**: calles cruzadas a
  nivel del suelo que el circuito sobrevuela + ~90 edificios con ventanas
  (InstancedMesh) flanqueando la pista formando cañones de calle — con un
  margen real (medio ancho de calzada + medio ancho de edificio + colchón)
  para que ninguno quede rozando o clavado en otro tramo de la pista (el
  circuito se cruza sobre sí mismo en el espacio). **12 carteles
  publicitarios** con coñas internas del propio proyecto ("SSR Motors:
  reflejamos hasta tu ego", "Convergencia garantizada o le devolvemos sus
  rayos", "Autoescuela Trompo: gira 360° gratis"…), textura procedural con
  texto autoajustado al ancho del panel y marco de neón a juego. **3
  cámaras** (botón 📷 o tecla V, "dentro" por defecto al entrar al
  circuito): persecución, dentro (primera persona desde el manillar, con
  el horizonte inclinándose al girar) y frontal (mirando a la moto). La
  visibilidad de la moto la decide el propio bucle de render cada frame
  (no un ajuste puntual al cambiar de cámara), así no depende de si la
  escena 3D ya había terminado de cargar. La moto lleva un **halo
  billboard** aditivo para leerse sobre
  cualquier fondo (la tierra clara se la comía). **Reflejos SSR en el
  asfalto**: no un cubemap fijo — un barrido real en espacio de pantalla
  (12 pasos, distancia creciente) contra una pasada previa de
  color+profundidad de la propia escena (256×144, a fotogramas alternos
  para abaratarla), con Fresnel (más reflejo a rasante, como el asfalto
  mojado) y el mismo tinte de niebla por zona. Solo el asfalto —la tierra
  no debe verse pulida—, con un coste medido de ~15-20% de fps bajo
  render por software (SwiftShader headless; en una GPU real, mucho
  menos). Sacrifica recibir la sombra del jugador en esos tramos (el
  reflejo del propio kart compensa visualmente).
  **Ranking global** (`wasm/api/leaderboard.js`, Vercel Serverless Function
  sin dependencias + Upstash Redis como sorted set) con **fallback
  automático al ranking local** si no hay backend desplegado o falla la
  red — el mismo objeto `lb` decide en tiempo real y lo indica en la UI
  (🌐 global / 📴 este dispositivo). Sin capas pregrabadas: un solo motor,
  parámetros vivos. En móvil: arrastre táctil (vertical en N1, horizontal
  en N2), canvas retina ~56vh, pantalla completa, mini-HUD en el lienzo,
  vibración y pausa automática.
  > ⚠️ **Importante para el despliegue**: si en Vercel el "Root Directory"
  > del proyecto está puesto a `wasm` (lo normal, ya que ese es el sitio
  > estático que se sirve), la función **tiene** que vivir en
  > `wasm/api/leaderboard.js` — Vercel solo detecta funciones dentro del
  > Root Directory configurado. Si estuviera en `api/` en la raíz del
  > repo (fuera de `wasm/`), Vercel jamás la despliega y visitar
  > `/api/leaderboard` da 404, aunque el resto del sitio funcione bien.
  > Además hay que conectar la integración de Storage (Upstash o Vercel
  > KV) al proyecto para que existan `KV_REST_API_URL`/`KV_REST_API_TOKEN`
  > (o `UPSTASH_REDIS_REST_URL`/`UPSTASH_REDIS_REST_TOKEN`).
  **Circuito mucho más largo** (~5750u, 2.5× la versión anterior) con un
  **primer tramo de calentamiento** (recta larga y llana + curva amplia,
  ~17% de la vuelta) sin obstáculos en la primera vuelta — tiempo de
  acostumbrarse a la moto antes del chicane, la cresta grande, el túnel y
  la tierra; el resto de elementos (pilones, farolas, edificios, dunas,
  turbo pads) escalan su densidad con la longitud real de la vuelta en
  vez de usar recuentos fijos. **Sidebar de teclas** (tecla H o botón ⌨):
  panel semi-transparente (50% de opacidad, no tapa el juego) con todos
  los controles de los tres niveles.
  **Nivel 3 (circuito de día, sentido inverso)**: tras completar 2 vueltas
  al circuito neón nocturno, la partida no salta a la mazmorra todavía — antes
  hay una segunda vuelta por la MISMA escena 3D (ni un solo mesh se
  reconstruye: asfalto, edificios, pilones, túnel, carteles… todo se reutiliza
  tal cual), pero recorrida **en sentido inverso** y bajo un **cielo de día**.
  El sentido inverso no duplica el trazado ni la física: un flag (`st.trackRev`)
  y tres funciones puente (`curveF`/`curvePt`/`curveTan`/`curveK`) reinterpretan
  la MISMA curva Catmull-Rom leyéndola al revés — posición, tangente (rumbo) y
  curvatura (para el banking/centrífuga) se recalculan coherentemente, así que
  el manillar, la cámara, los rivales fantasma, los turbo pads y el minimapa
  quedan bien orientados sin tocar la geometría estática. El día no es solo un
  cambio de fondo: cielo y sol cambian de textura (gradiente azul/blanco en vez
  de morado nocturno), la luz hemisférica y el sol direccional cambian de
  color e intensidad, las estrellas se ocultan, y el bloom baja su intensidad
  base (el neón no necesita tanto halo a plena luz). Mismas armas, mismo
  acelerador/freno, misma cámara de 3 vistas, mismo minimapa — es el nivel 2
  con otra luz y el volante al revés, no un nivel nuevo que aprender.
  El look de día está cuidado: **nubes procedurales** (cúmulos de elipses
  difusas pintados en la textura del cielo, con copias en ±ancho para que la
  costura de la esfera no corte ninguna nube, y estratos finos cerca del
  horizonte), **sol de mediodía** alto y compacto, atmósfera despejada
  (niebla mucho más lejana que de noche) y el **umbral del bloom subido a
  0.82** — con el umbral nocturno (0.4) el cielo claro entero "bloomeaba" y
  lavaba la escena; de día solo brillan los neones de verdad. Los cheurones
  de los **turbo pads giran 180°** para apuntar en el sentido de marcha real.
  **Botón "🎯 Entrenar"** en la portada: un modal con los 5 niveles para
  saltar directo a cualquiera y practicarlo — en modo entreno la **puntuación
  NO se guarda** en el ranking (saltarse niveles sería trampa).
  **Nivel 4 (Mazmorra)**: se entra al completar 2 vueltas del nivel 3;
  cruzar la **puerta de la cripta** (+1000) da paso al asalto final. Hack & slash de scroll lateral en el
  canvas 2D. Fases 1+2 listas: héroe de plataformas con los 8
  estados de la hoja de sprites (idle/walk/run/jump/fall/attack/damage/die),
  salto (↑/espacio o toque corto), tajo de espada con arco de luz (Z o clic),
  andar/correr (←→, botones, o el dedo apoyado a un lado), suelo + 14
  plataformas one-way, cámara lateral con suavizado y **parallax por biomas**
  (bosque oscuro → paso de montaña → patio del castillo → cripta del jefe),
  donde cada bioma = una zona musical (worldT sigue tu avance). El mundo
  vive en unidades fijas (DUN_H): un resize no desincroniza nada. Tiene su
  propio sample de 24 s (quintas de bosque → viento y campana de montaña →
  órgano del castillo → coro grave y tambor de la cripta), y en táctil los
  botones centrales pasan a ser **⚔ (tajo) y ⬆ (salto)**.
  **Enemigos (fase 2)**: 21 spawns deterministas repartidos por biomas, 6
  tipos con IA de tres líneas cada uno — slime (patrulla), seta (escupe
  esporas que puedes batear con la espada, +5), goblin y esqueleto
  (patrullan y te persiguen de cerca; el esqueleto aguanta 3 tajos),
  murciélago (vuelo en seno + persecución) y lobo (carga rápida al verte).
  **Combate**: la espada tiene hitbox solo en el tramo central del barrido
  y golpea a cada enemigo como mucho una vez por tajo (swingId); matar suma
  puntos y **sube el combo → N** (la música converge al encadenar); el
  contacto enemigo te quita una vida por `hitShip()` (mismo contrato
  sonoro que el resto del juego) con knockback e **invulnerabilidad breve
  con parpadeo**.
  **Pipeline de hojas de sprites (atlas JSON, no auto-detección)**: cada
  `sprites/*.png` opcional lleva un `sprites/*.json` hermano con las
  coordenadas de cada frame **por nombre** (`hero_walk_3`, `goblin_attack_1`…
  — el estándar de cualquier estudio: los compañeros usan nombres, no
  coordenadas a ojo). Detectar la rejilla píxel a píxel sobre arte generado
  por IA es frágil (sin cuadrícula perfecta) y fue la causa de un bug real:
  un flood-fill por brillo se colaba por los contornos oscuros del sprite y
  se comía la imagen entera. Ahora: (1) el **fondo se quita por color EXACTO**
  — se muestrea el propio color de fondo en las 4 esquinas y solo se hace
  transparente lo que cae dentro de esa tolerancia de color (no "cualquier
  pixel oscuro"), así que contornos/sombras oscuros pero DISTINTOS del fondo
  sobreviven; y (2) las coordenadas del atlas son **fraccionales** (0..1 del
  ancho/alto real de la imagen), así no dependen de a qué resolución exporte
  el PNG. `wasm/sprites/hero.json` (8 filas × frames variables: idle/walk/
  run=9, jump/fall=7, attack/damage=8, die=6) y `enemies.json` (10 criaturas
  × 7 columnas fijas: idle/walk1/walk2/attack1/attack2/hit/die) vienen
  generados con esta rejilla-por-proporción — ajustables a mano si el PNG
  real difiere. `fetch(..., {cache:'no-store'})` para el PNG y el JSON
  (no `<img src>`): si reemplazas el archivo sin cambiar el nombre, el
  navegador no sirve una copia cacheada. Si el atlas no cubre todos los
  estados/columnas necesarios, un `console.warn` explica qué falta (no un
  fallo silencioso). Sin PNG/JSON, placeholders dibujados a mano (héroe y
  los 6 enemigos) mantienen el nivel jugable.
  Además del recorte por atlas, cada frame pasa por un **auto-trim**: el
  rect declarado se mete unos píxeles hacia dentro (las líneas de rejilla
  viven en los bordes de celda) y se encoge a la caja mínima con píxeles
  opacos — una rejilla estimada con ±unos px de error no deja ni líneas ni
  márgenes en el sprite dibujado. Y la clave de fondo es **GLOBAL** (no
  flood-fill): el flood se frenaba en las líneas de rejilla y el negro
  encerrado dentro de cada celda quedaba opaco — las "cajas negras" detrás
  de los enemigos; como el fondo es un color uniforme, la clave global por
  color exacto lo elimina también dentro de las celdas.
  **Extras (`sprites/extras.png` + `extras.json`)**: el tercer sheet usa
  coordenadas en píxel (origen arriba-izquierda) relativas a
  `meta.atlasSize`, escaladas al tamaño real de la imagen al cargar. De él
  salen: los **fondos parallax pixel-art por bioma** (bosque → montañas →
  ruinas → cueva, en mosaico horizontal a 0.22× con fundido al bioma
  siguiente; sin el sheet, la cordillera procedural de siempre), las
  **decoraciones de suelo** (props deterministas cada ~210u, posición y
  frame por índice — sin RNG, los tests saben qué hay), y la **puerta** al
  final de la mazmorra (interactable_object_001), que hace visible el
  objetivo del nivel.
  Pendiente: orco miniboss, mago jefe y pickups.
  **Nivel 5 (Komandos)**: el nivel final — run-and-gun **cenital** estilo
  Commando/Mercs en el canvas 2D, con el atlas real cargado (playa, selva,
  aldea en llamas y base militar, soldados/vehículos con arte de verdad).
  Movimiento en 8 direcciones (flechas/WASD o el dedo), **uzi** hacia donde
  miras (Z/espacio/clic; el power-up dualgun cambia a las metralletas dobles
  del atlas), **bazooka** con X (gasta 🚀, cohete con estela de humo y
  explosión en área) y **bombardeo aéreo** con C (gasta 💣, misiles cayendo
  del cielo). Cuatro biomas = cuatro zonas musicales de un sample propio de
  24 s (desembarco → selva → aldea en llamas → base enemiga) y avance de
  izquierda a derecha con la misma cámara suavizada de la mazmorra.
  Enemigos por tabla determinista (KOM_SPAWNS), cada uno con su propia IA y
  **telegrafía antes de disparar** (se plantan y "apuntan" un instante,
  visible, antes del tiro — da tiempo a reaccionar): soldados con fusil,
  **motos kamikaze** que aceleran en rampa antes de embestir, jeeps que
  patrullan y ametrallan en ráfagas de 3, y **tanques** con obuses que
  explotan en área y fogonazo en el cañón. Atrezzo con semilla fija (mismo
  mapa siempre): palmeras, rocas, casas ardiendo con resplandor parpadeante,
  sacos terreros que paran balas, alambradas/neumáticos que las balas
  atraviesan, **cajas** que sueltan suministros (con imán suave al
  acercarse) y **barriles rojos que explotan en cadena**. Llegar al
  **helipuerto (H)** real del atlas = extracción: +1500 y fin del juego
  como victoria 🏆.
  **FX de combate**: trazadoras aditivas con estela en cada disparo,
  chispazo de impacto en el enemigo, explosiones con núcleo blanco → bola
  naranja → onda expansiva, marca de quemadura persistente en el suelo,
  humo con física propia (denso y oscuro de las explosiones, claro y
  disperso del disparo), y **popups de puntos** flotando desde cada baja.
  Todas las entidades llevan sombra elíptica en los pies — despega los
  sprites del terreno en vez de flotar sobre él.
  **Atlas del Komandos**: contratos JSON con **rects absolutos medidos
  sobre el PNG real** (extraídos por detección de componentes conexas, no
  una rejilla teórica adivinada — las hojas reales no vienen en una
  cuadrícula perfecta) en `sprites/backgrounds.json`, `world_tiles.json`,
  `enemy_units.json` y `commando_weapons.json`. Cada frame declara su
  `facing` de fábrica (el arte de fusil/bazooka mira a la derecha, el de
  uzi/dualgun en reposo mira arriba, los vehículos traen vistas
  frente/espalda/lado) y el render rota o elige la vista según el rumbo
  real — no hacen falta 8 direcciones dibujadas a mano. Sin los PNG, el
  nivel entero funciona con placeholders vectoriales.
  En la mazmorra no hay armas a distancia: X/C/bláster quedan desactivados
  (el héroe pelea con la espada) y al entrar se limpian los restos del
  circuito — sin ello, un misil podía "matar" un fantasma invisible sobrante
  y regalar puntos.
  **Récords "leyenda"**: unas marcas base (p. ej. BRUZOS, THOR) se fusionan
  siempre en el ranking (cliente y servidor, ordenadas y sin duplicar) como
  marca a batir.
  **Modal "Cómo jugar"**: la portada queda limpia (título, tagline y
  botones); las instrucciones completas viven en un modal aparte que se
  abre solo, una vez, en la primera visita (recordado en localStorage) y
  siempre accesible después con el botón ❓ — se cierra con el botón ✕, con
  Escape o clicando fuera.

Paridad completa con la versión JS. La compilación local se valida con
`build.ps1` en Windows o `build.sh` en macOS/Linux, y `node test_engine.mjs`
ejecuta la suite funcional del DSP compilado.
