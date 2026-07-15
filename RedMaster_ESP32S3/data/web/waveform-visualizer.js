/*
 * waveform-visualizer.js
 * Motor de renderizado de forma de onda para RED808 Drum Machine
 * Tres modos: Sample Browser (estático), FX Edit (dual A/B), Mixer (mini-scopes)
 */

// ============================================
// WAVEFORM RENDERER ENGINE
// ============================================

const WaveformRenderer = {
  // Track colors matching the existing pad color scheme
  trackColors: [
    '#ff3366', '#ff6633', '#ffcc00', '#33ff66',
    '#00ccff', '#6633ff', '#ff33cc', '#33ccff',
    '#ff9933', '#99ff33', '#33ffcc', '#3366ff',
    '#cc33ff', '#ff3399', '#66ff33', '#00ffcc'
  ],

  hexToRgba(hex, alpha = 1) {
    if (!hex || typeof hex !== 'string') return `rgba(255,255,255,${alpha})`;
    const clean = hex.replace('#', '').trim();
    if (clean.length !== 6) return `rgba(255,255,255,${alpha})`;
    const r = parseInt(clean.slice(0, 2), 16);
    const g = parseInt(clean.slice(2, 4), 16);
    const b = parseInt(clean.slice(4, 6), 16);
    return `rgba(${r},${g},${b},${alpha})`;
  },

  drawPanelBackground(ctx, w, h, options = {}) {
    const top = options.top || 'rgba(18,20,28,0.92)';
    const bottom = options.bottom || 'rgba(6,8,14,0.96)';
    const line = options.line || 'rgba(255,255,255,0.06)';

    const bg = ctx.createLinearGradient(0, 0, 0, h);
    bg.addColorStop(0, top);
    bg.addColorStop(1, bottom);
    ctx.fillStyle = bg;
    ctx.fillRect(0, 0, w, h);

    ctx.strokeStyle = line;
    ctx.lineWidth = 1;
    for (let x = 0; x < w; x += 24) {
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, h);
      ctx.stroke();
    }
    for (let y = 0; y < h; y += 16) {
      ctx.beginPath();
      ctx.moveTo(0, y + 0.5);
      ctx.lineTo(w, y + 0.5);
      ctx.stroke();
    }

    ctx.strokeStyle = 'rgba(255,255,255,0.12)';
    ctx.strokeRect(0.5, 0.5, w - 1, h - 1);
  },
  
  // Draw a static waveform from peaks data array [[max,min], ...]
  drawStatic(canvas, peaks, options = {}) {
    if (!canvas || !peaks || peaks.length === 0) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    const color = options.color || '#00ff88';
    const bgColor = options.bgColor || 'rgba(0,0,0,0.85)';
    const startPoint = options.startPoint || 0;  // 0-1 normalized
    const endPoint = options.endPoint || 1;       // 0-1 normalized
    const accentColor = options.accentColor || '#ff3366';
    
    this.drawPanelBackground(ctx, w, h, {
      top: 'rgba(20,22,32,0.92)',
      bottom: bgColor,
      line: 'rgba(255,255,255,0.045)'
    });
    
    // Draw center line
    ctx.strokeStyle = 'rgba(255,255,255,0.16)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(w, h / 2);
    ctx.stroke();
    
    // Draw waveform
    const midY = h / 2;
    const scale = midY * 0.9;  // 90% of half height
    const step = w / peaks.length;
    
    // Fill waveform shape
    ctx.beginPath();
    for (let i = 0; i < peaks.length; i++) {
      const x = i * step;
      const maxVal = peaks[i][0] / 127;  // Normalize from int8 range
      const y = midY - maxVal * scale;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    // Go back drawing min values
    for (let i = peaks.length - 1; i >= 0; i--) {
      const x = i * step;
      const minVal = peaks[i][1] / 127;
      const y = midY - minVal * scale;
      ctx.lineTo(x, y);
    }
    ctx.closePath();
    
    // Gradient fill
    const grad = ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, this.hexToRgba(color, 0.84));
    grad.addColorStop(0.5, this.hexToRgba(color, 0.38));
    grad.addColorStop(1, this.hexToRgba(color, 0.84));
    ctx.fillStyle = grad;
    ctx.fill();
    
    // Outline
    ctx.shadowColor = this.hexToRgba(color, 0.45);
    ctx.shadowBlur = 8;
    ctx.strokeStyle = this.hexToRgba(color, 0.96);
    ctx.lineWidth = 1.25;
    ctx.beginPath();
    for (let i = 0; i < peaks.length; i++) {
      const x = i * step;
      const maxVal = peaks[i][0] / 127;
      const y = midY - maxVal * scale;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.beginPath();
    for (let i = 0; i < peaks.length; i++) {
      const x = i * step;
      const minVal = peaks[i][1] / 127;
      const y = midY - minVal * scale;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.shadowBlur = 0;
    
    // Draw start/end markers if set
    if (startPoint > 0) {
      const sx = startPoint * w;
      ctx.strokeStyle = accentColor;
      ctx.lineWidth = 1.5;
      ctx.setLineDash([4, 2]);
      ctx.beginPath();
      ctx.moveTo(sx, 0);
      ctx.lineTo(sx, h);
      ctx.stroke();
      ctx.setLineDash([]);
      
      // Dim area before start
      ctx.fillStyle = 'rgba(0,0,0,0.5)';
      ctx.fillRect(0, 0, sx, h);
      
      // Label
      ctx.fillStyle = accentColor;
      ctx.font = '10px monospace';
      ctx.fillText('S', sx + 3, 12);
    }
    if (endPoint < 1) {
      const ex = endPoint * w;
      ctx.strokeStyle = '#ff6633';
      ctx.lineWidth = 1.5;
      ctx.setLineDash([4, 2]);
      ctx.beginPath();
      ctx.moveTo(ex, 0);
      ctx.lineTo(ex, h);
      ctx.stroke();
      ctx.setLineDash([]);
      
      // Dim area after end
      ctx.fillStyle = 'rgba(0,0,0,0.5)';
      ctx.fillRect(ex, 0, w - ex, h);
      
      // Label
      ctx.fillStyle = '#ff6633';
      ctx.font = '10px monospace';
      ctx.fillText('E', ex + 3, 12);
    }
  },
  
  // Draw a mini oscilloscope from live level data (for mixer mini-scopes)
  drawMiniScope(canvas, history, options = {}) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    const color = options.color || '#00ff88';
    
    this.drawPanelBackground(ctx, w, h, {
      top: 'rgba(12,14,22,0.92)',
      bottom: 'rgba(6,8,12,0.96)',
      line: 'rgba(255,255,255,0.04)'
    });
    
    if (!history || history.length < 2) return;
    
    const midY = h / 2;
    const step = w / (history.length - 1);

    ctx.strokeStyle = 'rgba(255,255,255,0.15)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, midY + 0.5);
    ctx.lineTo(w, midY + 0.5);
    ctx.stroke();
    
    const pathPoints = [];
    for (let i = 0; i < history.length; i++) {
      const x = i * step;
      const val = history[i] / 255;
      const phase = (i / history.length) * Math.PI * 4 + Date.now() * 0.005;
      const y = midY - val * midY * 0.85 * Math.sin(phase);
      pathPoints.push({ x, y });
    }

    const area = ctx.createLinearGradient(0, 0, 0, h);
    area.addColorStop(0, this.hexToRgba(color, 0.24));
    area.addColorStop(1, this.hexToRgba(color, 0.02));
    ctx.fillStyle = area;
    ctx.beginPath();
    ctx.moveTo(pathPoints[0].x, midY);
    pathPoints.forEach(p => ctx.lineTo(p.x, p.y));
    ctx.lineTo(pathPoints[pathPoints.length - 1].x, midY);
    ctx.closePath();
    ctx.fill();

    ctx.shadowColor = this.hexToRgba(color, 0.55);
    ctx.shadowBlur = 6;
    ctx.strokeStyle = this.hexToRgba(color, 0.96);
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    pathPoints.forEach((p, idx) => {
      if (idx === 0) ctx.moveTo(p.x, p.y);
      else ctx.lineTo(p.x, p.y);
    });
    ctx.stroke();
    ctx.shadowBlur = 0;
  },
  
  // Draw VU meter bar (vertical)
  drawVUMeter(canvas, level, peakHold, options = {}) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    const color = options.color || '#00ff88';
    
    this.drawPanelBackground(ctx, w, h, {
      top: 'rgba(10,12,16,0.98)',
      bottom: 'rgba(3,4,8,1)',
      line: 'rgba(255,255,255,0.03)'
    });
    
    const normalizedLevel = Math.min(level / 255, 1.0);
    const barHeight = normalizedLevel * h;
    
    // Gradient: green -> yellow -> red
    const grad = ctx.createLinearGradient(0, h, 0, 0);
    grad.addColorStop(0, '#00ff88');
    grad.addColorStop(0.62, '#ffcc00');
    grad.addColorStop(0.84, '#ff6633');
    grad.addColorStop(1.0, '#ff3366');
    
    ctx.fillStyle = grad;
    ctx.shadowColor = this.hexToRgba(color, 0.5);
    ctx.shadowBlur = 6;
    ctx.fillRect(1, h - barHeight, w - 2, barHeight);
    ctx.shadowBlur = 0;
    
    // Peak hold indicator
    if (peakHold > 0) {
      const peakY = h - (Math.min(peakHold / 255, 1.0) * h);
      ctx.fillStyle = 'rgba(255,255,255,0.9)';
      ctx.fillRect(0, peakY, w, 1.5);
    }
    
    // Segment lines
    ctx.strokeStyle = 'rgba(0,0,0,0.3)';
    ctx.lineWidth = 1;
    const segments = 10;
    for (let i = 1; i < segments; i++) {
      const y = (i / segments) * h;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
    }
  },
  
  // Draw dual waveform (input vs output) for FX comparison
  drawDualScope(canvas, inputHistory, outputHistory, options = {}) {
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    const inputColor = options.inputColor || '#00ff88';
    const outputColor = options.outputColor || '#ff3366';
    
    this.drawPanelBackground(ctx, w, h, {
      top: 'rgba(16,18,26,0.95)',
      bottom: 'rgba(7,9,14,0.98)',
      line: 'rgba(255,255,255,0.04)'
    });
    
    // Labels
    ctx.font = '9px monospace';
    ctx.fillStyle = inputColor;
    ctx.fillText('IN', 4, 11);
    ctx.fillStyle = outputColor;
    ctx.fillText('OUT', w - 22, 11);
    
    // Center line
    ctx.strokeStyle = 'rgba(255,255,255,0.12)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(w, h / 2);
    ctx.stroke();
    
    const midY = h / 2;
    
    // Draw input (dry signal)
    if (inputHistory && inputHistory.length > 1) {
      const step = w / (inputHistory.length - 1);
      ctx.strokeStyle = this.hexToRgba(inputColor, 0.55);
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let i = 0; i < inputHistory.length; i++) {
        const x = i * step;
        const val = inputHistory[i] / 255;
        const phase = (i / inputHistory.length) * Math.PI * 6 + Date.now() * 0.004;
        const y = midY - val * midY * 0.8 * Math.sin(phase);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }
    
    // Draw output (wet/processed signal)
    if (outputHistory && outputHistory.length > 1) {
      const step = w / (outputHistory.length - 1);
      const points = [];
      for (let i = 0; i < outputHistory.length; i++) {
        const x = i * step;
        const val = outputHistory[i] / 255;
        const phase = (i / outputHistory.length) * Math.PI * 6 + Date.now() * 0.004;
        const y = midY - val * midY * 0.8 * Math.sin(phase * 0.9 + 0.3);
        points.push({ x, y });
      }

      const outFill = ctx.createLinearGradient(0, 0, 0, h);
      outFill.addColorStop(0, this.hexToRgba(outputColor, 0.22));
      outFill.addColorStop(1, this.hexToRgba(outputColor, 0.03));
      ctx.fillStyle = outFill;
      ctx.beginPath();
      ctx.moveTo(points[0].x, midY);
      points.forEach(p => ctx.lineTo(p.x, p.y));
      ctx.lineTo(points[points.length - 1].x, midY);
      ctx.closePath();
      ctx.fill();

      ctx.shadowColor = this.hexToRgba(outputColor, 0.55);
      ctx.shadowBlur = 5;
      ctx.strokeStyle = this.hexToRgba(outputColor, 0.98);
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      points.forEach((p, idx) => {
        if (idx === 0) ctx.moveTo(p.x, p.y);
        else ctx.lineTo(p.x, p.y);
      });
      ctx.stroke();
      ctx.shadowBlur = 0;
    }
  }
};


