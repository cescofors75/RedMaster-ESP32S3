/* =========================================================================
   RED808 Mobile — lógica mínima para tocar desde el móvil.
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
  // 16 colores fijos (uno por pista/pad), legibles sobre fondo oscuro.
  const PALETTE = [
    '#ff3b30','#ff9500','#ffcc00','#34c759','#00e5ff','#0a84ff','#5e5ce6','#bf5af2',
    '#ff2d55','#ff6b3d','#a3d900','#30d158','#64d2ff','#5ac8fa','#af52de','#ff375f'
  ];
  // Paletas por tema visual (16 colores, uno por pista).
  const PALETTES = {
    red:    PALETTE,
    neon:   ['#00e5ff','#ff2d95','#7dff3a','#ffe600','#4488ff','#ff5fa0','#bf5af2','#34c759',
             '#00cfff','#ff80c0','#4dffa6','#ffd000','#36c5f0','#ff3385','#d070ff','#00ffcc'],
    acid:   ['#00ff88','#c8ff00','#39ff14','#ffe600','#80ff00','#4dffa6','#b3ff00','#00ffc8',
             '#a3f000','#f5e642','#00ff55','#e6ff00','#44ff88','#aaff00','#66ff33','#00ffaa'],
    sunset: ['#ff7a18','#ff2d55','#ff9d4d','#ff3d7f','#ffcc00','#ff6b3d','#ff5500','#ff9966',
             '#ffc18a','#ff1493','#ff8c00','#ff3366','#ffaa44','#ff6699','#ff7733','#ff4080'],
    retro:  ['#f5a623','#e74c3c','#f39c12','#d35400','#e67e22','#c0392b','#f1c40f','#ca6f1e',
             '#eb984e','#d68910','#f5b041','#a93226','#fad7a0','#dc7633','#f0b27a','#e59866']
  };
  let currentPalette = PALETTE;
  // Efectos "modo niño": presets con nombre divertido aplicados a TODAS las
  // pistas a la vez. `type` es el filtro que espera el firmware (1..9), `res`
  // la resonancia. El cutoff lo controla el slider "Tono".
  const FX_PRESETS = [
    { id: 'off',    name: 'Normal',    emoji: '🎵', color: '#5b6472', type: 0,  cutoff: 4000, res: 1   },
    { id: 'sub',    name: 'Submarino', emoji: '🌊', color: '#0a84ff', type: 1,  cutoff: 220,  res: 7   },
    { id: 'bright', name: 'Brillante', emoji: '✨', color: '#ffcc00', type: 2,  cutoff: 2800, res: 3   },
    { id: 'phone',  name: 'Teléfono',  emoji: '📞', color: '#ff9500', type: 3,  cutoff: 1400, res: 9   },
    { id: 'robot',  name: 'Robot',     emoji: '🤖', color: '#34c759', type: 9,  cutoff: 900,  res: 16  },
    { id: 'tunnel', name: 'Túnel',     emoji: '🕳️', color: '#bf5af2', type: 14, cutoff: 420,  res: 18  }
  ];
  const STEP_COUNTS = [16, 32, 64];
  // Piano: una octava = 12 semitonos. Patrón de teclas negras.
  const WHITE_OFFSETS = [0, 2, 4, 5, 7, 9, 11];
  const BLACK = { 0: 1, 1: 3, 3: 6, 4: 8, 5: 10 }; // posición blanca -> semitono negro
  // Colores arcoíris para las 7 teclas blancas (modo niño).
  const WHITE_COLORS = ['#ff4d4d', '#ff9f40', '#ffd633', '#4dd964', '#36c5f0', '#5b8def', '#b06bf0'];
  const PIANO_ENGINES = [
    { id: 3, label: '303' },
    { id: 4, label: 'WT' },
    { id: 5, label: 'SH101' },
    { id: 6, label: 'FM2OP' }
  ];
  let pianoEngine = 3;
  // Temas visuales (re-pintan los tokens RED808 vía body[data-theme]).
  const THEMES = [
    { id: 'red',    name: 'RED808', color: '#ff3b30' },
    { id: 'neon',   name: 'Neon',   color: '#00e5ff' },
    { id: 'acid',   name: 'Acid',   color: '#00ff88' },
    { id: 'sunset', name: 'Sunset', color: '#ff7a18' },
    { id: 'retro',  name: 'Retro',  color: '#f5a623' }
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
  let activeFxPreset = 'off';
  const seqState = []; // seqState[track][step] = bool
  for (let t = 0; t < 16; t++) seqState.push(new Array(64).fill(false));

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
      if (ev.data instanceof ArrayBuffer) return; // niveles de audio: ignorar en móvil
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
      if (d.pattern !== undefined) setPatternSel(d.pattern, false);
      return;
    }
    if (type === 'pattern') {
      if (d.stepCount && d.stepCount !== stepCount) applyStepCount(d.stepCount);
      loadPattern(d);
      if (d.index !== undefined) setPatternSel(d.index, false);
      return;
    }
    if (type === 'step') { highlightPlayhead(d.step); syncFlashPads(d.step); return; }
    if (type === 'pad') { flashPadEl(d.pad); return; }
    if (type === 'stepCount' && d.count) { applyStepCount(d.count); return; }
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
  // Navegación de vistas
  // =====================================================================
  function initNav() {
    document.querySelectorAll('.m-tab').forEach((tab) => {
      tab.addEventListener('click', () => {
        const view = tab.dataset.view;
        document.querySelectorAll('.m-tab').forEach((t) => t.classList.toggle('is-active', t === tab));
        document.querySelectorAll('.m-view').forEach((v) => {
          v.classList.toggle('is-active', v.id === 'view-' + view);
        });
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

    const sel = $('patternSel');
    PATTERN_NAMES.forEach((name, i) => {
      const o = document.createElement('option');
      o.value = String(i); o.textContent = name; sel.appendChild(o);
    });
    sel.addEventListener('change', () => {
      const idx = parseInt(sel.value, 10);
      send({ cmd: 'selectPattern', index: idx });
      setTimeout(() => send({ cmd: 'getPattern' }), 120);
    });
  }
  function setPlaying(on, doSend) {
    isPlaying = on;
    $('playBtn').classList.toggle('playing', on);
    $('playBtn').textContent = on ? '⏹' : '▶';
    if (doSend) send({ cmd: on ? 'start' : 'stop' });
  }
  function setBpm(v, doSend) {
    bpm = Math.max(40, Math.min(300, Math.round(v)));
    $('bpmVal').textContent = String(bpm);
    if (doSend) send({ cmd: 'tempo', value: bpm });
  }
  function setPatternSel(idx, doSend) {
    if ($('patternSel').value !== String(idx)) $('patternSel').value = String(idx);
    if (doSend) send({ cmd: 'selectPattern', index: idx });
  }

  // =====================================================================
  // Pads
  // =====================================================================
  function initPads() {
    const grid = $('padsGrid');
    for (let i = 0; i < 16; i++) {
      const pad = document.createElement('button');
      pad.className = 'm-pad';
      pad.style.setProperty('--pad-c', currentPalette[i]);
      pad.innerHTML = `${TRACK_NAMES[i]}<small>${i + 1}</small>`;
      const hit = (e) => { e.preventDefault(); triggerPad(i); flash(pad); };
      pad.addEventListener('touchstart', hit, { passive: false });
      pad.addEventListener('mousedown', hit);
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
        key.style.setProperty('--kc', WHITE_COLORS[w]);
        bindKey(key, note);
        row.appendChild(key);
      }
      for (let w = 0; w < 7; w++) {
        if (!(w in BLACK)) continue;
        const note = oct * 12 + BLACK[w];
        const key = document.createElement('div');
        key.className = 'm-key black';
        key.style.left = `calc(${(w + 1) * wPct}% - 3.5%)`;
        bindKey(key, note);
        row.appendChild(key);
      }
      piano.appendChild(row);
    }
  }
  function bindKey(el, note) {
    const down = (e) => {
      e.preventDefault();
      el.classList.add('down');
      send({ cmd: 'synthNoteOnEx', engine: pianoEngine, note, velocity: 110, accent: false, slide: false });
    };
    const up = (e) => {
      if (e) e.preventDefault();
      el.classList.remove('down');
      send({ cmd: 'synthNoteOff', engine: pianoEngine, track: 255, note });
    };
    el.addEventListener('touchstart', down, { passive: false });
    el.addEventListener('touchend', up, { passive: false });
    el.addEventListener('touchcancel', up, { passive: false });
    el.addEventListener('mousedown', down);
    el.addEventListener('mouseup', up);
    el.addEventListener('mouseleave', (e) => { if (el.classList.contains('down')) up(e); });
  }

  // =====================================================================
  // Sequencer (grid de puntos de colores)
  // =====================================================================
  function initSeq() {
    $('seqClear').addEventListener('click', clearPattern);
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
    FX_PRESETS.forEach((p) => {
      const b = document.createElement('button');
      b.className = 'm-fx-preset';
      b.dataset.id = p.id;
      b.style.setProperty('--fx-c', p.color);
      b.innerHTML = `<span class="fx-emoji">${p.emoji}</span>${p.name}`;
      b.addEventListener('click', () => applyKidFx(p.id));
      grid.appendChild(b);
    });
    $('tono').addEventListener('input', onTono);
    renderFx();
    onTono(); // pinta la etiqueta inicial
  }
  // Slider Tono 0..100 -> cutoff según preset (curva exponencial, más musical).
  function tonoCutoff() {
    const p = FX_PRESETS.find((x) => x.id === activeFxPreset) || FX_PRESETS[0];
    if (p.type === 0) return 4000;
    const v = parseFloat($('tono').value) / 100;
    // Rango dinámico por preset: submarina baja frecuencia, brillante alta
    const lo = p.type === 1 ? 80  : p.type === 14 ? 100 : 200;
    const hi = p.type === 2 ? 12000 : p.type === 14 ? 1200 : 6000;
    return Math.round(lo * Math.pow(hi / lo, v));
  }
  function applyKidFx(id) {
    activeFxPreset = id;
    const p = FX_PRESETS.find((x) => x.id === id) || FX_PRESETS[0];
    const cutoff = tonoCutoff();
    for (let t = 0; t < 16; t++) {
      if (p.type === 0) {
        send({ cmd: 'clearTrackFilter', track: t });
      } else {
        send({ cmd: 'setTrackFilter', track: t, type: p.type, cutoff, resonance: p.res, gain: 0 });
      }
    }
    renderFx();
  }
  function onTono() {
    const v = parseFloat($('tono').value);
    $('tonoVal').textContent = v < 33 ? 'grave' : (v > 66 ? 'agudo' : 'medio');
    const p = FX_PRESETS.find((x) => x.id === activeFxPreset);
    if (p && p.type > 0) {
      const cutoff = tonoCutoff();
      for (let t = 0; t < 16; t++) {
        send({ cmd: 'setTrackFilter', track: t, type: p.type, cutoff, resonance: p.res, gain: 0 });
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
    // Migrar tema 'violet' antiguo → 'retro'
    if (saved === 'violet') saved = 'retro';
    applyTheme(saved);

    $('themeBtn').addEventListener('click', openThemeSheet);
    $('sheetBackdrop').addEventListener('click', closeThemeSheet);
  }
  function applyTheme(id) {
    document.body.dataset.theme = id;
    try { localStorage.setItem(THEME_KEY, id); } catch (_) {}
    currentPalette = PALETTES[id] || PALETTE;
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
  }
  function openThemeSheet() { $('themeSheet').hidden = false; $('sheetBackdrop').hidden = false; }
  function closeThemeSheet() { $('themeSheet').hidden = true; $('sheetBackdrop').hidden = true; }

  // =====================================================================
  // Init
  // =====================================================================
  function init() {
    initTheme();
    initNav();
    initTransport();
    initPads();
    initPiano();
    initSeq();
    initFx();
    connect();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
