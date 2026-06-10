/*
 * sample-editor.js
 * Modal de edición de samples: waveform, trim, fade in/out, preview play, upload
 * RED808 Drum Machine
 */

// ============================================
// SAMPLE EDITOR MODAL
// ============================================

const SampleEditor = (() => {

  // Estado interno
  let _state = {
    padIndex: -1,
    file: null,
    audioBuffer: null,       // AudioBuffer decodificado
    audioCtx: null,
    sourceNode: null,
    isPlaying: false,
    animFrame: null,
    peaks: [],               // [[max,min], ...] normalizados 0-1
    trimStart: 0,            // 0.0 - 1.0
    trimEnd: 1,              // 0.0 - 1.0
    fadeIn: 0,               // 0.0 - 1.0 (relativo al rango trim)
    fadeOut: 0,              // 0.0 - 1.0 (relativo al rango trim)
    dragMode: null,          // 'trimStart'|'trimEnd'|'fadeIn'|'fadeOut'
    dragStartX: 0,
    dragStartVal: 0,
    canvasW: 0,
  };

  // Constantes
  const DAISY_MAX_BYTES = 8 * 1024 * 1024;   // 8MB
  const DAISY_PADS = 24;
  const DAISY_TOTAL_BYTES = DAISY_MAX_BYTES * DAISY_PADS;

  // ---- DOM refs (lazy) ----
  const $ = id => document.getElementById(id);

  // ============================================
  // OPEN / CLOSE
  // ============================================

  function open(padIndex, file) {
    _state.padIndex = padIndex;
    _state.file = file;
    _state.trimStart = 0;
    _state.trimEnd = 1;
    _state.fadeIn = 0;
    _state.fadeOut = 0;
    _state.isPlaying = false;
    _state.peaks = [];

    _ensureDOM();
    _show();
    _loadFile(file);
  }

  function close() {
    _stopPlay();
    if ($('se-overlay')) $('se-overlay').style.display = 'none';
    _state.file = null;
    _state.audioBuffer = null;
  }

  // ============================================
  // DOM CREATION (once)
  // ============================================

  function _ensureDOM() {
    if ($('se-overlay')) return;

    const overlay = document.createElement('div');
    overlay.id = 'se-overlay';
    overlay.innerHTML = `
<div id="se-modal">
  <div id="se-header">
    <span id="se-title">Sample Editor</span>
    <button id="se-close-btn" title="Cerrar">✕</button>
  </div>

  <!-- INFO BAR -->
  <div id="se-info-bar">
    <span id="se-info-file">—</span>
    <span id="se-info-dur">—</span>
    <span id="se-info-sr">—</span>
    <span id="se-info-ch">—</span>
    <span id="se-info-bits">—</span>
    <span id="se-info-size">—</span>
  </div>

  <!-- DAISY SPACE BAR -->
  <div id="se-daisy-bar">
    <span id="se-daisy-label">Espacio Daisy (pad actual):</span>
    <div id="se-daisy-track">
      <div id="se-daisy-fill"></div>
    </div>
    <span id="se-daisy-pct">—</span>
  </div>

  <!-- WAVEFORM CANVAS -->
  <div id="se-canvas-wrap">
    <canvas id="se-canvas"></canvas>
    <!-- Trim handles -->
    <div id="se-handle-start" class="se-handle se-handle-start" title="Trim Start"></div>
    <div id="se-handle-end"   class="se-handle se-handle-end"   title="Trim End"></div>
    <!-- Fade handles (inner, top) -->
    <div id="se-handle-fi" class="se-handle se-handle-fade" title="Fade In"></div>
    <div id="se-handle-fo" class="se-handle se-handle-fade se-handle-fade-out" title="Fade Out"></div>
    <!-- Playhead -->
    <div id="se-playhead"></div>
  </div>

  <!-- TRIM / FADE CONTROLS -->
  <div id="se-controls">
    <div class="se-ctrl-group">
      <label>Trim Start</label>
      <input type="range" id="se-trim-start" min="0" max="1000" value="0" step="1">
      <span id="se-trim-start-val">0.000s</span>
    </div>
    <div class="se-ctrl-group">
      <label>Trim End</label>
      <input type="range" id="se-trim-end" min="0" max="1000" value="1000" step="1">
      <span id="se-trim-end-val">—</span>
    </div>
    <div class="se-ctrl-group">
      <label>Fade In</label>
      <input type="range" id="se-fade-in" min="0" max="500" value="0" step="1">
      <span id="se-fade-in-val">0.000s</span>
    </div>
    <div class="se-ctrl-group">
      <label>Fade Out</label>
      <input type="range" id="se-fade-out" min="0" max="500" value="0" step="1">
      <span id="se-fade-out-val">0.000s</span>
    </div>
  </div>

  <!-- TRIM RESULT INFO -->
  <div id="se-trim-info">
    <span id="se-trim-dur">Duración trim: —</span>
    <span id="se-trim-bytes">Tamaño: —</span>
    <span id="se-trim-fit" class="se-fit-ok">✓ Cabe en Daisy</span>
  </div>

  <!-- ACTION BUTTONS -->
  <div id="se-actions">
    <button id="se-play-btn" class="se-btn se-btn-play">▶ Play</button>
    <button id="se-reset-btn" class="se-btn se-btn-reset">↺ Reset</button>
    <div style="flex:1"></div>
    <button id="se-cancel-btn" class="se-btn se-btn-cancel">Cancelar</button>
    <button id="se-import-btn" class="se-btn se-btn-import" disabled>⬆ Importar</button>
  </div>

  <!-- PROGRESS (hidden until import) -->
  <div id="se-progress-wrap" style="display:none">
    <div id="se-progress-bar"><div id="se-progress-fill"></div></div>
    <span id="se-progress-label">Subiendo...</span>
  </div>
</div>`;
    document.body.appendChild(overlay);

    _injectStyles();
    _bindEvents();
  }

  function _injectStyles() {
    if ($('se-styles')) return;
    const s = document.createElement('style');
    s.id = 'se-styles';
    s.textContent = `
#se-overlay {
  display:none; position:fixed; inset:0; background:rgba(0,0,0,0.82);
  z-index:9000; display:flex; align-items:center; justify-content:center;
}
#se-modal {
  background:linear-gradient(160deg,#12141e 0%,#0a0c14 100%);
  border:1px solid rgba(255,255,255,0.12); border-radius:12px;
  width:min(860px,96vw); max-height:92vh; overflow-y:auto;
  box-shadow:0 8px 48px rgba(0,0,0,0.8), 0 0 0 1px rgba(255,102,0,0.18);
  padding:0 0 16px 0; display:flex; flex-direction:column; gap:0;
}
#se-header {
  display:flex; align-items:center; justify-content:space-between;
  padding:14px 20px 10px; border-bottom:1px solid rgba(255,255,255,0.08);
}
#se-title { color:#ff6600; font-size:15px; font-weight:700; font-family:monospace; letter-spacing:1px; }
#se-close-btn {
  background:none; border:none; color:#888; font-size:18px; cursor:pointer;
  padding:2px 6px; border-radius:4px;
}
#se-close-btn:hover { color:#fff; background:rgba(255,255,255,0.08); }

#se-info-bar {
  display:flex; gap:16px; flex-wrap:wrap; padding:8px 20px;
  border-bottom:1px solid rgba(255,255,255,0.06);
}
#se-info-bar span {
  font-family:monospace; font-size:11px; color:#aaa;
  background:rgba(255,255,255,0.05); border-radius:4px; padding:2px 8px;
}

#se-daisy-bar {
  display:flex; align-items:center; gap:10px; padding:6px 20px;
  border-bottom:1px solid rgba(255,255,255,0.06);
}
#se-daisy-label { font-size:11px; color:#666; font-family:monospace; white-space:nowrap; }
#se-daisy-track {
  flex:1; height:8px; background:rgba(255,255,255,0.07); border-radius:4px; overflow:hidden;
}
#se-daisy-fill { height:100%; background:#ff6600; border-radius:4px; transition:width .3s; width:0%; }
#se-daisy-pct { font-family:monospace; font-size:11px; color:#ff6600; min-width:44px; text-align:right; }

#se-canvas-wrap {
  position:relative; margin:12px 20px 0; border-radius:8px; overflow:hidden;
  border:1px solid rgba(255,255,255,0.1); background:#080a10;
  cursor:crosshair;
}
#se-canvas { display:block; width:100%; height:160px; }
#se-playhead {
  position:absolute; top:0; bottom:0; width:2px; background:#fff;
  pointer-events:none; display:none; left:0;
  box-shadow:0 0 6px rgba(255,255,255,0.6);
}

.se-handle {
  position:absolute; top:0; bottom:0; width:10px;
  cursor:ew-resize; z-index:2;
}
.se-handle-start { left:0; background:rgba(0,255,136,0.35); border-right:2px solid #00ff88; }
.se-handle-end   { right:0; background:rgba(0,255,136,0.35); border-left:2px solid #00ff88; }
.se-handle-fade  { top:0; bottom:auto; height:24px; width:10px; background:rgba(255,200,0,0.5);
  border-bottom:2px solid #ffcc00; border-radius:0 0 4px 0; }
.se-handle-fade-out { right:0; left:auto; border-radius:0 0 0 4px; }

#se-controls {
  display:grid; grid-template-columns:1fr 1fr; gap:4px 20px;
  padding:10px 20px 0;
}
.se-ctrl-group { display:flex; align-items:center; gap:8px; }
.se-ctrl-group label { font-family:monospace; font-size:11px; color:#888; min-width:68px; }
.se-ctrl-group input[type=range] { flex:1; accent-color:#ff6600; }
.se-ctrl-group span { font-family:monospace; font-size:11px; color:#ff9944; min-width:54px; text-align:right; }

#se-trim-info {
  display:flex; gap:16px; flex-wrap:wrap; padding:8px 20px 0;
}
#se-trim-info span { font-family:monospace; font-size:11px; color:#aaa; }
.se-fit-ok  { color:#00ff88 !important; }
.se-fit-bad { color:#ff3333 !important; }

#se-actions {
  display:flex; gap:10px; align-items:center; padding:14px 20px 0;
}
.se-btn {
  border:none; border-radius:6px; padding:8px 18px; font-size:13px;
  font-family:monospace; cursor:pointer; font-weight:600; transition:all .15s;
}
.se-btn-play   { background:#00ff88; color:#000; }
.se-btn-play:hover { background:#00ffaa; }
.se-btn-play.playing { background:#ff3366; color:#fff; }
.se-btn-reset  { background:rgba(255,255,255,0.1); color:#ccc; }
.se-btn-reset:hover { background:rgba(255,255,255,0.2); }
.se-btn-cancel { background:rgba(255,255,255,0.08); color:#888; }
.se-btn-cancel:hover { background:rgba(255,255,255,0.15); color:#fff; }
.se-btn-import { background:#ff6600; color:#fff; }
.se-btn-import:hover:not(:disabled) { background:#ff8833; }
.se-btn-import:disabled { background:rgba(255,102,0,0.25); color:#555; cursor:not-allowed; }

#se-progress-wrap { padding:10px 20px 0; }
#se-progress-bar { height:8px; background:rgba(255,255,255,0.1); border-radius:4px; overflow:hidden; margin-bottom:4px; }
#se-progress-fill { height:100%; background:#ff6600; width:0%; transition:width .2s; border-radius:4px; }
#se-progress-label { font-family:monospace; font-size:11px; color:#888; }

@media (max-width:600px) {
  #se-controls { grid-template-columns:1fr; }
  #se-actions { flex-wrap:wrap; }
}`;
    document.head.appendChild(s);
  }

  // ============================================
  // EVENTS
  // ============================================

  function _bindEvents() {
    $('se-close-btn').onclick  = close;
    $('se-cancel-btn').onclick = close;
    $('se-overlay').onclick = e => { if (e.target === $('se-overlay')) close(); };

    $('se-play-btn').onclick  = _togglePlay;
    $('se-reset-btn').onclick = _resetTrim;
    $('se-import-btn').onclick = _doImport;

    $('se-trim-start').oninput = () => {
      let v = parseInt($('se-trim-start').value) / 1000;
      if (v >= _state.trimEnd - 0.01) v = _state.trimEnd - 0.01;
      _state.trimStart = v;
      _syncSliders();
      _redrawCanvas();
    };
    $('se-trim-end').oninput = () => {
      let v = parseInt($('se-trim-end').value) / 1000;
      if (v <= _state.trimStart + 0.01) v = _state.trimStart + 0.01;
      _state.trimEnd = v;
      _syncSliders();
      _redrawCanvas();
    };
    $('se-fade-in').oninput = () => {
      _state.fadeIn = parseInt($('se-fade-in').value) / 1000;
      _syncSliders();
      _redrawCanvas();
    };
    $('se-fade-out').oninput = () => {
      _state.fadeOut = parseInt($('se-fade-out').value) / 1000;
      _syncSliders();
      _redrawCanvas();
    };

    // Canvas drag for trim handles
    const wrap = $('se-canvas-wrap');
    wrap.addEventListener('mousedown',  _onCanvasDragStart);
    wrap.addEventListener('touchstart', _onCanvasTouchStart, {passive:false});
    document.addEventListener('mousemove',  _onDragMove);
    document.addEventListener('mouseup',    _onDragEnd);
    document.addEventListener('touchmove',  _onTouchMove, {passive:false});
    document.addEventListener('touchend',   _onDragEnd);

    window.addEventListener('resize', () => {
      _resizeCanvas();
      _redrawCanvas();
    });
  }

  // ============================================
  // SHOW / HIDE
  // ============================================

  function _show() {
    const ov = $('se-overlay');
    ov.style.display = 'flex';

    // Título
    const padName = (typeof padNames !== 'undefined') ? padNames[_state.padIndex] : `Pad ${_state.padIndex + 1}`;
    $('se-title').textContent = `Sample Editor — ${padName}`;

    // Reset progress
    $('se-progress-wrap').style.display = 'none';
    $('se-progress-fill').style.width = '0%';
    $('se-import-btn').disabled = true;

    _resizeCanvas();
  }

  function _resizeCanvas() {
    const canvas = $('se-canvas');
    if (!canvas) return;
    const wrap = $('se-canvas-wrap');
    canvas.width  = wrap.clientWidth  || 800;
    canvas.height = 160;
    _state.canvasW = canvas.width;
  }

  // ============================================
  // FILE LOADING & DECODING
  // ============================================

  function _loadFile(file) {
    // Show loading state
    $('se-info-file').textContent = `📄 ${file.name}`;
    $('se-info-size').textContent = `📦 ${_fmtBytes(file.size)}`;

    // Decode with Web Audio API
    if (!_state.audioCtx) {
      _state.audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }

    const reader = new FileReader();
    reader.onload = async e => {
      try {
        const ab = e.target.result;
        const audioBuf = await _state.audioCtx.decodeAudioData(ab.slice(0));
        _state.audioBuffer = audioBuf;

        // Info
        const dur = audioBuf.duration;
        $('se-info-dur').textContent  = `⏱ ${dur.toFixed(3)}s`;
        $('se-info-sr').textContent   = `🎵 ${audioBuf.sampleRate} Hz`;
        $('se-info-ch').textContent   = `🔊 ${audioBuf.numberOfChannels === 1 ? 'Mono' : 'Stereo'}`;
        $('se-info-bits').textContent = `📐 16-bit`;

        // Daisy space
        _updateDaisyBar(file.size);

        // Build peaks
        _state.peaks = _buildPeaks(audioBuf, _state.canvasW || 800);

        // Reset trim
        _state.trimEnd = 1;
        $('se-trim-end').value = 1000;
        $('se-trim-start').value = 0;
        $('se-fade-in').value = 0;
        $('se-fade-out').value = 0;
        _syncSliders();
        _redrawCanvas();

        $('se-import-btn').disabled = false;
      } catch(err) {
        console.error('[SampleEditor] decode error', err);
        $('se-info-dur').textContent = '❌ Error decodificando';
      }
    };
    reader.readAsArrayBuffer(file);
  }

  function _buildPeaks(audioBuf, numBins) {
    const data = audioBuf.getChannelData(0);  // left/mono
    const step = Math.floor(data.length / numBins) || 1;
    const peaks = [];
    for (let i = 0; i < numBins; i++) {
      let mx = 0, mn = 0;
      for (let j = 0; j < step; j++) {
        const s = data[i * step + j] || 0;
        if (s > mx) mx = s;
        if (s < mn) mn = s;
      }
      peaks.push([mx, mn]);
    }
    return peaks;
  }

  // ============================================
  // WAVEFORM DRAW
  // ============================================

  function _redrawCanvas() {
    const canvas = $('se-canvas');
    if (!canvas || _state.peaks.length === 0) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;

    // Background grid
    ctx.clearRect(0, 0, w, h);
    const bg = ctx.createLinearGradient(0, 0, 0, h);
    bg.addColorStop(0, 'rgba(20,22,32,0.97)');
    bg.addColorStop(1, 'rgba(8,10,18,0.99)');
    ctx.fillStyle = bg;
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = 'rgba(255,255,255,0.04)';
    ctx.lineWidth = 1;
    for (let x = 0; x < w; x += 32) { ctx.beginPath(); ctx.moveTo(x+.5,0); ctx.lineTo(x+.5,h); ctx.stroke(); }
    for (let y = 0; y < h; y += 20) { ctx.beginPath(); ctx.moveTo(0,y+.5); ctx.lineTo(w,y+.5); ctx.stroke(); }

    const peaks = _state.peaks;
    const ts = _state.trimStart, te = _state.trimEnd;
    const fi = _state.fadeIn,   fo = _state.fadeOut;
    const midY = h / 2, scale = midY * 0.88;
    const step = w / peaks.length;

    // Center line
    ctx.strokeStyle = 'rgba(255,255,255,0.14)';
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, midY); ctx.lineTo(w, midY); ctx.stroke();

    // Draw full waveform dimmed (outside trim)
    ctx.globalAlpha = 0.18;
    _drawWaveShape(ctx, peaks, w, h, midY, scale, step, '#00ff88');
    ctx.globalAlpha = 1;

    // Draw trim region with full brightness
    const sx = ts * w, ex = te * w;
    ctx.save();
    ctx.beginPath();
    ctx.rect(sx, 0, ex - sx, h);
    ctx.clip();

    // Fade overlay inside trim
    const fiX = sx + fi * (ex - sx);
    const foX = ex - fo * (ex - sx);

    // Fade in gradient
    if (fi > 0) {
      const grad = ctx.createLinearGradient(sx, 0, fiX, 0);
      grad.addColorStop(0, 'rgba(0,0,0,0.82)');
      grad.addColorStop(1, 'rgba(0,0,0,0)');
      // applied after waveform
    }

    _drawWaveShape(ctx, peaks, w, h, midY, scale, step, '#00ff88');

    // Fade in dark overlay
    if (fi > 0) {
      const gfi = ctx.createLinearGradient(sx, 0, fiX, 0);
      gfi.addColorStop(0, 'rgba(0,0,0,0.80)');
      gfi.addColorStop(1, 'rgba(0,0,0,0)');
      ctx.fillStyle = gfi;
      ctx.fillRect(sx, 0, fiX - sx, h);
      // Fade In label
      ctx.fillStyle = '#ffcc00';
      ctx.font = '9px monospace';
      ctx.fillText('FI', sx + 4, 14);
    }
    if (fo > 0) {
      const gfo = ctx.createLinearGradient(foX, 0, ex, 0);
      gfo.addColorStop(0, 'rgba(0,0,0,0)');
      gfo.addColorStop(1, 'rgba(0,0,0,0.80)');
      ctx.fillStyle = gfo;
      ctx.fillRect(foX, 0, ex - foX, h);
      ctx.fillStyle = '#ffcc00';
      ctx.font = '9px monospace';
      ctx.fillText('FO', ex - 22, 14);
    }

    ctx.restore();

    // Trim lines
    ctx.strokeStyle = '#00ff88';
    ctx.lineWidth = 2;
    ctx.setLineDash([]);
    ctx.beginPath(); ctx.moveTo(sx, 0); ctx.lineTo(sx, h); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(ex, 0); ctx.lineTo(ex, h); ctx.stroke();

    // Dim before start
    ctx.fillStyle = 'rgba(0,0,0,0.55)';
    if (sx > 0) ctx.fillRect(0, 0, sx, h);
    if (ex < w) ctx.fillRect(ex, 0, w - ex, h);

    // Handle arrows
    _drawHandleArrow(ctx, sx, h, 'right', '#00ff88');
    _drawHandleArrow(ctx, ex, h, 'left', '#00ff88');

    // Time labels
    if (_state.audioBuffer) {
      const dur = _state.audioBuffer.duration;
      ctx.fillStyle = '#00ff88';
      ctx.font = '10px monospace';
      ctx.fillText(_fmtTime(ts * dur), sx + 4, h - 6);
      const endLabel = _fmtTime(te * dur);
      ctx.fillText(endLabel, ex - ctx.measureText(endLabel).width - 4, h - 6);
    }

    // Update handle positions
    _positionHandles();
  }

  function _drawWaveShape(ctx, peaks, w, h, midY, scale, step, color) {
    ctx.beginPath();
    for (let i = 0; i < peaks.length; i++) {
      const x = i * step;
      const y = midY - peaks[i][0] * scale;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    for (let i = peaks.length - 1; i >= 0; i--) {
      ctx.lineTo(i * step, midY - peaks[i][1] * scale);
    }
    ctx.closePath();
    const grad = ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, _hexRgba(color, 0.75));
    grad.addColorStop(0.5, _hexRgba(color, 0.28));
    grad.addColorStop(1, _hexRgba(color, 0.75));
    ctx.fillStyle = grad;
    ctx.fill();

    ctx.strokeStyle = _hexRgba(color, 0.9);
    ctx.lineWidth = 1;
    ctx.shadowColor = _hexRgba(color, 0.4);
    ctx.shadowBlur = 5;
    ctx.beginPath();
    for (let i = 0; i < peaks.length; i++) {
      const x = i * step, y = midY - peaks[i][0] * scale;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.beginPath();
    for (let i = 0; i < peaks.length; i++) {
      const x = i * step, y = midY - peaks[i][1] * scale;
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.shadowBlur = 0;
  }

  function _drawHandleArrow(ctx, x, h, dir, color) {
    const aw = 7, ah = 12, cy = h / 2;
    ctx.fillStyle = color;
    ctx.beginPath();
    if (dir === 'right') {
      ctx.moveTo(x, cy - ah/2);
      ctx.lineTo(x + aw, cy);
      ctx.lineTo(x, cy + ah/2);
    } else {
      ctx.moveTo(x, cy - ah/2);
      ctx.lineTo(x - aw, cy);
      ctx.lineTo(x, cy + ah/2);
    }
    ctx.closePath();
    ctx.fill();
  }

  function _positionHandles() {
    const wrap = $('se-canvas-wrap');
    if (!wrap) return;
    const W = wrap.clientWidth;
    const ts = _state.trimStart, te = _state.trimEnd;

    const hs = $('se-handle-start');
    const he = $('se-handle-end');
    const hfi = $('se-handle-fi');
    const hfo = $('se-handle-fo');

    if (hs) hs.style.left  = `${ts * W - 5}px`;
    if (he) he.style.right = `${(1 - te) * W - 5}px`;

    const sx = ts * W, ex = te * W;
    if (hfi) hfi.style.left = `${sx + _state.fadeIn * (ex - sx) - 5}px`;
    if (hfo) hfo.style.right = `${W - ex + _state.fadeOut * (ex - sx) - 5}px`;
  }

  // ============================================
  // DRAG (trim handles on canvas)
  // ============================================

  function _xToNorm(clientX) {
    const wrap = $('se-canvas-wrap');
    const rect = wrap.getBoundingClientRect();
    return Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
  }

  function _onCanvasDragStart(e) {
    const norm = _xToNorm(e.clientX);
    _startDrag(norm);
  }
  function _onCanvasTouchStart(e) {
    e.preventDefault();
    const norm = _xToNorm(e.touches[0].clientX);
    _startDrag(norm);
  }
  function _startDrag(norm) {
    const ts = _state.trimStart, te = _state.trimEnd;
    const distStart = Math.abs(norm - ts);
    const distEnd   = Math.abs(norm - te);
    const thr = 0.04;
    if (distStart < thr && distStart <= distEnd) { _state.dragMode = 'trimStart'; }
    else if (distEnd < thr) { _state.dragMode = 'trimEnd'; }
    else { _state.dragMode = null; }
  }
  function _onDragMove(e) {
    if (!_state.dragMode) return;
    const norm = _xToNorm(e.clientX);
    _applyDrag(norm);
  }
  function _onTouchMove(e) {
    if (!_state.dragMode) return;
    e.preventDefault();
    const norm = _xToNorm(e.touches[0].clientX);
    _applyDrag(norm);
  }
  function _applyDrag(norm) {
    if (_state.dragMode === 'trimStart') {
      _state.trimStart = Math.max(0, Math.min(norm, _state.trimEnd - 0.01));
      $('se-trim-start').value = Math.round(_state.trimStart * 1000);
    } else if (_state.dragMode === 'trimEnd') {
      _state.trimEnd = Math.min(1, Math.max(norm, _state.trimStart + 0.01));
      $('se-trim-end').value = Math.round(_state.trimEnd * 1000);
    }
    _syncSliders();
    _redrawCanvas();
  }
  function _onDragEnd() { _state.dragMode = null; }

  // ============================================
  // SLIDER SYNC & INFO UPDATE
  // ============================================

  function _syncSliders() {
    if (!_state.audioBuffer) return;
    const dur = _state.audioBuffer.duration;
    const ts = _state.trimStart, te = _state.trimEnd;
    const fi = _state.fadeIn, fo = _state.fadeOut;
    const trimDur = (te - ts) * dur;

    $('se-trim-start-val').textContent = _fmtTime(ts * dur);
    $('se-trim-end-val').textContent   = _fmtTime(te * dur);
    $('se-fade-in-val').textContent    = _fmtTime(fi * trimDur);
    $('se-fade-out-val').textContent   = _fmtTime(fo * trimDur);

    // Estimate output bytes: trimDur * sampleRate * 2 bytes * channels (mono mixdown)
    const sr = _state.audioBuffer.sampleRate;
    const outBytes = Math.round(trimDur * sr * 2);
    $('se-trim-dur').textContent   = `Duración: ${trimDur.toFixed(3)}s`;
    $('se-trim-bytes').textContent = `Tamaño: ${_fmtBytes(outBytes)}`;

    const fits = outBytes <= DAISY_MAX_BYTES;
    const fitEl = $('se-trim-fit');
    if (fits) {
      fitEl.textContent = `✓ Cabe en Daisy (${_pct(outBytes, DAISY_MAX_BYTES)}%)`;
      fitEl.className = 'se-fit-ok';
    } else {
      fitEl.textContent = `✗ Excede límite Daisy (max ${_fmtBytes(DAISY_MAX_BYTES)})`;
      fitEl.className = 'se-fit-bad';
    }

    _updateDaisyBar(outBytes);

    // Only enable import if it fits
    const importBtn = $('se-import-btn');
    if (importBtn && _state.audioBuffer) importBtn.disabled = !fits;
  }

  function _updateDaisyBar(bytes) {
    const pct = Math.min(100, bytes / DAISY_MAX_BYTES * 100);
    $('se-daisy-fill').style.width = pct + '%';
    $('se-daisy-fill').style.background = pct > 90 ? '#ff3333' : pct > 70 ? '#ffcc00' : '#ff6600';
    $('se-daisy-pct').textContent = pct.toFixed(1) + '%';
  }

  // ============================================
  // RESET
  // ============================================

  function _resetTrim() {
    _state.trimStart = 0;
    _state.trimEnd = 1;
    _state.fadeIn = 0;
    _state.fadeOut = 0;
    $('se-trim-start').value = 0;
    $('se-trim-end').value = 1000;
    $('se-fade-in').value = 0;
    $('se-fade-out').value = 0;
    _syncSliders();
    _redrawCanvas();
  }

  // ============================================
  // PLAYBACK
  // ============================================

  function _togglePlay() {
    if (_state.isPlaying) { _stopPlay(); return; }
    if (!_state.audioBuffer) return;

    const ctx = _state.audioCtx;
    const buf = _state.audioBuffer;
    const dur = buf.duration;
    const start = _state.trimStart * dur;
    const end   = _state.trimEnd   * dur;

    const src = ctx.createBufferSource();
    src.buffer = buf;

    // Apply gain with fade in/out
    const gainNode = ctx.createGain();
    gainNode.gain.setValueAtTime(0, ctx.currentTime);

    const trimDur = end - start;
    const fiDur = _state.fadeIn * trimDur;
    const foDur = _state.fadeOut * trimDur;

    if (fiDur > 0) {
      gainNode.gain.linearRampToValueAtTime(1, ctx.currentTime + fiDur);
    } else {
      gainNode.gain.setValueAtTime(1, ctx.currentTime);
    }
    if (foDur > 0) {
      gainNode.gain.setValueAtTime(1, ctx.currentTime + trimDur - foDur);
      gainNode.gain.linearRampToValueAtTime(0, ctx.currentTime + trimDur);
    }

    src.connect(gainNode);
    gainNode.connect(ctx.destination);
    src.start(0, start, end - start);

    _state.sourceNode = src;
    _state.isPlaying = true;
    _state._playStartCtxTime = ctx.currentTime;
    _state._playDuration = end - start;

    $('se-play-btn').textContent = '■ Stop';
    $('se-play-btn').classList.add('playing');
    $('se-playhead').style.display = 'block';

    src.onended = () => {
      if (_state.isPlaying) _stopPlay();
    };

    _animatePlayhead();
  }

  function _stopPlay() {
    if (_state.sourceNode) {
      try { _state.sourceNode.stop(); } catch(e) {}
      _state.sourceNode = null;
    }
    _state.isPlaying = false;
    if (_state.animFrame) { cancelAnimationFrame(_state.animFrame); _state.animFrame = null; }
    $('se-play-btn').textContent = '▶ Play';
    $('se-play-btn').classList.remove('playing');
    $('se-playhead').style.display = 'none';
  }

  function _animatePlayhead() {
    const wrap = $('se-canvas-wrap');
    const ph = $('se-playhead');
    if (!wrap || !ph) return;

    const ctx = _state.audioCtx;
    const W = wrap.clientWidth;
    const ts = _state.trimStart, te = _state.trimEnd;

    function frame() {
      if (!_state.isPlaying) return;
      const elapsed = ctx.currentTime - _state._playStartCtxTime;
      const progress = elapsed / _state._playDuration;
      const x = (ts + progress * (te - ts)) * W;
      ph.style.left = x + 'px';
      _state.animFrame = requestAnimationFrame(frame);
    }
    _state.animFrame = requestAnimationFrame(frame);
  }

  // ============================================
  // IMPORT (build trimmed WAV + upload)
  // ============================================

  function _doImport() {
    if (!_state.audioBuffer || !_state.file) return;

    const buf = _state.audioBuffer;
    const dur = buf.duration;
    const sr  = buf.sampleRate;
    const trimStart = _state.trimStart;
    const trimEnd   = _state.trimEnd;
    const fadeIn    = _state.fadeIn;
    const fadeOut   = _state.fadeOut;

    // Build offline trimmed + fade buffer
    const startSample = Math.floor(trimStart * buf.length);
    const endSample   = Math.ceil(trimEnd   * buf.length);
    const trimLen     = endSample - startSample;
    const numCh       = buf.numberOfChannels;

    // Mixdown to mono int16
    const int16 = new Int16Array(trimLen);
    for (let i = 0; i < trimLen; i++) {
      let s = 0;
      for (let ch = 0; ch < numCh; ch++) {
        s += buf.getChannelData(ch)[startSample + i];
      }
      s /= numCh;

      // Apply fades
      const fiSamples = Math.floor(fadeIn  * trimLen);
      const foSamples = Math.floor(fadeOut * trimLen);
      if (i < fiSamples && fiSamples > 0) s *= i / fiSamples;
      if ((trimLen - i) < foSamples && foSamples > 0) s *= (trimLen - i) / foSamples;

      int16[i] = Math.max(-32768, Math.min(32767, Math.round(s * 32767)));
    }

    const wavBlob = _encodeWAV(int16, sr, 1);

    // Show progress
    $('se-progress-wrap').style.display = 'block';
    $('se-import-btn').disabled = true;
    $('se-play-btn').disabled = true;
    _stopPlay();

    const formData = new FormData();
    formData.append('file', wavBlob, _state.file.name);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', `/api/upload?pad=${_state.padIndex}`);

    xhr.upload.onprogress = e => {
      if (e.lengthComputable) {
        const pct = Math.round(e.loaded / e.total * 100);
        $('se-progress-fill').style.width = pct + '%';
        $('se-progress-label').textContent = `Subiendo... ${pct}%`;
      }
    };

    xhr.onload = () => {
      $('se-progress-label').textContent = 'Procesando en ESP32...';
      // El resultado llega por WebSocket (handleUploadComplete)
    };

    xhr.onerror = () => {
      $('se-progress-label').textContent = '❌ Error de conexión';
      $('se-import-btn').disabled = false;
      $('se-play-btn').disabled = false;
    };

    xhr.send(formData);

    // Escuchar evento de WS para cerrar modal al completar
    _waitForUploadComplete();
  }

  function _waitForUploadComplete() {
    const pad = _state.padIndex;
    // Override handleUploadComplete temporalmente
    const original = window.handleUploadComplete;
    window.handleUploadComplete = function(data) {
      if (data.pad === pad) {
        window.handleUploadComplete = original;
        if (data.success) {
          $('se-progress-fill').style.width = '100%';
          $('se-progress-label').textContent = '✓ Importado correctamente';
          setTimeout(() => close(), 1200);
        } else {
          $('se-progress-label').textContent = `❌ ${data.message}`;
          $('se-import-btn').disabled = false;
          $('se-play-btn').disabled = false;
        }
        // Call original for toast + pad refresh
        if (original) original(data);
      } else {
        if (original) original(data);
      }
    };
  }

  // ============================================
  // WAV ENCODER
  // ============================================

  function _encodeWAV(samples, sampleRate, numChannels) {
    const bytesPerSample = 2;
    const blockAlign = numChannels * bytesPerSample;
    const byteRate = sampleRate * blockAlign;
    const dataSize = samples.length * bytesPerSample;
    const buffer = new ArrayBuffer(44 + dataSize);
    const view = new DataView(buffer);

    function writeStr(offset, str) {
      for (let i = 0; i < str.length; i++) view.setUint8(offset + i, str.charCodeAt(i));
    }
    writeStr(0, 'RIFF');
    view.setUint32(4,  36 + dataSize, true);
    writeStr(8, 'WAVE');
    writeStr(12, 'fmt ');
    view.setUint32(16, 16, true);
    view.setUint16(20, 1,  true);   // PCM
    view.setUint16(22, numChannels, true);
    view.setUint32(24, sampleRate, true);
    view.setUint32(28, byteRate, true);
    view.setUint16(32, blockAlign, true);
    view.setUint16(34, 16, true);   // bits
    writeStr(36, 'data');
    view.setUint32(40, dataSize, true);
    for (let i = 0; i < samples.length; i++) {
      view.setInt16(44 + i * 2, samples[i], true);
    }
    return new Blob([buffer], { type: 'audio/wav' });
  }

  // ============================================
  // HELPERS
  // ============================================

  function _fmtBytes(b) {
    if (b >= 1024*1024) return (b/1024/1024).toFixed(2) + ' MB';
    if (b >= 1024)      return (b/1024).toFixed(1) + ' KB';
    return b + ' B';
  }
  function _fmtTime(s) {
    if (s >= 60) return `${Math.floor(s/60)}:${(s%60).toFixed(2).padStart(5,'0')}`;
    return s.toFixed(3) + 's';
  }
  function _pct(v, total) { return (v/total*100).toFixed(1); }
  function _hexRgba(hex, a) {
    const c = hex.replace('#','');
    const r = parseInt(c.slice(0,2),16), g = parseInt(c.slice(2,4),16), b = parseInt(c.slice(4,6),16);
    return `rgba(${r},${g},${b},${a})`;
  }

  // ============================================
  // PUBLIC API
  // ============================================

  return { open, close };

})();

window.SampleEditor = SampleEditor;