// ============================================
// AUDIO LEVELS STATE
// ============================================

const AudioLevels = {
  trackPeaks: new Uint8Array(16),
  masterPeak: 0,
  trackHistory: [],    // Array of 16 arrays, each with 30 samples of history
  peakHold: new Uint8Array(16),
  peakHoldDecay: new Float32Array(16),
  masterHistory: [],
  initialized: false,
  animFrameId: null,
  isActive: false,     // Only animate when a relevant tab is visible
  
  init() {
    if (this.initialized) return;
    for (let i = 0; i < 16; i++) {
      this.trackHistory.push([]);
      this.peakHold[i] = 0;
      this.peakHoldDecay[i] = 0;
    }
    this.masterHistory = [];
    this.initialized = true;
  },
  
  // Process incoming binary audio level data from WebSocket
  processBinaryMessage(data) {
    if (!this.initialized) this.init();
    
    const view = new Uint8Array(data);
    if (view.length < 18 || view[0] !== 0xAA) return false;
    
    for (let i = 0; i < 16; i++) {
      this.trackPeaks[i] = view[i + 1];
      
      // Update peak hold
      if (view[i + 1] > this.peakHold[i]) {
        this.peakHold[i] = view[i + 1];
        this.peakHoldDecay[i] = 0;
      } else {
        this.peakHoldDecay[i] += 0.02;
        if (this.peakHoldDecay[i] > 1) {
          this.peakHold[i] = Math.max(0, this.peakHold[i] - 2);
        }
      }
      
      // History for mini-scopes (keep last 30 samples = 1.5 sec at 20fps)
      this.trackHistory[i].push(view[i + 1]);
      if (this.trackHistory[i].length > 30) this.trackHistory[i].shift();
    }
    
    this.masterPeak = view[17];
    this.masterHistory.push(view[17]);
    if (this.masterHistory.length > 30) this.masterHistory.shift();
    
    return true;
  },
  
  startAnimation() {
    if (this.animFrameId) return;
    this.isActive = true;
    const animate = () => {
      if (!this.isActive) { this.animFrameId = null; return; }
      this.renderAll();
      this.animFrameId = requestAnimationFrame(animate);
    };
    this.animFrameId = requestAnimationFrame(animate);
  },
  
  stopAnimation() {
    this.isActive = false;
    if (this.animFrameId) {
      cancelAnimationFrame(this.animFrameId);
      this.animFrameId = null;
    }
  },
  
  renderAll() {
    // Render mixer mini-scopes if visible
    this.renderMixerScopes();
    // Render FX dual scope if visible
    this.renderFxScope();
  },
  
  // ============================================
  // MIXER VIEW - Mini Scopes + VU Meters
  // ============================================
  
  renderMixerScopes() {
    for (let i = 0; i < 16; i++) {
      // Mini oscilloscope
      const scopeCanvas = document.getElementById(`miniScope${i}`);
      if (scopeCanvas && scopeCanvas.offsetParent !== null) {
        WaveformRenderer.drawMiniScope(scopeCanvas, this.trackHistory[i], {
          color: WaveformRenderer.trackColors[i]
        });
      }
      
      // VU meter
      const vuCanvas = document.getElementById(`vuMeter${i}`);
      if (vuCanvas && vuCanvas.offsetParent !== null) {
        WaveformRenderer.drawVUMeter(vuCanvas, this.trackPeaks[i], this.peakHold[i], {
          color: WaveformRenderer.trackColors[i]
        });
      }
    }
    
    // Master VU
    const masterVu = document.getElementById('vuMeterMaster');
    if (masterVu && masterVu.offsetParent !== null) {
      WaveformRenderer.drawVUMeter(masterVu, this.masterPeak, 0, {
        color: '#00ff88'
      });
    }
  },
  
  // ============================================
  // FX EDIT VIEW - Dual Input/Output Scope
  // ============================================
  
  renderFxScope() {
    const fxScopeCanvas = document.getElementById('fxDualScope');
    if (!fxScopeCanvas || fxScopeCanvas.offsetParent === null) return;
    
    const selectedTrack = window._fxSelectedTrack || 0;
    const inputHist = this.trackHistory[selectedTrack] || [];
    
    // For output, we simulate the processed signal by attenuating based on FX
    // (Real difference would need backend support - this is a visual approximation)
    const outputHist = inputHist.map(v => {
      // Apply visual filter effect based on current FX state
      return Math.max(0, Math.min(255, v * 0.85 + Math.random() * 10));
    });
    
    WaveformRenderer.drawDualScope(fxScopeCanvas, inputHist, outputHist, {
      inputColor: WaveformRenderer.trackColors[selectedTrack],
      outputColor: '#ff3366'
    });
  }
};


