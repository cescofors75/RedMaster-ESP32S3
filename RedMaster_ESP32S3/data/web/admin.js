/* RED808 Admin Dashboard JS v2 */
'use strict';

// ── Config ──────────────────────────────────────────────────────────────
const REFRESH_INTERVAL = 3000;
const MAX_HIST = 80;
const PAD_NAMES = ['BD','SD','CH','OH','CY','CP','RS','CB','LT','MT','HT','MA','CL','HC','MC','LC'];

// ── State ────────────────────────────────────────────────────────────────
let ws = null;
let wsConnected = false;
let autoRefreshTimer = null;
let autoRefreshEnabled = true;
let logPaused = false;
let spiLogPaused = false;
let spiPktCount = 0;
const chartHistory = { heap: [], psram: [], labels: [] };
let chartCtx = null;
let chartAnimId = null;

// ── Init ─────────────────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  connectWS();
  buildPeaksGrid();
  initChart();
  startAutoRefresh();
  fetchSysinfo();
  // Pausar el polling cuando la pestaña esta oculta: una pestaña de admin
  // olvidada en background cargaba el ESP32 con /api/sysinfo cada 3s.
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      stopAutoRefresh();
    } else if (autoRefreshEnabled) {
      startAutoRefresh();
      fetchSysinfo();  // refresco inmediato al volver
    }
  });
  document.getElementById('logPauseChk').addEventListener('change', e => {
    logPaused = e.target.checked;
  });
  const spiPauseChk = document.getElementById('spiLogPauseChk');
  if (spiPauseChk) spiPauseChk.addEventListener('change', e => { spiLogPaused = e.target.checked; });
});

// ── WebSocket ─────────────────────────────────────────────────────────────
function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    wsConnected = true;
    setBadge('admWsBadge', '● WS ONLINE', 'badge-ok');
    admLog('WebSocket conectado', 'info');
  };
  ws.onclose = () => {
    wsConnected = false;
    setBadge('admWsBadge', '● WS OFFLINE', 'badge-err');
    admLog('WebSocket desconectado – reintentando en 4s…', 'warn');
    setTimeout(connectWS, 4000);
  };
  ws.onerror = () => admLog('WS error', 'err');
  ws.onmessage = e => {
    if (e.data instanceof ArrayBuffer) {
      handleBinaryFrame(new Uint8Array(e.data));
      return;
    }
    try { handleWsMsg(JSON.parse(e.data)); } catch (_) { }
  };
}

function handleBinaryFrame(v) {
  if (!v || v.length < 2) return;
  // 0xAA + 16 tracks + master
  if (v[0] === 0xAA && v.length >= 18) {
    const peaks = [];
    for (let i = 0; i < 16; i++) peaks.push((v[i + 1] || 0) / 255);
    const master = (v[17] || 0) / 255;
    updatePeaks(peaks, master);
  }
}

function handleWsMsg(d) {
  // Estado del secuenciador — viene en 'state' (broadcast periódico) o 'playState' (respuesta al play/stop)
  if (d.type === 'state' || d.type === 'playState') updateSeqFromState(d);
  // El mensaje 'state' también contiene datos de sysinfo (heap, samplesLoaded, psramFree)
  if (d.type === 'state') updateDashboardFromState(d);
  if (d.type === 'sysinfo') updateDashboard(d);
  if (d.type === 'peaks')   updatePeaks(d.peaks, d.master);
  if (d.type === 'log')     admLog(d.msg, d.level || 'info');
  if (d.type === 'spi_log') spiLogAppend(d);
  if (d.type === 'daisy')   updateDaisyStatus(d);
  if (d.wsClients != null)  updateWsClients(d.wsClients);
  if (d.udpClients != null) updateUdpClients(d.udpClients);
}

