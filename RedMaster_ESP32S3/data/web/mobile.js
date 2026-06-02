/* =========================================================================
   RED808 Mobile ÔÇö l├│gica m├¡nima para tocar desde el m├│vil.
   Reutiliza el MISMO protocolo WebSocket que la UI principal (app.js):
     - Pads (sample)  : mensaje binario [0x90, pad, velocity]
     - Piano (synth)  : {cmd:'synthNoteOnEx'} / {cmd:'synthNoteOff'}
     - Sequencer      : {cmd:'setStep'}, sync por mensajes 'pattern'/'step'/'state'
     - Filtros        : {cmd:'setTrackFilter'} / {cmd:'clearTrackFilter'}
     - Transporte     : {cmd:'start'|'stop'|'tempo'|'selectPattern'|'setStepCount'}
   ========================================================================= */
(function () {
  'use strict';

  // ---- Constantes compartidas con app.js -------------------------------
  const TRACK_NAMES = ['BD','SD','CH','OH','CY','CP','RS','CB','LT','MT','HT','MA','CL','HC','MC','LC'];
  const PATTERN_NAMES = ['HIP HOP','TECHNO','DnB','BREAK','HOUSE','TRAP'];
  // ├ìndices de pista por nombre (para los patrones demo).
  const TR = { BD:0, SD:1, CH:2, OH:3, CY:4, CP:5, RS:6, CB:7, LT:8, MT:9, HT:10, MA:11, CL:12, HC:13, MC:14, LC:15 };
  // Patrones demo reales (16 pasos), uno por nombre, para que cada estilo SUENE
  // distinto. Cada entrada: { pista: [pasos activos 0..15] }.
  const DEMO_PATTERNS = [
    // HIP HOP ÔÇö boom bap
    { [TR.BD]:[0,6,10], [TR.SD]:[4,12], [TR.CH]:[0,2,4,6,8,10,12,14], [TR.OH]:[14] },
    // TECHNO ÔÇö four on the floor
    { [TR.BD]:[0,4,8,12], [TR.CH]:[0,2,4,6,8,10,12,14], [TR.OH]:[2,6,10,14], [TR.CP]:[4,12] },
    // DnB ÔÇö two step
    { [TR.BD]:[0,10], [TR.SD]:[4,12], [TR.CH]:[0,2,4,6,8,10,12,14], [TR.OH]:[7,15] },
    // BREAK ÔÇö amen-ish
    { [TR.BD]:[0,10], [TR.SD]:[4,7,12], [TR.CH]:[0,2,4,6,8,10,12,14], [TR.OH]:[14] },
    // HOUSE ÔÇö kick + clap + offbeat hats
    { [TR.BD]:[0,4,8,12], [TR.CP]:[4,12], [TR.CH]:[0,2,4,6,8,10,12,14], [TR.OH]:[2,6,10,14] },
    // TRAP ÔÇö sparse kick + hi-hat rolls
    { [TR.BD]:[0,6,9], [TR.SD]:[8], [TR.CH]:[0,2,3,4,6,8,10,11,12,14,15], [TR.CB]:[0,8] }
  ];
  // 16 colores fijos (uno por pista/pad), legibles sobre fondo oscuro.
  const PALETTE = [
    '#ff3b30','#ff9500','#ffcc00','#34c759','#00e5ff','#0a84ff','#5e5ce6','#bf5af2',
    '#ff2d55','#ff6b3d','#a3d900','#30d158','#64d2ff','#5ac8fa','#af52de','#ff375f'
  ];
  // Paletas por tema visual (16 colores, uno por pista).
  const PALETTES = {
    red:    ['#ff2020','#ff5630','#ff7a3d','#ff9540','#ff4060','#ff2d55','#e01030','#ff6a4d',
             '#ff8c5a','#ff3b30','#d92020','#ff7050','#ffae8a','#ff5040','#ff9070','#ff4530'],
    neon:   ['#00e5ff','#ff2d95','#7dff3a','#ffe600','#4488ff','#ff5fa0','#bf5af2','#34c759',
             '#00cfff','#ff80c0','#4dffa6','#ffd000','#36c5f0','#ff3385','#d070ff','#00ffcc'],
    acid:   ['#00ff88','#c8ff00','#39ff14','#ffe600','#80ff00','#4dffa6','#b3ff00','#00ffc8',
             '#a3f000','#f5e642','#00ff55','#e6ff00','#44ff88','#aaff00','#66ff33','#00ffaa'],
    sunset: ['#ff7a18','#ff2d55','#ff9d4d','#ff3d7f','#ffcc00','#ff6b3d','#ff5500','#ff9966',
             '#ffc18a','#ff1493','#ff8c00','#ff3366','#ffaa44','#ff6699','#ff7733','#ff4080'],
    retro:  ['#f5a623','#e74c3c','#f39c12','#d35400','#e67e22','#c0392b','#f1c40f','#ca6f1e',
             '#eb984e','#d68910','#f5b041','#a93226','#fad7a0','#dc7633','#f0b27a','#e59866'],
    gray:   ['#f0f0f0','#d8d8d8','#c0c0c0','#a8a8a8','#e8e8e8','#d0d0d0','#b8b8b8','#a0a0a0',
             '#e4e4e4','#cccccc','#b4b4b4','#9c9c9c','#dcdcdc','#c4c4c4','#acacac','#949494']
  };
  let currentPalette = PALETTES.red;
  // Color para cada uno de los 6 presets FX seg├║n la paleta del tema actual.
  const fxColor = (i) => currentPalette[(i * 3) % 16];
  // Efectos "modo ni├▒o": presets con nombre divertido aplicados a TODAS las
  // pistas a la vez. `type` es el filtro que espera el firmware (1..9), `res`
  // la resonancia. El cutoff lo controla el slider "Tono".
  const FX_PRESETS = [
    { id: 'off',    name: 'Normal',    emoji: '🎵', color: '#5b6472', filterType: 0,  cutoff: 4000, res: 1        },
    { id: 'sub',    name: 'Submarino', emoji: '🌊', color: '#0a84ff', filterType: 1,  cutoff: 350,  res: 6        },
    { id: 'bright', name: 'Brillante', emoji: '✨', color: '#ffcc00', filterType: 2,  cutoff: 3500, res: 6        },
    { id: 'phone',  name: 'Teléfono',  emoji: '📞', color: '#ff9500', filterType: 3,  cutoff: 1200, res: 8        },
    { id: 'robot',  name: 'Robot',     emoji: '🤖', color: '#34c759', filterType: 9,  cutoff: 700,  res: 14       },
    { id: 'wah',    name: 'Wah',       emoji: '🎸', color: '#bf5af2', filterType: 6,  cutoff: 1200, res: 8, gain: 10 }
  ];
  const STEP_COUNTS = [16, 32, 64];
  // Piano: una octava = 12 semitonos. Patr├│n de teclas negras.
  const WHITE_OFFSETS = [0, 2, 4, 5, 7, 9, 11];
  const BLACK = { 0: 1, 1: 3, 3: 6, 4: 8, 5: 10 }; // posici├│n blanca -> semitono negro
  // Colores arco├¡ris para las 7 teclas blancas (modo ni├▒o).
  const WHITE_COLORS = ['#ff4d4d', '#ff9f40', '#ffd633', '#4dd964', '#36c5f0', '#5b8def', '#b06bf0'];
  const PIANO_ENGINES = [
    { id: 3, label: '303' },
    { id: 4, label: 'WT' },
    { id: 5, label: 'SH101' },
    { id: 6, label: 'FM2OP' }
  ];
  let pianoEngine = 3;
  // Temas visuales (re-pintan los tokens RED808 v├¡a body[data-theme]).
  const THEMES = [
    { id: 'red',    name: 'RED808', color: '#ff2020' },
    { id: 'neon',   name: 'Neon',   color: '#00e5ff' },
    { id: 'acid',   name: 'Acid',   color: '#00ff88' },
    { id: 'sunset', name: 'Sunset', color: '#ff7a18' },
    { id: 'retro',  name: 'Retro',  color: '#f5a623' },
    { id: 'gray',   name: 'Gray',   color: '#cccccc' }
  ];
  const THEME_KEY = 'r808_mobile_theme';

  // ---- Estado ----------------------------------------------------------
  let ws = null;
  let connected = false;
  let retryTimer = null;
  let isPlaying = false;
  let bpm = 120;
  let stepCount = 16;
  let octave = 4;
  let currentPattern = 0;
  let _patLock = false;
  let _patLockTimer = null;
  let activeFxPreset = 'off';
  const seqState = []; // seqState[track][step] = bool
  for (let t = 0; t < 16; t++) seqState.push(new Array(64).fill(false));
  const muteState = new Array(16).fill(false);
  const soloState = new Array(16).fill(false);

  const $ = (id) => document.getElementById(id);

  // =====================================================================
  // WebSocket
  // =====================================================================
  function connect() {
    clearTimeout(retryTimer);
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(`${scheme}://${location.host}/ws`);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
      connected = true;
      setConn(true);
      send({ cmd: 'init' });
      setTimeout(() => send({ cmd: 'getPattern' }), 250);
    };
    ws.onclose = () => { connected = false; setConn(false); retryTimer = setTimeout(connect, 1500); };
    ws.onerror = () => { try { ws.close(); } catch (_) {} };
    ws.onmessage = (ev) => {
      if (ev.data instanceof ArrayBuffer) return; // niveles de audio: ignorar en m├│vil
      if (typeof ev.data !== 'string') return;
      let msg; try { msg = JSON.parse(ev.data); } catch (_) { return; }
      handleMessage(msg);
    };
  }

  function send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) { ws.send(JSON.stringify(obj)); return true; }
    return false;
  }
  function sendBinary(bytes) {
    if (ws && ws.readyState === WebSocket.OPEN) { ws.send(new Uint8Array(bytes)); return true; }
    return false;
  }
  function setConn(on) { $('connDot').classList.toggle('on', on); }

  // ---- Mensajes entrantes ---------------------------------------------
  function handleMessage(d) {
    const type = d.type;
    if (type === 'state') {
      if (d.tempo !== undefined) setBpm(parseFloat(d.tempo), false);
      if (d.stepCount && d.stepCount !== stepCount) applyStepCount(d.stepCount);
      if (d.step !== undefined) highlightPlayhead(d.step);
      if (d.playing !== undefined) setPlaying(!!d.playing, false);
      if (d.pattern !== undefined && !_patLock) setPatternSel(d.pattern, false);
      return;
    }
    if (type === 'pattern') {
      if (d.stepCount && d.stepCount !== stepCount) applyStepCount(d.stepCount);
      loadPattern(d);
      if (d.index !== undefined && !_patLock) setPatternSel(d.index, false);
      return;
    }
    if (type === 'step') { highlightPlayhead(d.step); syncFlashPads(d.step); return; }
    if (type === 'pad') { flashPadEl(d.pad); return; }
    if (type === 'stepCount' && d.count) { applyStepCount(d.count); return; }
    if (type === 'sampleList') { renderSampleList(d); return; }
    if (type === 'sampleLoaded') { if (d.pad !== undefined) flashPadEl(d.pad); return; }
    if (type === 'trackMuted') { if (d.track !== undefined) { muteState[d.track] = !!d.muted; refreshTrackButtons(); } return; }
    if (type === 'trackSolo') { if (d.track !== undefined) { soloState[d.track] = !!d.solo; refreshTrackButtons(); } return; }
  }

  function loadPattern(d) {
    for (let t = 0; t < 16; t++) {
      const row = d[t] || d[String(t)];
      for (let s = 0; s < stepCount; s++) {
        seqState[t][s] = !!(row && row[s]);
      }
    }
    refreshSeqDots();
  }

  // =====================================================================
  // Navegaci├│n de vistas
  // =====================================================================
  function initNav() {
    document.querySelectorAll('.m-tab').forEach((tab) => {
      tab.addEventListener('click', () => {
        const view = tab.dataset.view;
        document.querySelectorAll('.m-tab').forEach((t) => t.classList.toggle('is-active', t === tab));
        document.querySelectorAll('.m-view').forEach((v) => {
          v.classList.toggle('is-active', v.id === 'view-' + view);
        });
        jamSetActive(view === 'jam'); // arranca/para el loop del canvas
      });
    });
  }

  // =====================================================================
  // Transporte
  // =====================================================================
  function initTransport() {
    $('playBtn').addEventListener('click', () => {
      if (isPlaying) { send({ cmd: 'stop' }); setPlaying(false); }
      else { send({ cmd: 'start' }); setPlaying(true); }
    });
    $('bpmDown').addEventListener('click', () => setBpm(bpm - 1, true));
    $('bpmUp').addEventListener('click', () => setBpm(bpm + 1, true));

    $('patPrev').addEventListener('click', () => stepPattern(-1));
    $('patNext').addEventListener('click', () => stepPattern(1));
    setPatternSel(currentPattern, false);
  }
  function stepPattern(dir) {
    const n = PATTERN_NAMES.length;
    currentPattern = (currentPattern + dir + n) % n;
    setPatternSel(currentPattern, false);
    // Bloquea ecos del firmware durante 1.5s para que +/- mande de verdad.
    _patLock = true;
    if (_patLockTimer) clearTimeout(_patLockTimer);
    _patLockTimer = setTimeout(() => { _patLock = false; }, 1500);
    send({ cmd: 'selectPattern', index: currentPattern });
    // Carga el beat demo del estilo elegido para que cada nombre suene distinto.
    applyDemoPattern(currentPattern);
  }
  // Vuelca el patr├│n demo del ├¡ndice en el secuenciador (solo manda los cambios).
  function applyDemoPattern(idx) {
    const demo = DEMO_PATTERNS[idx] || {};
    for (let t = 0; t < 16; t++) {
      const onSteps = new Set(demo[t] || []);
      for (let s = 0; s < stepCount; s++) {
        const want = s < 16 ? onSteps.has(s) : false;
        if (seqState[t][s] !== want) {
          seqState[t][s] = want;
          send({ cmd: 'setStep', track: t, step: s, active: want, noteLen: 1 });
        }
      }
    }
    refreshSeqDots();
  }
  function setPlaying(on, doSend) {
    isPlaying = on;
    $('playBtn').classList.toggle('playing', on);
    $('playBtn').textContent = on ? 'ÔÅ╣' : 'ÔûÂ';
    if (doSend) send({ cmd: on ? 'start' : 'stop' });
  }
  function setBpm(v, doSend) {
    bpm = Math.max(40, Math.min(300, Math.round(v)));
    $('bpmVal').textContent = String(bpm);
    if (doSend) send({ cmd: 'tempo', value: bpm });
  }
  function setPatternSel(idx, doSend) {
    currentPattern = ((idx % PATTERN_NAMES.length) + PATTERN_NAMES.length) % PATTERN_NAMES.length;
    $('patName').textContent = PATTERN_NAMES[currentPattern] || String(idx);
    if (doSend) send({ cmd: 'selectPattern', index: currentPattern });
  }

  // =====================================================================
  // Pads
  // =====================================================================
  let _holdTimer = null;
  let _tremoloTimer = null;
  let _tremoloPad = -1;
  let _tremoloEl = null;
  let lastTappedPad = 0;
  let _tonoDebounce = null;
  // Cu├íntos pads se muestran en pantalla. Menos pads = pads m├ís grandes.
  const PAD_COUNTS = [16, 8, 4, 2, 1];
  let padCount = 16;
  // Columnas del grid seg├║n el n├║mero de pads visibles.
  function padCols(n) {
    if (n >= 16) return 4;
    if (n >= 8) return 2;
    if (n >= 4) return 2;
    return 1; // 2 y 1 pad => una sola columna
  }
  function initPadCountBar() {
    const bar = $('rollBar');
    bar.innerHTML = '';
    // Wire static Sonidos button
    const uploadBtn = $('padsUploadBtn');
    if (uploadBtn) {
      uploadBtn.addEventListener('click', () => openSampleSheet(lastTappedPad));
    }
    PAD_COUNTS.forEach((n) => {
      const b = document.createElement('button');
      b.className = 'm-roll-btn';
      b.textContent = String(n);
      b.dataset.count = String(n);
      b.classList.toggle('active', n === padCount);
      b.addEventListener('click', () => {
        padCount = n;
        document.querySelectorAll('#rollBar .m-roll-btn').forEach((x) =>
          x.classList.toggle('active', +x.dataset.count === n));
        buildPads();
      });
      bar.appendChild(b);
    });
  }
  function buildPads() {
    const grid = $('padsGrid');
    grid.innerHTML = '';
    grid.style.gridTemplateColumns = `repeat(${padCols(padCount)}, 1fr)`;
    grid.classList.toggle('few', padCount <= 4);
    for (let i = 0; i < padCount; i++) {
      const pad = document.createElement('button');
      pad.className = 'm-pad';
      pad.style.setProperty('--pad-c', currentPalette[i]);
      pad.textContent = TRACK_NAMES[i];
      let startX = 0, startY = 0;
      const startHold = () => {
        _holdTimer = setTimeout(() => { _holdTimer = null; startTremolo(i, pad); }, 160);
      };
      const cancelHold = () => {
        if (_holdTimer) { clearTimeout(_holdTimer); _holdTimer = null; }
        if (_tremoloPad === i) stopTremolo();
      };
      const down = (e) => {
        e.preventDefault();
        const t = e.touches ? e.touches[0] : e;
        startX = t.clientX; startY = t.clientY;
        lastTappedPad = i;
        triggerPad(i); flash(pad);
        startHold();
      };
      const move = (e) => {
        if (!_holdTimer && _tremoloPad !== i) return;
        const t = e.touches ? e.touches[0] : e;
        if (Math.abs(t.clientX - startX) > 14 || Math.abs(t.clientY - startY) > 14) cancelHold();
      };
      const release = () => { cancelHold(); };
      pad.addEventListener('touchstart', down, { passive: false });
      pad.addEventListener('touchend', release);
      pad.addEventListener('touchmove', move, { passive: true });
      pad.addEventListener('touchcancel', release);
      pad.addEventListener('mousedown', down);
      pad.addEventListener('mouseup', release);
      pad.addEventListener('mouseleave', release);
      grid.appendChild(pad);
    }
  }
  function triggerPad(i) { sendBinary([0x90, i, 127]); }
  function flash(el) { el.classList.add('hit'); setTimeout(() => el.classList.remove('hit'), 90); }
  function flashPadEl(idx) {
    const pads = document.querySelectorAll('#padsGrid .m-pad');
    if (pads[idx]) flash(pads[idx]);
  }
  let _syncFlashTimer = null;
  function syncFlashPads(step) {
    if (!isPlaying) return;
    const pads = document.querySelectorAll('#padsGrid .m-pad');
    const toFlash = [];
    for (let t = 0; t < 16; t++) {
      if (seqState[t][step] && pads[t]) { pads[t].classList.add('sync-flash'); toFlash.push(pads[t]); }
    }
    if (!toFlash.length) return;
    if (_syncFlashTimer) clearTimeout(_syncFlashTimer);
    _syncFlashTimer = setTimeout(() => { toFlash.forEach(e => e.classList.remove('sync-flash')); _syncFlashTimer = null; }, 110);
  }
  // ---- Tremolo (hold >= 160ms) ----------------------------------------
  function startTremolo(padIdx, el) {
    stopTremolo();
    _tremoloPad = padIdx;
    _tremoloEl = el;
    el.classList.add('tremolo');
    const fire = () => {
      if (_tremoloPad < 0) return;
      triggerPad(_tremoloPad);
      tremoloPunch(_tremoloEl);
      _tremoloTimer = setTimeout(fire, Math.round(60000 / bpm / 4));
    };
    _tremoloTimer = setTimeout(fire, Math.round(60000 / bpm / 4));
  }
  function stopTremolo() {
    if (_tremoloTimer) { clearTimeout(_tremoloTimer); _tremoloTimer = null; }
    if (_tremoloEl) { _tremoloEl.classList.remove('tremolo', 'tremolo-hit'); _tremoloEl = null; }
    _tremoloPad = -1;
  }
  function tremoloPunch(el) {
    if (!el) return;
    el.classList.remove('tremolo-hit');
    void el.offsetWidth;
    el.classList.add('tremolo-hit');
  }


  // =====================================================================
  // Piano
  // =====================================================================
  function initPiano() {
    $('octUp').addEventListener('click', () => { octave = Math.min(7, octave + 1); $('octVal').textContent = octave; buildPiano(); });
    $('octDown').addEventListener('click', () => { octave = Math.max(1, octave - 1); $('octVal').textContent = octave; buildPiano(); });
    $('octVal').textContent = octave;
    // Selector de engine
    const pianoSection = $('view-piano');
    const engBar = document.createElement('div');
    engBar.className = 'm-engine-sel';
    engBar.id = 'engineSel';
    PIANO_ENGINES.forEach(({ id, label }) => {
      const b = document.createElement('button');
      b.textContent = label;
      b.dataset.engine = String(id);
      b.classList.toggle('active', id === pianoEngine);
      b.addEventListener('click', () => {
        pianoEngine = id;
        document.querySelectorAll('#engineSel button').forEach(x => x.classList.toggle('active', +x.dataset.engine === id));
      });
      engBar.appendChild(b);
    });
    pianoSection.querySelector('.m-piano-bar').after(engBar);
    buildPiano();
  }
  function buildPiano() {
    const piano = $('piano');
    piano.innerHTML = '';
    const wPct = 100 / 7;
    for (let o = 0; o < 2; o++) {         // 2 filas apiladas = 2 octavas
      const row = document.createElement('div');
      row.className = 'm-piano-row';
      const oct = octave + o;
      for (let w = 0; w < 7; w++) {
        const note = oct * 12 + WHITE_OFFSETS[w];
        const key = document.createElement('div');
        key.className = 'm-key';
        key.dataset.note = note;
        key.style.setProperty('--kc', currentPalette[(o * 7 + w) % 16]);
        row.appendChild(key);
      }
      for (let w = 0; w < 7; w++) {
        if (!(w in BLACK)) continue;
        const note = oct * 12 + BLACK[w];
        const key = document.createElement('div');
        key.className = 'm-key black';
        key.dataset.note = note;
        key.style.left = `calc(${(w + 1) * wPct}% - 3.5%)`;
        row.appendChild(key);
      }
      piano.appendChild(row);
    }
    bindGlissando(piano);
  }
  // Notas que suenan actualmente (por nota MIDI), para el glissando.
  const _activeNotes = new Set();
  function noteOn(note) {
    if (_activeNotes.has(note)) return;
    _activeNotes.add(note);
    send({ cmd: 'synthNoteOnEx', engine: pianoEngine, note, velocity: 110, accent: false, slide: false });
  }
  function noteOff(note) {
    if (!_activeNotes.has(note)) return;
    _activeNotes.delete(note);
    send({ cmd: 'synthNoteOff', engine: pianoEngine, track: 255, note });
  }
  function keyElFromPoint(x, y) {
    const el = document.elementFromPoint(x, y);
    if (el && el.classList && el.classList.contains('m-key')) return el;
    return null;
  }
  // Permite tocar varias teclas a la vez y deslizar el dedo (glissando "tirirri").
  function bindGlissando(piano) {
    // Estado por dedo (pointerId): { el, note } o { el:null, note:null } si est├í fuera de teclas.
    const fingerKey = new Map();
    const stopNote = (st) => {
      if (st && st.el) { st.el.classList.remove('down'); noteOff(st.note); }
    };
    const moveTo = (pid, x, y) => {
      const el = keyElFromPoint(x, y);
      const note = el ? +el.dataset.note : null;
      const prev = fingerKey.get(pid);
      if (prev && prev.note === note) return;       // misma tecla, nada que hacer
      stopNote(prev);                                // soltar la anterior
      if (el) { el.classList.add('down'); noteOn(note); }
      fingerKey.set(pid, { el, note });
    };
    const release = (pid) => { stopNote(fingerKey.get(pid)); fingerKey.delete(pid); };
    piano.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      try { piano.setPointerCapture(e.pointerId); } catch (_) {}
      moveTo(e.pointerId, e.clientX, e.clientY);
    });
    piano.addEventListener('pointermove', (e) => {
      if (!fingerKey.has(e.pointerId)) return;       // solo dedos ya pulsados
      e.preventDefault();
      moveTo(e.pointerId, e.clientX, e.clientY);
    });
    const end = (e) => release(e.pointerId);
    piano.addEventListener('pointerup', end);
    piano.addEventListener('pointercancel', end);
  }

  // =====================================================================
  // Sequencer (grid de puntos de colores)
  // =====================================================================
  function initSeq() {
    $('seqClear').addEventListener('click', clearPattern);
    $('seqMuteAll').addEventListener('click', () => setAllMute(true));
    $('seqUnmuteAll').addEventListener('click', () => setAllMute(false));
    buildSeqGrid();
  }
  function applyStepCount(c) {
    if (!STEP_COUNTS.includes(c)) return;
    stepCount = c;
    document.querySelectorAll('#stepCountSel button').forEach((b) =>
      b.classList.toggle('active', parseInt(b.dataset.count, 10) === c));
    buildSeqGrid();
    refreshSeqDots();
  }
  function buildSeqGrid() {
    const grid = $('seqGrid');
    grid.innerHTML = '';
    for (let t = 0; t < 16; t++) {
      const row = document.createElement('div');
      row.className = 'm-seq-row';
      const label = document.createElement('div');
      label.className = 'm-seq-label';
      label.textContent = TRACK_NAMES[t];
      label.style.color = currentPalette[t];
      row.appendChild(label);
      // Botones Mute / Solo por pista
      const mBtn = document.createElement('button');
      mBtn.className = 'm-trk-btn m-trk-mute';
      mBtn.textContent = 'M';
      mBtn.dataset.track = t;
      mBtn.classList.toggle('on', muteState[t]);
      mBtn.addEventListener('click', () => toggleMute(t));
      row.appendChild(mBtn);
      const sBtn = document.createElement('button');
      sBtn.className = 'm-trk-btn m-trk-solo';
      sBtn.textContent = 'S';
      sBtn.dataset.track = t;
      sBtn.classList.toggle('on', soloState[t]);
      sBtn.addEventListener('click', () => toggleSolo(t));
      row.appendChild(sBtn);
      for (let s = 0; s < stepCount; s++) {
        const dot = document.createElement('div');
        dot.className = 'm-dot-cell' + (s % 4 === 0 ? ' beat' : '');
        dot.style.setProperty('--dot-c', currentPalette[t]);
        dot.dataset.track = t; dot.dataset.step = s;
        dot.addEventListener('click', () => toggleStep(t, s, dot));
        row.appendChild(dot);
      }
      grid.appendChild(row);
    }
  }
  function toggleMute(t) {
    muteState[t] = !muteState[t];
    send({ cmd: 'mute', track: t, value: muteState[t] });
    refreshTrackButtons();
  }
  function toggleSolo(t) {
    const newVal = !soloState[t];
    // Solo exclusivo: apagar solos de las demas pistas primero
    if (newVal) {
      for (let i = 0; i < 16; i++) {
        if (i !== t && soloState[i]) {
          soloState[i] = false;
          send({ cmd: 'solo', track: i, value: false });
        }
      }
    }
    soloState[t] = newVal;
    send({ cmd: 'solo', track: t, value: newVal });
    refreshTrackButtons();
  }
  function setAllMute(on) {
    for (let t = 0; t < 16; t++) muteState[t] = on;
    // Env├¡o at├│mico (una sola orden) para evitar parpadeos.
    send({ cmd: 'setMuteMask', mask: on ? 0xFFFF : 0 });
    refreshTrackButtons();
  }
  function refreshTrackButtons() {
    document.querySelectorAll('#seqGrid .m-trk-mute').forEach((b) =>
      b.classList.toggle('on', !!muteState[+b.dataset.track]));
    document.querySelectorAll('#seqGrid .m-trk-solo').forEach((b) =>
      b.classList.toggle('on', !!soloState[+b.dataset.track]));
  }
  function toggleStep(t, s, dot) {
    const active = !seqState[t][s];
    seqState[t][s] = active;
    dot.classList.toggle('on', active);
    send({ cmd: 'setStep', track: t, step: s, active, noteLen: 1 });
  }
  function refreshSeqDots() {
    document.querySelectorAll('#seqGrid .m-dot-cell').forEach((dot) => {
      const t = +dot.dataset.track, s = +dot.dataset.step;
      dot.classList.toggle('on', !!seqState[t][s]);
    });
  }
  function clearPattern() {
    for (let t = 0; t < 16; t++) {
      for (let s = 0; s < stepCount; s++) {
        if (seqState[t][s]) {
          seqState[t][s] = false;
          send({ cmd: 'setStep', track: t, step: s, active: false, noteLen: 1 });
        }
      }
    }
    refreshSeqDots();
  }
  let _lastPlayhead = -1;
  function highlightPlayhead(step) {
    if (step === _lastPlayhead) return;
    document.querySelectorAll('#seqGrid .m-dot-cell.playhead').forEach((d) => d.classList.remove('playhead'));
    document.querySelectorAll(`#seqGrid .m-dot-cell[data-step="${step}"]`).forEach((d) => d.classList.add('playhead'));
    _lastPlayhead = step;
  }

  // =====================================================================
  // Filtros
  // =====================================================================
  function initFx() {
    const grid = $('fxPresets');
    FX_PRESETS.forEach((p, i) => {
      const b = document.createElement('button');
      b.className = 'm-fx-preset';
      b.dataset.id = p.id;
      b.dataset.fxi = String(i);
      b.style.setProperty('--fx-c', fxColor(i));
      b.innerHTML = `<span class="fx-emoji">${p.emoji}</span>${p.name}`;
      b.addEventListener('click', () => applyKidFx(p.id));
      grid.appendChild(b);
    });
    $('tono').addEventListener('input', () => {
      onTono();
      if (_tonoDebounce) clearTimeout(_tonoDebounce);
      _tonoDebounce = setTimeout(() => { _tonoDebounce = null; onTonoSend(); }, 40);
    });
    renderFx();
    onTono(); // pinta la etiqueta inicial
  }
  // Slider Tono 0..100 -> escala el cutoff base del preset (0.3x .. 3x).
  function tonoCutoff() {
    const p = FX_PRESETS.find((x) => x.id === activeFxPreset) || FX_PRESETS[0];
    if (p.filterType === 0) return 4000;
    const v = parseFloat($('tono').value) / 100;   // 0..1
    const factor = Math.pow(2, (v - 0.5) * 3.4);    // ~0.3x .. ~3.2x
    return Math.round(Math.max(60, Math.min(16000, p.cutoff * factor)));
  }
  function applyKidFx(id) {
    activeFxPreset = id;
    const p = FX_PRESETS.find((x) => x.id === id) || FX_PRESETS[0];
    const cutoff = tonoCutoff();
    for (let t = 0; t < 16; t++) {
      if (p.filterType === 0) {
        send({ cmd: 'clearTrackFilter', track: t });
      } else {
        const msg = { cmd: 'setTrackFilter', track: t, filterType: p.filterType, cutoff, resonance: p.res, gain: p.gain || 0 };
        send(msg);
      }
    }
    renderFx();
  }
  function onTono() {
    const v = parseFloat($('tono').value);
    $('tonoVal').textContent = v < 33 ? 'grave' : (v > 66 ? 'agudo' : 'medio');
  }
  function onTonoSend() {
    const p = FX_PRESETS.find((x) => x.id === activeFxPreset);
    if (p && p.filterType > 0) {
      const cutoff = tonoCutoff();
      for (let t = 0; t < 16; t++) {
        send({ cmd: 'setTrackFilter', track: t, filterType: p.filterType, cutoff, resonance: p.res, gain: p.gain || 0 });
      }
    }
  }
  function renderFx() {
    document.querySelectorAll('#fxPresets .m-fx-preset').forEach((b) =>
      b.classList.toggle('active', b.dataset.id === activeFxPreset));
  }

  // =====================================================================
  // Temas visuales
  // =====================================================================
  function initTheme() {
    const opts = $('themeOptions');
    THEMES.forEach((t) => {
      const b = document.createElement('button');
      b.className = 'm-theme-opt';
      b.dataset.theme = t.id;
      b.innerHTML = `<span class="m-theme-swatch" style="--sw:${t.color}"></span>${t.name}`;
      b.addEventListener('click', () => { applyTheme(t.id); closeThemeSheet(); });
      opts.appendChild(b);
    });
    let saved = 'red';
    try { saved = localStorage.getItem(THEME_KEY) || 'red'; } catch (_) {}
    if (!THEMES.some((t) => t.id === saved)) saved = 'red';
    // Migrar tema 'violet' antiguo ÔåÆ 'retro'
    if (saved === 'violet') saved = 'retro';
    applyTheme(saved);

    $('themeBtn').addEventListener('click', openThemeSheet);
    $('sheetBackdrop').addEventListener('click', closeThemeSheet);
    $('sampleBackdrop').addEventListener('click', closeSampleSheet);
    setupSampleUpload();
  }
  function applyTheme(id) {
    document.body.dataset.theme = id;
    try { localStorage.setItem(THEME_KEY, id); } catch (_) {}
    currentPalette = PALETTES[id] || PALETTES.red;
    document.querySelectorAll('#themeOptions .m-theme-opt').forEach((b) =>
      b.classList.toggle('active', b.dataset.theme === id));
    // Re-pintar pads con la paleta del tema
    document.querySelectorAll('#padsGrid .m-pad').forEach((pad, i) =>
      pad.style.setProperty('--pad-c', currentPalette[i]));
    // Re-pintar etiquetas y dots del sequencer
    document.querySelectorAll('#seqGrid .m-seq-row').forEach((row, t) => {
      const lbl = row.querySelector('.m-seq-label');
      if (lbl) lbl.style.color = currentPalette[t];
      row.querySelectorAll('.m-dot-cell').forEach((dot) =>
        dot.style.setProperty('--dot-c', currentPalette[t]));
    });
    // Re-pintar teclas del piano
    document.querySelectorAll('#piano .m-piano-row').forEach((row, o) => {
      row.querySelectorAll('.m-key:not(.black)').forEach((k, w) =>
        k.style.setProperty('--kc', currentPalette[(o * 7 + w) % 16]));
    });
    // Re-pintar presets de filtros
    document.querySelectorAll('#fxPresets .m-fx-preset').forEach((b) =>
      b.style.setProperty('--fx-c', fxColor(+b.dataset.fxi)));
  }
  function openThemeSheet() { $('themeSheet').hidden = false; $('sheetBackdrop').hidden = false; }
  function closeThemeSheet() { $('themeSheet').hidden = true; $('sheetBackdrop').hidden = true; }

  // =====================================================================
  // JAM ÔÇö canvas multitouch (samples + synth) con efectos visuales
  // =====================================================================
  let jamCanvas = null, jamCtx = null, jamW = 0, jamH = 0, jamDpr = 1;
  let jamRaf = 0, jamLastT = 0, jamRunning = false, jamMode = 'samples';
  const jamParticles = [];
  const jamRings = [];
  const jamTouch = new Map(); // pointerId -> { zone, note|null }
  const JAM_COLS = 4, JAM_ROWS = 4; // 16 zonas = 16 pads
  // 16 notas (escala mayor, 2 octavas) para el modo synth.
  const JAM_NOTES = [48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 74];

  function hexToRgb(hex) {
    const n = parseInt(hex.slice(1), 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }

  function initJam() {
    jamCanvas = $('jamCanvas');
    jamCtx = jamCanvas.getContext('2d');
    jamCanvas.addEventListener('pointerdown', jamDown);
    jamCanvas.addEventListener('pointermove', jamMove);
    jamCanvas.addEventListener('pointerup', jamUp);
    jamCanvas.addEventListener('pointercancel', jamUp);
    jamCanvas.addEventListener('pointerleave', jamUp);
    $('jamModeSamples').addEventListener('click', () => setJamMode('samples'));
    $('jamModeSynth').addEventListener('click', () => setJamMode('synth'));
    window.addEventListener('resize', () => { if (jamRunning) jamResize(); });
  }
  function setJamMode(m) {
    jamReleaseAll();
    jamMode = m;
    $('jamModeSamples').classList.toggle('active', m === 'samples');
    $('jamModeSynth').classList.toggle('active', m === 'synth');
  }
  function jamResize() {
    jamDpr = Math.min(window.devicePixelRatio || 1, 2);
    const r = jamCanvas.getBoundingClientRect();
    jamW = r.width; jamH = r.height;
    jamCanvas.width = Math.round(jamW * jamDpr);
    jamCanvas.height = Math.round(jamH * jamDpr);
    jamCtx.setTransform(jamDpr, 0, 0, jamDpr, 0, 0);
  }
  function jamSetActive(on) {
    if (on) {
      jamResize();
      if (!jamRunning) { jamRunning = true; jamLastT = performance.now(); jamRaf = requestAnimationFrame(jamLoop); }
    } else {
      jamRunning = false;
      if (jamRaf) { cancelAnimationFrame(jamRaf); jamRaf = 0; }
      jamReleaseAll();
    }
  }
  function jamZoneAt(x, y) {
    const col = Math.min(JAM_COLS - 1, Math.max(0, Math.floor(x / jamW * JAM_COLS)));
    const row = Math.min(JAM_ROWS - 1, Math.max(0, Math.floor(y / jamH * JAM_ROWS)));
    return row * JAM_COLS + col;
  }
  function jamTrigger(zone, x, y) {
    spawnJamBurst(x, y, hexToRgb(PALETTE[zone]));
    if (jamMode === 'samples') {
      triggerPad(zone);
      return null;
    }
    const note = JAM_NOTES[zone];
    send({ cmd: 'synthNoteOnEx', engine: PIANO_ENGINE, note, velocity: 110, accent: false, slide: false });
    return note;
  }
  function jamPos(e) {
    const r = jamCanvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }
  function jamDown(e) {
    e.preventDefault();
    const t = $('jamTip'); if (t) t.classList.add('hide');
    const { x, y } = jamPos(e);
    const zone = jamZoneAt(x, y);
    const note = jamTrigger(zone, x, y);
    jamTouch.set(e.pointerId, { zone, note });
  }
  function jamMove(e) {
    const st = jamTouch.get(e.pointerId);
    if (!st) return;
    e.preventDefault();
    const { x, y } = jamPos(e);
    const zone = jamZoneAt(x, y);
    if (zone === st.zone) return; // mismo sitio ÔåÆ no re-disparar
    if (st.note !== null) send({ cmd: 'synthNoteOff', engine: PIANO_ENGINE, track: 255, note: st.note });
    st.note = jamTrigger(zone, x, y);
    st.zone = zone;
  }
  function jamUp(e) {
    const st = jamTouch.get(e.pointerId);
    if (!st) return;
    if (st.note !== null) send({ cmd: 'synthNoteOff', engine: PIANO_ENGINE, track: 255, note: st.note });
    jamTouch.delete(e.pointerId);
  }
  function jamReleaseAll() {
    jamTouch.forEach((st) => {
      if (st.note !== null) send({ cmd: 'synthNoteOff', engine: PIANO_ENGINE, track: 255, note: st.note });
    });
    jamTouch.clear();
  }
  function spawnJamBurst(x, y, rgb) {
    jamRings.push({ x, y, r: 8, life: 1, rgb });
    for (let i = 0; i < 16; i++) {
      const a = (Math.PI * 2 * i) / 16 + Math.random() * 0.4;
      const sp = 60 + Math.random() * 200;
      jamParticles.push({ x, y, vx: Math.cos(a) * sp, vy: Math.sin(a) * sp, life: 1, r: 3 + Math.random() * 4, rgb });
    }
  }
  function jamLoop(t) {
    if (!jamRunning) { jamRaf = 0; return; }
    jamRaf = requestAnimationFrame(jamLoop);
    const dt = Math.min(0.05, (t - jamLastT) / 1000); jamLastT = t;
    const ctx = jamCtx;
    // estela: capa semitransparente para que los trazos se desvanezcan
    ctx.fillStyle = 'rgba(8,11,16,0.28)';
    ctx.fillRect(0, 0, jamW, jamH);
    // gu├¡as suaves en el centro de cada zona
    const cw = jamW / JAM_COLS, ch = jamH / JAM_ROWS;
    for (let z = 0; z < JAM_COLS * JAM_ROWS; z++) {
      const cx = ((z % JAM_COLS) + 0.5) * cw, cy = (Math.floor(z / JAM_COLS) + 0.5) * ch;
      const c = hexToRgb(PALETTE[z]);
      ctx.beginPath(); ctx.arc(cx, cy, 6, 0, Math.PI * 2);
      ctx.fillStyle = `rgba(${c[0]},${c[1]},${c[2]},0.18)`; ctx.fill();
    }
    // anillos
    for (let i = jamRings.length - 1; i >= 0; i--) {
      const rg = jamRings[i];
      rg.r += dt * 260; rg.life -= dt * 1.6;
      if (rg.life <= 0) { jamRings.splice(i, 1); continue; }
      ctx.beginPath(); ctx.arc(rg.x, rg.y, rg.r, 0, Math.PI * 2);
      ctx.strokeStyle = `rgba(${rg.rgb[0]},${rg.rgb[1]},${rg.rgb[2]},${rg.life * 0.6})`;
      ctx.lineWidth = 3 * rg.life + 0.5; ctx.stroke();
    }
    // part├¡culas
    for (let i = jamParticles.length - 1; i >= 0; i--) {
      const p = jamParticles[i];
      p.x += p.vx * dt; p.y += p.vy * dt; p.vx *= 0.94; p.vy *= 0.94;
      p.life -= dt * 1.1;
      if (p.life <= 0) { jamParticles.splice(i, 1); continue; }
      ctx.beginPath(); ctx.arc(p.x, p.y, Math.max(0.5, p.r * p.life), 0, Math.PI * 2);
      ctx.fillStyle = `rgba(${p.rgb[0]},${p.rgb[1]},${p.rgb[2]},${p.life * 0.9})`;
      ctx.fill();
    }
  }

  // =====================================================================
  // Cargar sample en un pad (long-press)
  // =====================================================================
  let sampleTargetPad = -1;
  let sampleFamily = '';
  function openSampleSheet(pad) {
    sampleTargetPad = pad;
    $('sampleTargetName').textContent = `${pad + 1} ┬À ${TRACK_NAMES[pad]}`;
    // Chips de familias
    const chips = $('famChips');
    chips.innerHTML = '';
    TRACK_NAMES.forEach((fam) => {
      const b = document.createElement('button');
      b.textContent = fam;
      b.dataset.fam = fam;
      b.addEventListener('click', () => requestSamples(fam));
      chips.appendChild(b);
    });
    $('sampleList').innerHTML = '<div class="se-empty">CargandoÔÇª</div>';
    $('sampleSheet').hidden = false;
    $('sampleBackdrop').hidden = false;
    requestSamples(TRACK_NAMES[pad]); // familia por defecto = la del pad
  }
  function closeSampleSheet() {
    $('sampleSheet').hidden = true;
    $('sampleBackdrop').hidden = true;
    sampleTargetPad = -1;
  }
  function requestSamples(fam) {
    sampleFamily = fam;
    document.querySelectorAll('#famChips button').forEach((b) =>
      b.classList.toggle('active', b.dataset.fam === fam));
    $('sampleList').innerHTML = '<div class="se-empty">CargandoÔÇª</div>';
    send({ cmd: 'getSamples', family: fam, pad: sampleTargetPad });
  }
  function renderSampleList(d) {
    if (sampleTargetPad < 0 || d.family !== sampleFamily) return;
    const list = $('sampleList');
    list.innerHTML = '';
    const samples = d.samples || [];
    if (!samples.length) {
      list.innerHTML = '<div class="se-empty">Sin sonidos en esta familia</div>';
      return;
    }
    samples.forEach((s) => {
      const b = document.createElement('button');
      const kb = s.size ? ` <span>${Math.round(s.size / 1024)}KB</span>` : '';
      b.innerHTML = `<span>${s.name}</span>${kb}`;
      b.addEventListener('click', () => {
        send({ cmd: 'loadSample', family: d.family, filename: s.name, pad: sampleTargetPad });
        triggerPad(sampleTargetPad);
        closeSampleSheet();
      });
      list.appendChild(b);
    });
  }

  // =====================================================================
  // Subir un sample (WAV/MP3) desde el dispositivo del usuario al pad
  // =====================================================================
  function setupSampleUpload() {
    const btn = $('sampleUploadBtn');
    const input = $('sampleFileInput');
    if (!btn || !input) return;
    btn.addEventListener('click', () => { if (sampleTargetPad >= 0) input.click(); });
    input.addEventListener('change', async () => {
      const file = input.files && input.files[0];
      input.value = '';                 // permite re-subir el mismo archivo
      if (!file || sampleTargetPad < 0) return;
      await uploadSampleFile(file, sampleTargetPad);
    });
  }
  function setUploadStatus(msg, kind) {
    const el = $('sampleUploadStatus');
    if (!el) return;
    el.hidden = !msg;
    el.textContent = msg || '';
    el.className = 'm-upload-status' + (kind ? ' ' + kind : '');
  }
  async function uploadSampleFile(file, pad) {
    try {
      const isWav = /\.wav$/i.test(file.name) || file.type === 'audio/wav' || file.type === 'audio/x-wav';
      let blob, name;
      if (isWav && file.size <= 8 * 1024 * 1024) {
        blob = file;
        name = file.name;
      } else {
        setUploadStatus('Convirtiendo audioÔÇª', '');
        const wav = await decodeToWav(file);
        blob = wav;
        name = file.name.replace(/\.[^.]+$/, '') + '.wav';
        if (blob.size > 8 * 1024 * 1024) { setUploadStatus('Archivo muy grande tras convertir (m├íx 8MB)', 'err'); return; }
      }
      setUploadStatus('SubiendoÔÇª 0%', '');
      await postSample(blob, name, pad);
    } catch (err) {
      setUploadStatus('Error: ' + (err && err.message ? err.message : err), 'err');
    }
  }
  function postSample(blob, name, pad) {
    return new Promise((resolve, reject) => {
      const fd = new FormData();
      fd.append('file', blob, name);
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/upload?pad=' + pad);
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) setUploadStatus('SubiendoÔÇª ' + Math.round((e.loaded / e.total) * 100) + '%', '');
      };
      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          setUploadStatus('Ô£à ' + name + ' cargado en pad ' + (pad + 1), 'ok');
          triggerPad(pad);
          setTimeout(() => { setUploadStatus('', ''); closeSampleSheet(); }, 1200);
          resolve();
        } else {
          reject(new Error('HTTP ' + xhr.status));
        }
      };
      xhr.onerror = () => reject(new Error('fallo de red'));
      xhr.send(fd);
    });
  }
  // Decodifica cualquier audio (MP3, etc.) y lo re-codifica a WAV PCM 16-bit.
  async function decodeToWav(file) {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) throw new Error('navegador sin Web Audio');
    const ctx = new AC();
    try {
      const buf = await file.arrayBuffer();
      const audio = await ctx.decodeAudioData(buf);
      return encodeWav(audio);
    } finally {
      try { ctx.close(); } catch (_) {}
    }
  }
  function encodeWav(audio) {
    const numCh = Math.min(2, audio.numberOfChannels);
    const sr = audio.sampleRate;
    const len = audio.length;
    const chans = [];
    for (let c = 0; c < numCh; c++) chans.push(audio.getChannelData(c));
    const bytesPerSample = 2;
    const blockAlign = numCh * bytesPerSample;
    const dataLen = len * blockAlign;
    const buffer = new ArrayBuffer(44 + dataLen);
    const view = new DataView(buffer);
    const wstr = (off, s) => { for (let i = 0; i < s.length; i++) view.setUint8(off + i, s.charCodeAt(i)); };
    wstr(0, 'RIFF'); view.setUint32(4, 36 + dataLen, true); wstr(8, 'WAVE');
    wstr(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true);
    view.setUint16(22, numCh, true); view.setUint32(24, sr, true);
    view.setUint32(28, sr * blockAlign, true); view.setUint16(32, blockAlign, true);
    view.setUint16(34, 16, true);
    wstr(36, 'data'); view.setUint32(40, dataLen, true);
    let off = 44;
    for (let i = 0; i < len; i++) {
      for (let c = 0; c < numCh; c++) {
        let s = Math.max(-1, Math.min(1, chans[c][i]));
        view.setInt16(off, s < 0 ? s * 0x8000 : s * 0x7fff, true);
        off += 2;
      }
    }
    return new Blob([buffer], { type: 'audio/wav' });
  }

  // =====================================================================
  // Init
  // =====================================================================
  function init() {
    initTheme();
    initNav();
    initTransport();
    initPadCountBar();
    buildPads();
    initPiano();
    initJam();
    initSeq();
    initFx();
    connect();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