// ============================================
// SAMPLE BROWSER WAVEFORM
// ============================================

const SampleWaveform = {
  cache: {},  // Cache of fetched waveform data per pad
  
  // Fetch waveform data for a pad from the backend
  async fetchWaveform(pad, points = 200) {
    const cacheKey = `pad_${pad}`;
    if (this.cache[cacheKey]) return this.cache[cacheKey];
    
    try {
      const response = await fetch(`/api/waveform?pad=${pad}&points=${points}`);
      if (!response.ok) return null;
      const data = await response.json();
      this.cache[cacheKey] = data;
      return data;
    } catch (e) {
      console.error('[Waveform] Fetch error:', e);
      return null;
    }
  },
  
  // Clear cache for a pad (call when sample changes)
  clearCache(pad) {
    delete this.cache[`pad_${pad}`];
  },
  
  clearAllCache() {
    this.cache = {};
  },
  
  // Create and render waveform canvas for sample browser/selector
  createWaveformDisplay(container, pad, options = {}) {
    const canvas = document.createElement('canvas');
    canvas.className = 'waveform-canvas';
    canvas.width = options.width || 300;
    canvas.height = options.height || 80;
    canvas.dataset.pad = pad;
    container.appendChild(canvas);
    
    // Show loading state
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = 'rgba(0,0,0,0.85)';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = '#666';
    ctx.font = '11px monospace';
    ctx.textAlign = 'center';
    ctx.fillText('Cargando...', canvas.width / 2, canvas.height / 2);
    
    // Fetch and render
    this.fetchWaveform(pad).then(data => {
      if (data && data.peaks) {
        const trackColor = (pad < 16) ? WaveformRenderer.trackColors[pad] : '#00ff88';
        WaveformRenderer.drawStatic(canvas, data.peaks, {
          color: options.color || trackColor,
          startPoint: options.startPoint || 0,
          endPoint: options.endPoint || 1
        });
        
        // Add duration label
        if (data.duration) {
          const dctx = canvas.getContext('2d');
          dctx.fillStyle = 'rgba(255,255,255,0.6)';
          dctx.font = '9px monospace';
          dctx.textAlign = 'right';
          const durSec = (data.duration / 1000).toFixed(2);
          dctx.fillText(`${durSec}s`, canvas.width - 4, canvas.height - 4);
          dctx.textAlign = 'left';
          dctx.fillText(data.name || '', 4, canvas.height - 4);
        }
      } else {
        ctx.fillStyle = 'rgba(0,0,0,0.85)';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.fillStyle = '#666';
        ctx.font = '11px monospace';
        ctx.textAlign = 'center';
        ctx.fillText('Sin datos', canvas.width / 2, canvas.height / 2);
      }
    });
    
    return canvas;
  }
};