// Extrae los campos de sysinfo que llegan dentro del mensaje 'state'
function updateDashboardFromState(d) {
  // El mensaje state incluye: heap, psramFree, samplesLoaded, tempo, playing, pattern, step
  if (d.heap != null)         setEl('sv-heap',    fmtBytes(d.heap));
  if (d.psramFree != null)    setEl('sv-psram',   fmtBytes(d.psramFree));
  if (d.samplesLoaded != null)setEl('sv-samples', d.samplesLoaded);
  if (d.tempo != null)        setEl('sv-bpm',     d.tempo);

  // Barras de memoria con los valores disponibles en el mensaje state
  const heapTotal  = 460 * 1024;  // ESP32-S3 heap total típico
  const psramTotal = 5.5 * 1024 * 1024;
  if (d.heap != null) {
    const used = heapTotal - d.heap;
    setBarPct('mb-heap', used / heapTotal * 100);
    setEl('mn-heap-used', fmtBytes(used));
    setEl('mn-heap-total', fmtBytes(heapTotal));
  }
  if (d.psramFree != null) {
    const used = psramTotal - d.psramFree;
    setBarPct('mb-psram', used / psramTotal * 100);
    setEl('mn-psram-used', fmtBytes(used));
    setEl('mn-psram-total', fmtBytes(psramTotal));
  }
  if (d.samplesLoaded != null) {
    setBarPct('mb-samples', d.samplesLoaded / 32 * 100);
    setEl('mn-samples-used', d.samplesLoaded + ' / 32 samples');
  }
  // Historial del chart
  if (d.heap != null || d.psramFree != null) {
    const ts = new Date().toLocaleTimeString('es',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
    chartHistory.heap.push(d.heap || 0);
    chartHistory.psram.push(d.psramFree || 0);
    chartHistory.labels.push(ts);
    if (chartHistory.heap.length   > MAX_HIST) chartHistory.heap.shift();
    if (chartHistory.psram.length  > MAX_HIST) chartHistory.psram.shift();
    if (chartHistory.labels.length > MAX_HIST) chartHistory.labels.shift();
    drawChart();
  }
}

// ── REST polling ──────────────────────────────────────────────────────────
async function fetchSysinfo() {
  try {
    const r = await fetch('/api/sysinfo');
    if (!r.ok) throw new Error(r.status);
    const d = await r.json();
    updateDashboard(d);
    setBadge('admSyncBadge', '↺ SYNC OK', 'badge-ok');
  } catch (err) {
    admLog('Error /api/sysinfo: ' + err, 'warn');
    setBadge('admSyncBadge', '↺ SYNC ERR', 'badge-warn');
  }
}

function startAutoRefresh() {
  stopAutoRefresh();
  if (!autoRefreshEnabled) return;
  autoRefreshTimer = setInterval(fetchSysinfo, REFRESH_INTERVAL);
}
function stopAutoRefresh() {
  if (autoRefreshTimer) { clearInterval(autoRefreshTimer); autoRefreshTimer = null; }
}

// ── Dashboard update ──────────────────────────────────────────────────────
function updateDashboard(d) {
  // Stat bar
  setEl('sv-heap',    d.heapFree    != null ? fmtBytes(d.heapFree)    : '--');
  setEl('sv-psram',   d.psramFree   != null ? fmtBytes(d.psramFree)   : '--');
  setEl('sv-samples', d.samplesLoaded != null ? d.samplesLoaded       : '--');
  setEl('sv-bpm',     d.tempo       != null ? d.tempo                 : '--');
  setEl('sv-uptime',  d.uptime      != null ? fmtUptime(d.uptime)     : '--');
  setEl('sv-clients', d.wsClients   != null ? d.wsClients             : '--');

  // System info table
  setEl('si-ip',   d.ip       || '--');
  setEl('si-ch',   d.channel  != null ? 'CH ' + d.channel   : '--');
  setEl('si-tx',   d.txPower  != null ? d.txPower           : '--');
  setEl('si-sta',  d.connectedStations != null ? d.connectedStations : (d.wifiMode === 'STA' ? 'STA' : '--'));
  setEl('si-fw',   'RED808 v2.0');
  setEl('si-flash',d.flashSize != null ? fmtBytes(d.flashSize) : '--');

  // Memory bars
  const heapTotal  = d.heapSize  || (460 * 1024);
  const psramTotal = d.psramSize || (5.5 * 1024 * 1024);
  const heapUsed   = heapTotal - (d.heapFree  || 0);
  const psramUsed  = psramTotal - (d.psramFree || 0);
  const sampLoaded = d.samplesLoaded || 0;
  const sampMax    = 32;  // MAX_TRACKS en el firmware

  setBarPct('mb-heap',    heapUsed  / heapTotal  * 100);
  setBarPct('mb-psram',   psramUsed / psramTotal * 100);
  setBarPct('mb-samples', sampLoaded / sampMax   * 100);

  setEl('mn-heap-used',   fmtBytes(heapUsed));
  setEl('mn-heap-total',  fmtBytes(heapTotal));
  setEl('mn-psram-used',  fmtBytes(psramUsed));
  setEl('mn-psram-total', fmtBytes(psramTotal));
  setEl('mn-samples-used', sampLoaded + ' / ' + sampMax + ' samples');

  // WS / UDP clients
  if (d.wsClientList)  updateWsClients(d.wsClientList);
  if (d.udpClientList) updateUdpClients(d.udpClientList);

  // Daisy telemetry from /api/sysinfo
  const daisyConnected = !!d.daisyConnected;
  setBadge('daisy-badge', daisyConnected ? 'CONECTADO' : 'SIN CONEXIÓN',
           daisyConnected ? 'badge-ok' : 'badge-dim');
  setEl('ds-conn', daisyConnected ? 'OK' : 'OFFLINE');
  setEl('ds-rtt', d.daisyRttMs != null && d.daisyRttMs >= 0 ? `${Number(d.daisyRttMs).toFixed(2)} ms` : '--');
  setEl('ds-samples', d.samplesLoaded != null ? d.samplesLoaded : '--');
  setEl('ds-voices', d.daisyVoices != null ? `${d.daisyVoices} voces` : '--');
  setEl('ds-pads',   d.daisyPadsLoaded != null ? d.daisyPadsLoaded + ' / 24' : '--');
  const errCnt = d.daisySpiErrCnt;
  const errEl  = document.getElementById('ds-errcnt');
  if (errEl && errCnt != null) {
    errEl.textContent = errCnt;
    errEl.style.color = errCnt > 0 ? '#f87171' : '';
  }
  setEl('ds-uptime', d.uptime != null ? fmtUptime(d.uptime) : '--');

  if (Array.isArray(d.daisyTrackPeaks)) {
    updatePeaks(d.daisyTrackPeaks, d.daisyMasterPeak);
  }

  // Sequencer (sysinfo también incluye estos campos)
  if (d.tempo   != null) { setEl('seq-bpm', d.tempo + ' BPM'); setEl('sv-bpm', d.tempo); }
  if (d.playing != null) updateSeqFromState(d);
  if (d.pattern != null) setEl('seq-pat', 'PAT ' + d.pattern);

  // Chart history
  const now = new Date().toLocaleTimeString('es',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
  chartHistory.heap.push(d.heapFree   || 0);
  chartHistory.psram.push(d.psramFree || 0);
  chartHistory.labels.push(now);
  if (chartHistory.heap.length   > MAX_HIST) chartHistory.heap.shift();
  if (chartHistory.psram.length  > MAX_HIST) chartHistory.psram.shift();
  if (chartHistory.labels.length > MAX_HIST) chartHistory.labels.shift();
  drawChart();
}

// ── Sequencer ─────────────────────────────────────────────────────────────
function updateSeqFromState(d) {
  // Normalizar campo playing (distintos tipos de mensaje usan distintos keys)
  const playing = d.playing === true || d.isPlaying === true;
  const dot = document.getElementById('seq-dot');
  const sta = document.getElementById('seq-status');
  if (dot) { dot.className = playing ? 'playing' : 'stopped'; }
  if (sta) { sta.textContent = playing ? '▶ PLAYING' : '■ STOPPED'; }
  // Actualizar BPM/patrón/step solo si el mensaje los incluye (playState no los tiene)
  const bpm = d.tempo || d.bpm;
  if (bpm != null) {
    setEl('seq-bpm', bpm + ' BPM');
    setEl('sv-bpm',  bpm);
  }
  if (d.pattern != null) setEl('seq-pat',  'PAT ' + d.pattern);
  if (d.step    != null) setEl('seq-step', 'STEP ' + d.step);
}

// ── Daisy ─────────────────────────────────────────────────────────────────
function updateDaisyStatus(d) {
  // Normalizar campo: el servidor envía 'daisyConnected', mensajes live usan 'connected'
  const connected = d.daisyConnected || d.connected || d.daisy_conn;
  setBadge('daisy-badge', connected ? 'CONECTADO' : 'SIN CONEXIÓN',
           connected ? 'badge-ok' : 'badge-dim');
  setEl('ds-conn', connected ? 'OK' : 'OFFLINE');
  setEl('ds-rtt',     d.daisyRttMs != null ? `${Number(d.daisyRttMs).toFixed(2)} ms` : (d.rtt != null ? d.rtt + ' ms' : '--'));
  setEl('ds-samples', d.samplesLoaded != null ? d.samplesLoaded : (d.samples != null ? d.samples : '--'));
  setEl('ds-voices',  d.daisyVoices  != null ? d.daisyVoices + ' voces' : (d.voices != null ? d.voices + ' voces' : '--'));
  setEl('ds-uptime',  d.uptime  != null ? fmtUptime(d.uptime): '--');
  const hint = document.getElementById('daisy-hint');
  if (hint) {
    const status = connected ? 'Conexión SPI activa' : 'Esperando Daisy Seed…';
    hint.textContent = `${status} — D7←GPIO7 (CS) · D8←GPIO4 (SCK) · D9→GPIO6 (MISO) · D10←GPIO5 (MOSI) · GND`;
  }
}

// ── Peaks grid ────────────────────────────────────────────────────────────
function buildPeaksGrid() {
  const grid = document.getElementById('peaksGrid');
  if (!grid) return;
  grid.innerHTML = '';
  PAD_NAMES.forEach((name, i) => {
    const t = document.createElement('div');
    t.className = 'peak-track';
    t.id = 'pt-' + i;
    t.innerHTML = `<div class="peak-bar-wrap"><div class="peak-bar" id="pb-${i}"></div></div><div class="peak-track-label">${name}</div>`;
    grid.appendChild(t);
  });
}

function updatePeaks(peaks, master) {
  if (!peaks) return;
  peaks.forEach((v, i) => {
    const bar = document.getElementById('pb-' + i);
    const trk = document.getElementById('pt-' + i);
    if (!bar) return;
    const pct = Math.min(100, Math.max(0, v * 100));
    bar.style.height = pct + '%';
    if (trk) trk.classList.toggle('hot', pct > 80);
  });
  if (master != null) {
    // peak-master-badge muestra el % en texto (no hay barra master separada)
    const lbl = document.getElementById('peak-master-badge');
    const pct = Math.min(100, Math.max(0, master * 100));
    if (lbl) lbl.textContent = 'MASTER ' + Math.round(pct) + '%';
  }
}

// ── WS / UDP Clients ──────────────────────────────────────────────────────
function updateWsClients(list) {
  const cnt  = document.getElementById('wc-count');
  const body = document.getElementById('wc-body');
  if (!body) return;
  const arr = Array.isArray(list) ? list : [];
  if (cnt) cnt.textContent = arr.length;
  if (arr.length === 0) { body.innerHTML = '<div class="empty-hint">Sin clientes WS</div>'; return; }
  body.innerHTML = '<div class="clients-list">' +
    arr.map(c => `<div class="client-item"><span class="client-dot"></span><span class="client-ip">${c.ip || c}</span><span class="client-info">${c.id || ''}</span></div>`).join('') +
    '</div>';
}

function updateUdpClients(list) {
  const cnt  = document.getElementById('uc-count');
  const body = document.getElementById('uc-body');
  if (!body) return;
  const arr = Array.isArray(list) ? list : [];
  if (cnt) cnt.textContent = arr.length;
  if (arr.length === 0) { body.innerHTML = '<div class="empty-hint">Sin clientes UDP</div>'; return; }
  body.innerHTML = '<div class="clients-list">' +
    arr.map(c => `<div class="client-item"><span class="client-dot" style="background:var(--adm-orange);box-shadow:0 0 5px var(--adm-orange)"></span><span class="client-ip">${c.ip || c}</span><span class="client-info">${c.port ? ':'+c.port : ''}</span></div>`).join('') +
    '</div>';
}

// ── Chart ─────────────────────────────────────────────────────────────────
function initChart() {
  const canvas = document.getElementById('admChart');
  if (!canvas) return;
  chartCtx = canvas.getContext('2d');
  new ResizeObserver(() => {
    canvas.width  = canvas.offsetWidth;
    canvas.height = canvas.offsetHeight || 160;
    drawChart();
  }).observe(canvas);
}

function drawChart() {
  if (!chartCtx) return;
  const canvas = chartCtx.canvas;
  const w = canvas.width, h = canvas.height;
  if (!w || !h) return;
  chartCtx.clearRect(0, 0, w, h);

  // grid lines
  chartCtx.strokeStyle = 'rgba(255,255,255,0.05)';
  chartCtx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = Math.round(h * i / 4) + 0.5;
    chartCtx.beginPath(); chartCtx.moveTo(0, y); chartCtx.lineTo(w, y); chartCtx.stroke();
  }

  const data = [
    { arr: chartHistory.heap,  color: '#00e5ff', fill: 'rgba(0,229,255,0.1)' },
    { arr: chartHistory.psram, color: '#bf5af2', fill: 'rgba(191,90,242,0.1)' }
  ];

  const maxVal = 6 * 1024 * 1024; // 6MB scale

  data.forEach(series => {
    if (series.arr.length < 2) return;
    const pts = series.arr;
    const step = w / (pts.length - 1);

    // fill
    chartCtx.beginPath();
    chartCtx.moveTo(0, h);
    pts.forEach((v, i) => {
      const x = i * step;
      const y = h - (v / maxVal) * h;
      i === 0 ? chartCtx.moveTo(x, y) : chartCtx.lineTo(x, y);
    });
    chartCtx.lineTo((pts.length - 1) * step, h);
    chartCtx.closePath();
    chartCtx.fillStyle = series.fill;
    chartCtx.fill();

    // line
    chartCtx.beginPath();
    pts.forEach((v, i) => {
      const x = i * step;
      const y = h - (v / maxVal) * h;
      i === 0 ? chartCtx.moveTo(x, y) : chartCtx.lineTo(x, y);
    });
    chartCtx.strokeStyle = series.color;
    chartCtx.lineWidth = 2;
    chartCtx.stroke();
  });
}

// ── Actions ───────────────────────────────────────────────────────────────
function admReboot() {
  if (!confirm('¿Reiniciar el ESP32?')) return;
  // El ESP32 no tiene comando reboot via WS; usar HTTP POST al endpoint de sequencer
  // como workaround de confirmación visual
  admLog('Reboot: no soportado via WebSocket en este firmware', 'warn');
  admLog('→ Pulsa el botón físico RST en la placa para reiniciar', 'dim');
}
function admClearSamples() {
  if (!confirm('¿Descargar kit de samples de la Daisy?')) return;
  admSendWS({ cmd: 'sdUnloadKit' });
  admLog('sdUnloadKit enviado', 'warn');
}
function admToggleAutoRefresh() {
  autoRefreshEnabled = !autoRefreshEnabled;
  const btn = document.getElementById('autRefBtn');
  if (autoRefreshEnabled) {
    startAutoRefresh();
    if (btn) { btn.textContent = '⏱ Auto: ON'; btn.className = 'act-btn warn'; }
    admLog('Auto-refresh activado', 'info');
  } else {
    stopAutoRefresh();
    if (btn) { btn.textContent = '⏱ Auto: OFF'; btn.className = 'act-btn accent'; }
    admLog('Auto-refresh pausado', 'dim');
  }
}
function admClearLog() {
  const el = document.getElementById('admLog');
  if (el) el.innerHTML = '';
}

// ── SPI Log ───────────────────────────────────────────────────────────────
function spiLogAppend(d) {
  if (spiLogPaused) return;
  const errOnly = document.getElementById('spiErrOnlyChk')?.checked;
  if (errOnly && d.ok !== false) return;
  const pre = document.getElementById('spiLog');
  if (!pre) return;

  spiPktCount++;
  const cnt = document.getElementById('spi-pkt-count');
  if (cnt) cnt.textContent = spiPktCount + ' pkts';

  // ok === null  → sendCommand (fire-and-forget, no response expected)
  // ok === true  → sendAndReceive OK
  // ok === false → sendAndReceive FAIL
  const okTag   = d.ok === true  ? ' ✓' : d.ok === false ? ' ✗ FAIL' : '';
  const tryTag  = d.try != null  ? ` try=${d.try}` : '';
  const lenTag  = d.len != null  ? ` len=${d.len}`  : '';
  const ts      = d.ms  != null  ? `+${d.ms}ms` : new Date().toLocaleTimeString('es', {hour12: false});
  const name    = d.name || ('0x' + (d.cmd || 0).toString(16).toUpperCase());

  const line = document.createElement('span');
  line.className = d.ok === false ? 'log-err' : d.ok === true ? 'log-info' : 'log-dim';
  line.textContent = `[${ts}] ${name} (CMD=0x${(d.cmd||0).toString(16).toUpperCase()}) #${d.seq ?? '?'}${lenTag}${tryTag}${okTag}\n`;
  pre.appendChild(line);
  while (pre.children.length > 500) pre.removeChild(pre.firstChild);
  pre.scrollTop = pre.scrollHeight;
}

function spiLogClear() {
  const el = document.getElementById('spiLog');
  if (el) el.innerHTML = '';
  spiPktCount = 0;
  const cnt = document.getElementById('spi-pkt-count');
  if (cnt) cnt.textContent = '0 pkts';
}
function admSendWS(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  } else {
    admLog('WS no disponible', 'err');
  }
}