// ============================================
// INTEGRATION: Hook into existing WebSocket and UI
// ============================================

// Hook into WebSocket binary message handler
function handleWaveformBinaryMessage(event) {
  if (event.data instanceof ArrayBuffer) {
    return AudioLevels.processBinaryMessage(event.data);
  }
  return false;
}

// Inject mixer mini-scopes into the volumes tab
function injectMixerScopes() {
  const trackNames = ['BD', 'SD', 'CH', 'OH', 'CP', 'RS', 'CL', 'CY',
                       'HT', 'MT', 'LT', 'HC', 'LC', 'MC', 'CB', 'MA'];
  
  for (let i = 0; i < 16; i++) {
    const card = document.querySelector(`.track-volume-card[data-track="${i}"]`);
    if (!card) continue;
    
    // Check if already injected
    if (card.querySelector('.mini-scope-container')) continue;
    
    // Create mini scope + VU container
    const scopeContainer = document.createElement('div');
    scopeContainer.className = 'mini-scope-container';
    scopeContainer.innerHTML = `
      <canvas id="miniScope${i}" class="mini-scope-canvas" width="60" height="24"></canvas>
      <canvas id="vuMeter${i}" class="vu-meter-canvas" width="8" height="60"></canvas>
    `;
    
    // Insert after the volume bar container
    const barContainer = card.querySelector('.track-volume-bar-container');
    if (barContainer) {
      barContainer.parentNode.insertBefore(scopeContainer, barContainer.nextSibling);
    }
  }
  
  // Master VU meter
  const masterCards = document.querySelectorAll('.volume-card');
  masterCards.forEach(card => {
    if (card.querySelector('.vu-meter-canvas')) return;
    const vuCanvas = document.createElement('canvas');
    vuCanvas.id = 'vuMeterMaster';
    vuCanvas.className = 'vu-meter-canvas master-vu';
    vuCanvas.width = 12;
    vuCanvas.height = 40;
    const display = card.querySelector('.volume-display');
    if (display) display.appendChild(vuCanvas);
  });
}

// Inject FX scope into the FX edit section
function injectFxScope() {
  const fxTab = document.getElementById('fxtab-masterFx');
  if (!fxTab || document.getElementById('fxDualScope')) return;
  
  const scopeSection = document.createElement('div');
  scopeSection.className = 'fx-scope-section';
  scopeSection.innerHTML = `
    <div class="fx-scope-header">
      <span class="fx-scope-title">🔊 AUDIO SCOPE</span>
      <span class="fx-scope-label" id="fxScopeTrackLabel">T01: BD</span>
    </div>
    <canvas id="fxDualScope" class="fx-dual-scope-canvas" width="320" height="80"></canvas>
    <div class="fx-scope-legend">
      <span class="fx-legend-in">● IN (dry)</span>
      <span class="fx-legend-out">● OUT (wet)</span>
    </div>
  `;
  
  // Insert at the top of the FX tab
  fxTab.insertBefore(scopeSection, fxTab.firstChild);
}

// Enhance sample selector modal with waveform
function enhanceSampleModal(modal, padIndex) {
  if (!modal) return;
  
  // Add waveform preview area to the modal header
  const content = modal.querySelector('.sample-modal-content');
  if (!content) return;
  
  const waveformArea = document.createElement('div');
  waveformArea.className = 'sample-modal-waveform';
  waveformArea.innerHTML = `
    <div class="waveform-preview-label">📊 FORMA DE ONDA</div>
    <canvas id="samplePreviewWaveform" class="sample-preview-canvas" width="320" height="80"></canvas>
    <div class="waveform-preview-info" id="samplePreviewInfo">
      Selecciona un sample para ver su forma de onda
    </div>
  `;
  
  // Insert after the h3 title
  const title = content.querySelector('h3');
  if (title) {
    title.after(waveformArea);
  }
}