// ── Log ───────────────────────────────────────────────────────────────────
function admLog(msg, type) {
  if (logPaused) return;
  const el = document.getElementById('admLog');
  if (!el) return;
  const ts = new Date().toLocaleTimeString('es', { hour12: false });
  const cls = type === 'info' ? 'log-info' : type === 'warn' ? 'log-warn' : type === 'err' ? 'log-err' : type === 'dim' ? 'log-dim' : '';
  const line = document.createElement('span');
  if (cls) line.className = cls;
  line.textContent = `[${ts}] ${msg}\n`;
  el.appendChild(line);
  // keep max 300 lines
  while (el.children.length > 300) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}

// ── Helpers ───────────────────────────────────────────────────────────────
function setEl(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}
function setBarPct(id, pct) {
  const el = document.getElementById(id);
  if (el) el.style.width = Math.min(100, Math.max(0, pct)).toFixed(1) + '%';
}
function setBadge(id, text, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  el.className = 'adm-badge ' + cls;
}
function admFetchRefreshAll() { fetchSysinfo(); admLog('Refresh manual', 'dim'); }

function fmtBytes(b) {
  if (b == null) return '--';
  if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB';
  if (b >= 1024)    return (b / 1024).toFixed(0) + ' KB';
  return b + ' B';
}
function fmtUptime(ms) {
  if (ms == null) return '--';
  const s  = Math.floor(ms / 1000);
  const d  = Math.floor(s / 86400);
  const h  = Math.floor((s % 86400) / 3600);
  const m  = Math.floor((s % 3600) / 60);
  const ss = s % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m ${ss}s`;
  return `${m}m ${ss}s`;
}