// Hook for when a sample is selected in the browser (preview waveform)
function previewSampleWaveform(padIndex) {
  const canvas = document.getElementById('samplePreviewWaveform');
  if (!canvas) return;
  
  SampleWaveform.fetchWaveform(padIndex).then(data => {
    if (data && data.peaks) {
      const color = (padIndex < 16) ? WaveformRenderer.trackColors[padIndex] : '#00ff88';
      WaveformRenderer.drawStatic(canvas, data.peaks, { color });
      
      // Update info
      const info = document.getElementById('samplePreviewInfo');
      if (info && data.duration) {
        const durSec = (data.duration / 1000).toFixed(2);
        info.textContent = `${data.name} · ${durSec}s · ${data.samples} samples`;
      }
    }
  });
}

// Enhance the sample browser (INFO tab) with waveform previews
function enhanceSampleBrowser() {
  // Watch for sample browser list changes
  const browserList = document.getElementById('sampleBrowserList');
  if (!browserList) return;
  
  const observer = new MutationObserver(() => {
    // Add waveform preview to loaded sample items
    const items = browserList.querySelectorAll('.sample-browser-item');
    items.forEach(item => {
      if (item.querySelector('.waveform-canvas')) return;
      const padAttr = item.dataset.pad;
      if (padAttr !== undefined) {
        const padIndex = parseInt(padAttr);
        if (!isNaN(padIndex)) {
          const wfContainer = document.createElement('div');
          wfContainer.className = 'browser-waveform-container';
          SampleWaveform.createWaveformDisplay(wfContainer, padIndex, {
            width: 160, height: 40
          });
          item.appendChild(wfContainer);
        }
      }
    });
  });
  
  observer.observe(browserList, { childList: true, subtree: true });
}


// ============================================
// TAB VISIBILITY MANAGEMENT
// ============================================

function onTabChanged(tabName) {
  if (tabName === 'volumes') {
    injectMixerScopes();
    AudioLevels.init();
    AudioLevels.startAnimation();
  } else if (tabName === 'fx') {
    injectFxScope();
    AudioLevels.init();
    AudioLevels.startAnimation();
  } else {
    // Stop animation when not on relevant tabs to save CPU
    AudioLevels.stopAnimation();
  }
}

// Listen for tab changes
function hookTabNavigation() {
  const tabBtns = document.querySelectorAll('.tab-btn');
  tabBtns.forEach(btn => {
    btn.addEventListener('click', () => {
      const tab = btn.dataset.tab;
      onTabChanged(tab);
    });
  });
}

// Track FX selector change - update scope label
function hookFxTrackSelector() {
  const trackNames = ['BD', 'SD', 'CH', 'OH', 'CP', 'RS', 'CL', 'CY',
                       'HT', 'MT', 'LT', 'HC', 'LC', 'MC', 'CB', 'MA'];
  
  // Watch for track FX selector clicks
  document.addEventListener('click', (e) => {
    const btn = e.target.closest('.track-fx-btn');
    if (btn) {
      const track = parseInt(btn.dataset.track);
      window._fxSelectedTrack = track;
      const label = document.getElementById('fxScopeTrackLabel');
      if (label) {
        const name = trackNames[track] || `T${track + 1}`;
        label.textContent = `T${String(track + 1).padStart(2, '0')}: ${name}`;
        label.style.color = WaveformRenderer.trackColors[track];
      }
    }
  });
}


// ============================================
// INITIALIZATION
// ============================================

function initWaveformVisualizer() {
  console.log('[Waveform] Initializing visualizer...');
  
  AudioLevels.init();
  hookTabNavigation();
  hookFxTrackSelector();
  enhanceSampleBrowser();
  
  // Initial FX track
  window._fxSelectedTrack = 0;
  
  console.log('[Waveform] ✓ Visualizer ready (3 modes: browser/fx/mixer)');
}

// Auto-init when DOM is ready
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initWaveformVisualizer);
} else {
  initWaveformVisualizer();
}
