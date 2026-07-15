/* ============================================================
   RED808 PATCHBAY — Signal Routing Engine v1.0
   Visual modular cable patching for the RED808 drum machine
   ============================================================ */
(function(){
'use strict';

/* ──────────── CONFIG ──────────── */
const TRACK_NAMES = [
  'BD','SD','CH','OH','CP','CB','CY','HC',
  'HT','MT','LT','LC','MC','RS','MA','CL'
];
const TRACK_LABELS = [
  'KICK','SNARE','CL.HAT','OP.HAT','CLAP','COWBELL','CYMBAL','HI CONGA',
  'HI TOM','MID TOM','LO TOM','LO CONGA','MID CONGA','RIMSHOT','MARACAS','CLAVES'
];
const PAD_COLORS = [
  '#ff2d2d','#ff6b35','#ffa726','#ffd600',
  '#c6ff00','#69f0ae','#26c6da','#448aff',
  '#7c4dff','#ea80fc','#ff4081','#f50057',
  '#ff1744','#d500f9','#651fff','#00e676'
];
const INITIAL_SOURCE_COUNT = 0;

const FX_DEFS = {
  // ── FILTERS ── (cutoff clamped 100–16000 Hz en firmware; resonance clamped 0.5–20; gain clamped ±12 dB)
  lowpass:    { label:'LOW PASS',    color:'cyan',   icon:'⏣', params:[ {id:'cutoff', label:'Cutoff', min:100, max:16000, def:1000, unit:'Hz', log:true}, {id:'resonance', label:'Res', min:0.5, max:20, def:0.707, step:0.01} ] },
  highpass:   { label:'HI PASS',     color:'cyan',   icon:'⏣', params:[ {id:'cutoff', label:'Cutoff', min:100, max:16000, def:1000, unit:'Hz', log:true}, {id:'resonance', label:'Res', min:0.5, max:20, def:0.707, step:0.01} ] },
  bandpass:   { label:'BAND PASS',   color:'blue',   icon:'⏣', params:[ {id:'cutoff', label:'Cutoff', min:100, max:16000, def:1000, unit:'Hz', log:true}, {id:'resonance', label:'Q', min:0.5, max:20, def:1, step:0.01} ] },
  notch:     { label:'NOTCH',     color:'silver',  icon:'⏣', params:[ {id:'cutoff', label:'Cutoff', min:100, max:16000, def:800,  unit:'Hz', log:true}, {id:'resonance', label:'Q',   min:0.5, max:20, def:1,     step:0.01} ] },
  allpass:   { label:'ALL PASS',  color:'slate',   icon:'⏣', params:[ {id:'cutoff', label:'Freq',   min:100, max:16000, def:1000, unit:'Hz', log:true}, {id:'resonance', label:'Q',   min:0.5, max:20, def:0.707, step:0.01} ] },
  peaking:   { label:'PEAKING',   color:'amber',   icon:'⏣', params:[ {id:'cutoff', label:'Freq',   min:100, max:16000, def:1000, unit:'Hz', log:true}, {id:'resonance', label:'Q',   min:0.5, max:20, def:1,     step:0.01}, {id:'gain', label:'Gain', min:-12, max:12, def:6, unit:'dB', step:0.5} ] },
  lowshelf:  { label:'LO SHELF',  color:'lime',    icon:'⏣', params:[ {id:'cutoff', label:'Freq',   min:100, max:5000,  def:200,  unit:'Hz', log:true}, {id:'gain', label:'Gain', min:-12, max:12, def:6, unit:'dB', step:0.5} ] },
  highshelf: { label:'HI SHELF',  color:'lavender',icon:'⏣', params:[ {id:'cutoff', label:'Freq',   min:200,max:16000, def:8000, unit:'Hz', log:true}, {id:'gain', label:'Gain', min:-12, max:12, def:6, unit:'dB', step:0.5} ] },
  resonant:  { label:'RESONANT',  color:'hotpink', icon:'⏣', params:[ {id:'cutoff', label:'Cutoff', min:100, max:16000, def:800,  unit:'Hz', log:true}, {id:'resonance', label:'Res', min:0.5, max:20, def:5,     step:0.1}  ] },
  // ── FX ──
  echo:       { label:'ECHO (LEGACY)', color:'orange', icon:'◎', params:[ {id:'time', label:'Time', min:10, max:200, def:100, unit:'ms'}, {id:'feedback', label:'Feedback', min:0, max:95, def:40, unit:'%'}, {id:'mix', label:'Mix', min:0, max:100, def:50, unit:'%'} ] },
  delay:      { label:'ECHO / DELAY', color:'yellow', icon:'◉', params:[ {id:'time', label:'Time', min:10, max:200, def:100, unit:'ms'}, {id:'feedback', label:'Feedback', min:0, max:95, def:50, unit:'%'}, {id:'mix', label:'Mix', min:0, max:100, def:50, unit:'%'} ] },
  bitcrusher: { label:'BITCRUSHER',  color:'purple', icon:'▦', params:[ {id:'bits', label:'Bit Depth', min:1, max:16, def:8, step:1} ] },
  distortion: { label:'DISTORTION',  color:'pink',   icon:'⚡', params:[ {id:'amount', label:'Amount', min:0, max:100, def:50, unit:'%'}, {id:'mode', label:'Mode', type:'select', options:['SOFT','HARD','TUBE','FUZZ'], def:0} ] },
  compressor: { label:'COMPRESSOR',  color:'green',  icon:'▬', params:[ {id:'threshold', label:'Threshold', min:0, max:100, def:60, unit:'%'}, {id:'ratio', label:'Ratio', min:1, max:20, def:4, step:0.5} ] },
  flanger:    { label:'FLANGER',     color:'teal',   icon:'≋', params:[ {id:'rate', label:'Rate', min:0, max:100, def:30, unit:'%'}, {id:'depth', label:'Depth', min:0, max:100, def:50, unit:'%'}, {id:'feedback', label:'Feedback', min:0, max:90, def:40, unit:'%'} ] },
  phaser:     { label:'PHASER',      color:'violet', icon:'◐', params:[ {id:'rate', label:'Rate', min:0, max:100, def:30, unit:'%'}, {id:'depth', label:'Depth', min:0, max:100, def:50, unit:'%'}, {id:'feedback', label:'Feedback', min:0, max:90, def:40, unit:'%'} ] }
};

const FILTER_TYPE_MAP = { lowpass:1, highpass:2, bandpass:3, notch:4, allpass:5, peaking:6, lowshelf:7, highshelf:8, resonant:9 };
const FX_SLOT_BY_TYPE = {
  lowpass:'filter', highpass:'filter', bandpass:'filter', notch:'filter', allpass:'filter',
  peaking:'filter', lowshelf:'filter', highshelf:'filter', resonant:'filter',
  echo:'echo', delay:'echo', bitcrusher:'bitcrusher', distortion:'distortion',
  compressor:'compressor', flanger:'flanger'
};
const TRACK_FX_SLOTS = ['filter', 'distortion', 'bitcrusher', 'echo', 'flanger', 'compressor'];
const UNSUPPORTED_PRE_FX = new Set(['phaser']);

const FACTORY_PRESETS = [
  {
    id: 'tight-bus',
    name: 'TIGHT BUS',
    description: 'HPF + Compressor — pegada limpia y punch',
    chain: [
      { key: 'hpf', fxType: 'highpass', x: 760, y: 500, params: { cutoff: 55, resonance: 0.82 } },
      { key: 'comp', fxType: 'compressor', x: 1120, y: 500, params: { threshold: 62, ratio: 5 } }
    ]
  },
  {
    id: 'lofi-crunch',
    name: 'LOFI CRUNCH',
    description: 'Bitcrusher + Distortion + Delay — cinta sucia',
    chain: [
      { key: 'crush', fxType: 'bitcrusher', x: 720, y: 660, params: { bits: 7 } },
      { key: 'dist', fxType: 'distortion', x: 1080, y: 780, params: { amount: 58, mode: 2 } },
      { key: 'dly', fxType: 'delay', x: 1440, y: 660, params: { time: 200, feedback: 44, mix: 36 } }
    ]
  },
  {
    id: 'space-wide',
    name: 'SPACE WIDE',
    description: 'BandPass + Echo + Comp — atmósfera amplia',
    chain: [
      { key: 'bp', fxType: 'bandpass', x: 680, y: 440, params: { cutoff: 1450, resonance: 1.1 } },
      { key: 'ph', fxType: 'phaser', x: 1020, y: 600, params: { rate: 34, depth: 62, feedback: 28 } },
      { key: 'rv', fxType: 'echo', x: 1360, y: 440, params: { time: 200, feedback: 26, mix: 40 } },
      { key: 'cp', fxType: 'compressor', x: 1700, y: 600, params: { threshold: 56, ratio: 3.5 } }
    ]
  },
  {
    id: 'dub-echo',
    name: 'DUB ECHO',
    description: 'Echo largo + LowPass — ecos jamaicanos',
    chain: [
      { key: 'dly', fxType: 'delay', x: 720, y: 480, params: { time: 180, feedback: 72, mix: 55 } },
      { key: 'rv', fxType: 'echo', x: 1100, y: 640, params: { time: 200, feedback: 50, mix: 45 } },
      { key: 'lpf', fxType: 'lowpass', x: 1480, y: 480, params: { cutoff: 2200, resonance: 1.2 } }
    ]
  },
  {
    id: '808-boom',
    name: '808 BOOM',
    description: 'LowShelf boost + Comp — sub potente estilo trap',
    chain: [
      { key: 'ls', fxType: 'lowshelf', x: 780, y: 560, params: { cutoff: 120, gain: 9 } },
      { key: 'comp', fxType: 'compressor', x: 1200, y: 420, params: { threshold: 45, ratio: 6 } }
    ]
  },
  {
    id: 'industrial',
    name: 'INDUSTRIAL',
    description: 'Distortion heavy + Bitcrusher — sonido agresivo',
    chain: [
      { key: 'dist', fxType: 'distortion', x: 700, y: 700, params: { amount: 82, mode: 3 } },
      { key: 'crush', fxType: 'bitcrusher', x: 1100, y: 500, params: { bits: 4 } },
      { key: 'hpf', fxType: 'highpass', x: 1500, y: 700, params: { cutoff: 200, resonance: 1.5 } }
    ]
  },
  {
    id: 'techno-acid',
    name: 'TECHNO ACID',
    description: 'LP resonante + Distortion — 303 acid vibes',
    chain: [
      { key: 'reso', fxType: 'resonant', x: 700, y: 520, params: { cutoff: 600, resonance: 12 } },
      { key: 'dist', fxType: 'distortion', x: 1100, y: 680, params: { amount: 40, mode: 1 } },
      { key: 'comp', fxType: 'compressor', x: 1500, y: 520, params: { threshold: 55, ratio: 4 } }
    ]
  },
  {
    id: 'vinyl-warmth',
    name: 'VINYL WARMTH',
    description: 'LPF suave + Bitcrusher ligero — calidez analógica',
    chain: [
      { key: 'lpf', fxType: 'lowpass', x: 740, y: 600, params: { cutoff: 6500, resonance: 0.6 } },
      { key: 'crush', fxType: 'bitcrusher', x: 1120, y: 440, params: { bits: 12 } },
      { key: 'rv', fxType: 'echo', x: 1500, y: 600, params: { time: 80, feedback: 18, mix: 22 } }
    ]
  },
  {
    id: 'glitch-stutter',
    name: 'GLITCH STUTTER',
    description: 'Delay corto + Crusher + Flanger — caos controlado',
    chain: [
      { key: 'dly', fxType: 'delay', x: 680, y: 460, params: { time: 30, feedback: 65, mix: 60 } },
      { key: 'crush', fxType: 'bitcrusher', x: 1080, y: 700, params: { bits: 6 } },
      { key: 'fl', fxType: 'flanger', x: 1480, y: 460, params: { rate: 70, depth: 80, feedback: 55 } }
    ]
  },
  {
    id: 'ambient-wash',
    name: 'AMBIENT WASH',
    description: 'Echo largo — paisaje sonoro etéreo',
    chain: [
      { key: 'rv', fxType: 'echo', x: 720, y: 540, params: { time: 200, feedback: 60, mix: 65 } },
      { key: 'ph', fxType: 'phaser', x: 1140, y: 380, params: { rate: 10, depth: 45, feedback: 20 } },
      { key: 'lpf', fxType: 'lowpass', x: 1540, y: 540, params: { cutoff: 4000, resonance: 0.7 } }
    ]
  },
  {
    id: 'hiphop-grit',
    name: 'HIP-HOP GRIT',
    description: 'Peaking mid + Compressor + Dist suave — boom bap sucio',
    chain: [
      { key: 'pk', fxType: 'peaking', x: 700, y: 640, params: { cutoff: 800, resonance: 2, gain: 4 } },
      { key: 'dist', fxType: 'distortion', x: 1100, y: 480, params: { amount: 25, mode: 0 } },
      { key: 'comp', fxType: 'compressor', x: 1500, y: 640, params: { threshold: 50, ratio: 7 } }
    ]
  },
  {
    id: 'minimal-clean',
    name: 'MINIMAL CLEAN',
    description: 'HPF + HiShelf treble + Comp — claridad minimal techno',
    chain: [
      { key: 'hpf', fxType: 'highpass', x: 760, y: 440, params: { cutoff: 150, resonance: 0.7 } },
      { key: 'hs', fxType: 'highshelf', x: 1160, y: 580, params: { cutoff: 6000, gain: 3 } },
      { key: 'comp', fxType: 'compressor', x: 1560, y: 440, params: { threshold: 65, ratio: 3 } }
    ]
  },
  {
    id: 'telephone',
    name: 'TELEPHONE',
    description: 'BandPass estrecho — efecto teléfono/radio retro',
    chain: [
      { key: 'bp', fxType: 'bandpass', x: 800, y: 520, params: { cutoff: 1800, resonance: 5 } },
      { key: 'dist', fxType: 'distortion', x: 1240, y: 520, params: { amount: 15, mode: 0 } }
    ]
  },
  {
    id: 'flanger-sweep',
    name: 'FLANGER SWEEP',
    description: 'Flanger profundo + Delay — barrido metálico espacial',
    chain: [
      { key: 'fl', fxType: 'flanger', x: 700, y: 500, params: { rate: 20, depth: 85, feedback: 65 } },
      { key: 'dly', fxType: 'delay', x: 1100, y: 660, params: { time: 120, feedback: 35, mix: 40 } },
      { key: 'comp', fxType: 'compressor', x: 1500, y: 500, params: { threshold: 58, ratio: 3.5 } }
    ]
  },
  {
    id: 'tape-saturation',
    name: 'TAPE SATURATION',
    description: 'Distortion suave + LPF + Comp — saturación de cinta',
    chain: [
      { key: 'dist', fxType: 'distortion', x: 740, y: 580, params: { amount: 30, mode: 2 } },
      { key: 'lpf', fxType: 'lowpass', x: 1140, y: 420, params: { cutoff: 8000, resonance: 0.5 } },
      { key: 'comp', fxType: 'compressor', x: 1540, y: 580, params: { threshold: 55, ratio: 3 } }
    ]
  },
  {
    id: 'massive-reverb',
    name: '⚡ MASSIVE REVERB',
    description: 'Echo al MÁXIMO — feedback y mezcla extremos',
    chain: [
      { key: 'rv', fxType: 'echo', x: 780, y: 500, params: { time: 200, feedback: 92, mix: 95 } },
      { key: 'lpf', fxType: 'lowpass', x: 1260, y: 640, params: { cutoff: 3000, resonance: 1.5 } }
    ]
  },
  {
    id: 'extreme-flanger',
    name: '⚡ EXTREME FLANGER',
    description: 'Flanger al MÁXIMO — jet engine psicodélico',
    chain: [
      { key: 'fl', fxType: 'flanger', x: 1000, y: 520, params: { rate: 85, depth: 95, feedback: 88 } }
    ]
  },
  {
    id: 'cathedral',
    name: '⚡ CATHEDRAL',
    description: 'Echo amplio — espacio denso y profundo',
    chain: [
      { key: 'rv1', fxType: 'echo', x: 680, y: 440, params: { time: 200, feedback: 85, mix: 80 } },
      { key: 'ph', fxType: 'phaser', x: 1100, y: 680, params: { rate: 8, depth: 70, feedback: 60 } },
      { key: 'rv2', fxType: 'echo', x: 1520, y: 440, params: { time: 150, feedback: 70, mix: 75 } }
    ]
  },
  {
    id: 'robot-voice',
    name: '⚡ ROBOT VOICE',
    description: 'Flanger extremo + Crusher — droide loco',
    chain: [
      { key: 'fl', fxType: 'flanger', x: 680, y: 660, params: { rate: 95, depth: 90, feedback: 85 } },
      { key: 'ph', fxType: 'phaser', x: 1100, y: 440, params: { rate: 80, depth: 90, feedback: 75 } },
      { key: 'crush', fxType: 'bitcrusher', x: 1520, y: 660, params: { bits: 5 } }
    ]
  }
];

const CANVAS_W = 2800;
const CANVAS_H = 2200;
const SNAP = 20;

/* ──────────── STATE ──────────── */
let nodes = [];
let cables = [];
let nextId = 1;
let ws = null;
let wsConnected = false;
let isPlaying = false;
let currentTempo = 120;
let currentStep = -1;
let stepPulseTimer = null;
let serverPlaying = null;
let stepDrivenPlaying = false;
let stepDrivenTimer = null;
let signalDemoMode = false;
let viewZoom = 1;
let gridVisible = true;
let heatMode = 'on'; // 'off' | 'on' | 'auto'
let sceneQuantizeEnabled = true;
let pendingSceneState = null;
let pendingSceneLabel = '';
let trackVolumes = new Array(16).fill(100);
let trackPeaks = new Array(16).fill(0);
let trackMuted = new Array(16).fill(false);
const throttledCmdTimers = new Map();
const queuedWsPayloads = [];
const appliedRouteSlots = Array.from({ length: 16 }, () => new Map());
let routeReconcileTimer = null;
let routeReconcileForce = false;
let wsFlushTimer = null;
let lastWsSendTs = 0;
const WS_MIN_SEND_GAP_MS = 12;
const WS_MAX_QUEUE = 240;
const WS_BOOT_SYNC_DELAY_MS = 34;
let activeMacroScene = 'A';
let macrosEnabled = true;
let macroScenes = {
  A: [45, 35, 55, 40],
  B: [70, 20, 68, 55],
  C: [25, 62, 34, 22],
  D: [55, 48, 72, 68]
};

/* Drag state */
let drag = null;   // { type:'node'|'cable', nodeId, startX, startY, offsetX, offsetY, fromId, fromType }
let previewPath = null;
let selectedCable = null;
let editingNode = null;
let activeTool = 'patch'; // 'patch' | 'hand' | 'select'
const selectedNodeIds = new Set();

/* DOM refs */
let svgEl, nodesEl, canvasEl, worldEl, selectionBoxEl;

function setPBMenuOpen(open) {
  const menu = document.getElementById('pbMenu');
  const backdrop = document.getElementById('pbMenuBackdrop');
  if (!menu || !backdrop) return;
  menu.classList.toggle('hidden', !open);
  backdrop.classList.toggle('hidden', !open);
}

function updateToolButtons() {
  const handBtn = document.getElementById('pbToolHandBtn');
  const selectBtn = document.getElementById('pbToolSelectBtn');
  if (handBtn) handBtn.classList.toggle('is-active', activeTool === 'hand');
  if (selectBtn) selectBtn.classList.toggle('is-active', activeTool === 'select');

  if (canvasEl) {
    canvasEl.classList.toggle('pb-tool-hand', activeTool === 'hand');
    canvasEl.classList.toggle('pb-tool-select', activeTool === 'select');
  }
}

function clearNodeSelection() {
  selectedNodeIds.clear();
  renderNodeSelection();
}

function renderNodeSelection() {
  if (!nodesEl) return;
  nodesEl.querySelectorAll('.pb-node').forEach(el => {
    const nodeId = el.dataset.nodeId;
    el.classList.toggle('is-selected', selectedNodeIds.has(nodeId));
  });
}

function selectNodesInWorldRect(x1, y1, x2, y2, additive = false) {
  if (!additive) selectedNodeIds.clear();

  const left = Math.min(x1, x2);
  const right = Math.max(x1, x2);
  const top = Math.min(y1, y2);
  const bottom = Math.max(y1, y2);

  nodes.forEach(node => {
    const width = node.type === 'pad' || node.type === 'fx' || node.type === 'master' || node.type === 'bus' ? 170 : 170;
    const height = node.type === 'pad' ? 72 : 80;
    const intersects = !(node.x + width < left || node.x > right || node.y + height < top || node.y > bottom);
    if (intersects) selectedNodeIds.add(node.id);
  });

  renderNodeSelection();
}

function showSelectionBoxWorldRect(x1, y1, x2, y2) {
  if (!selectionBoxEl) return;
  const left = Math.min(x1, x2);
  const right = Math.max(x1, x2);
  const top = Math.min(y1, y2);
  const bottom = Math.max(y1, y2);

  const vx = left * viewZoom - canvasEl.scrollLeft;
  const vy = top * viewZoom - canvasEl.scrollTop;
  const vw = Math.max(1, (right - left) * viewZoom);
  const vh = Math.max(1, (bottom - top) * viewZoom);

  selectionBoxEl.style.display = 'block';
  selectionBoxEl.style.left = `${vx}px`;
  selectionBoxEl.style.top = `${vy}px`;
  selectionBoxEl.style.width = `${vw}px`;
  selectionBoxEl.style.height = `${vh}px`;
}

function hideSelectionBox() {
  if (!selectionBoxEl) return;
  selectionBoxEl.style.display = 'none';
}

function setActiveTool(nextTool) {
  const target = (nextTool === 'hand' || nextTool === 'select') ? nextTool : 'patch';
  activeTool = (activeTool === target) ? 'patch' : target;

  if (activeTool !== 'select') {
    clearNodeSelection();
    hideSelectionBox();
  }
  updateToolButtons();
}

/* ──────────── INIT ──────────── */
function init() {
  svgEl   = document.getElementById('pbSVG');
  nodesEl = document.getElementById('pbNodes');
  canvasEl= document.getElementById('pbCanvas');
  worldEl = document.getElementById('pbWorld');
  selectionBoxEl = document.getElementById('pbSelectionBox');

  /* Embed mode — when loaded inside multiview iframe, hide header + toolbar */
  if (new URLSearchParams(location.search).get('embed') === '1') {
    document.getElementById('patchbay')?.classList.add('embed-mode');
  }

  updateToolButtons();
  renderNodeSelection();

  /* Set canvas size */
  nodesEl.style.width  = CANVAS_W + 'px';
  nodesEl.style.height = CANVAS_H + 'px';
  svgEl.setAttribute('width',  CANVAS_W);
  svgEl.setAttribute('height', CANVAS_H);
  svgEl.style.width  = CANVAS_W + 'px';
  svgEl.style.height = CANVAS_H + 'px';

  loadViewPrefs();
  applyZoom();
  applyGridVisibility();
  updateHeatModeButton();
  updateQuantizeButton();
  loadMacroScenes();
  initMacroPanel();

  /* Create initial sampler sources */
  for (let i = 0; i < Math.min(INITIAL_SOURCE_COUNT, TRACK_NAMES.length); i++) {
    createPadNode(i, 60, 60 + i * 120);
  }
  /* Create Master Out */
  createMasterNode(CANVAS_W - 260, 500);
  createBusNode('bus-a', 'BUS A', CANVAS_W - 560, 380, 'teal');
  createBusNode('bus-b', 'BUS B', CANVAS_W - 560, 640, 'violet');

  /* Restore saved patch topology — muestra cables/FX de la sesión anterior */
  loadState();
  refreshSourcePicker();

  /* Events */
  canvasEl.addEventListener('pointerdown', onPointerDown);
  canvasEl.addEventListener('pointermove', onPointerMove);
  canvasEl.addEventListener('pointerup', onPointerUp);
  canvasEl.addEventListener('pointercancel', onPointerUp);
  document.addEventListener('keydown', onKeyDown);
  document.addEventListener('click', onDocClick);

  /* WebSocket */
  connectWS();

  applyTempoVisuals();

  updateStatus();
}

function pbToggleEmbedToolbar() {
  const toolbar = document.getElementById('pbToolbar');
  const btn     = document.getElementById('pbEmbedToolbarToggle');
  if (!toolbar || !btn) return;
  const isOpen = toolbar.classList.toggle('embed-toolbar-open');
  btn.classList.toggle('open', isOpen);
  btn.textContent = isOpen ? '⊖ FX TOOLS' : '⊕ FX TOOLS';
}

function parsePadTrackId(nodeId) {
  if (typeof nodeId !== 'string' || !nodeId.startsWith('pad-')) return -1;
  const track = parseInt(nodeId.slice(4), 10);
  if (!Number.isInteger(track) || track < 0 || track >= TRACK_NAMES.length) return -1;
  return track;
}

function getMissingPadTracks() {
  const used = new Set(nodes.filter(n => n.type === 'pad').map(n => n.track));
  const missing = [];
  for (let track = 0; track < TRACK_NAMES.length; track++) {
    if (!used.has(track)) missing.push(track);
  }
  return missing;
}

function getActivePadTracks() {
  return nodes
    .filter(n => n.type === 'pad')
    .map(n => n.track)
    .filter(track => Number.isInteger(track) && track >= 0 && track < TRACK_NAMES.length)
    .sort((a, b) => a - b);
}

function getNextPadSlotY() {
  const padNodes = nodes.filter(n => n.type === 'pad');
  if (!padNodes.length) return 60;
  const maxY = padNodes.reduce((acc, node) => Math.max(acc, node.y), 60);
  return snap(maxY + 120);
}

function refreshSourcePicker() {
  const picker = document.getElementById('pbSourcePicker');
  const activePicker = document.getElementById('pbActiveSourcePicker');
  if (!picker) return;

  const addBtn = document.getElementById('pbAddSourceBtn');
  const removeBtn = document.getElementById('pbRemoveSourceBtn');
  const addAllBtn = document.getElementById('pbAddAllSourcesBtn');
  const countEl = document.getElementById('pbSourceCount');
  const missing = getMissingPadTracks();
  const activeTracks = getActivePadTracks();
  const activeCount = activeTracks.length;

  picker.innerHTML = '';
  if (missing.length) {
    missing.forEach(track => {
      const opt = document.createElement('option');
      opt.value = String(track);
      opt.textContent = `PAD ${track + 1} · ${TRACK_LABELS[track] || TRACK_NAMES[track]}`;
      picker.appendChild(opt);
    });
  } else {
    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = 'All sampler sources added';
    picker.appendChild(opt);
  }

  const disabled = !missing.length;
  picker.disabled = disabled;
  if (addBtn) addBtn.disabled = disabled;
  if (addAllBtn) addAllBtn.disabled = disabled;

  if (activePicker) {
    activePicker.innerHTML = '';
    if (activeTracks.length) {
      activeTracks.forEach(track => {
        const opt = document.createElement('option');
        opt.value = String(track);
        opt.textContent = `PAD ${track + 1} · ${TRACK_LABELS[track] || TRACK_NAMES[track]}`;
        activePicker.appendChild(opt);
      });
      activePicker.disabled = false;
    } else {
      const opt = document.createElement('option');
      opt.value = '';
      opt.textContent = 'No active sampler sources';
      activePicker.appendChild(opt);
      activePicker.disabled = true;
    }
  }

  if (removeBtn) removeBtn.disabled = !activeTracks.length;
  if (countEl) countEl.textContent = `${activeCount}/${TRACK_NAMES.length}`;
}

function addPadSource(track) {
  const parsedTrack = Number(track);
  if (!Number.isInteger(parsedTrack) || parsedTrack < 0 || parsedTrack >= TRACK_NAMES.length) return false;
  if (nodes.find(n => n.type === 'pad' && n.track === parsedTrack)) return false;

  createPadNode(parsedTrack, 60, getNextPadSlotY());
  updateStatus();
  saveState();
  refreshSourcePicker();
  return true;
}

function removePadSource(track) {
  const parsedTrack = Number(track);
  if (!Number.isInteger(parsedTrack) || parsedTrack < 0 || parsedTrack >= TRACK_NAMES.length) return false;

  const padNode = nodes.find(n => n.type === 'pad' && n.track === parsedTrack);
  if (!padNode) return false;

  const relatedCables = cables.filter(c => c.from === padNode.id || c.to === padNode.id);
  relatedCables.forEach(cable => clearConnection(cable));
  cables = cables.filter(c => c.from !== padNode.id && c.to !== padNode.id);

  const nodeEl = document.getElementById('node-' + padNode.id);
  if (nodeEl) nodeEl.remove();
  nodes = nodes.filter(n => n.id !== padNode.id);
  selectedNodeIds.delete(padNode.id);
  renderNodeSelection();

  if (selectedCable && !cables.find(c => c.id === selectedCable)) {
    selectedCable = null;
  }

  renderCables();
  updateStatus();
  saveState();
  refreshSourcePicker();
  return true;
}

function ensurePadNodesForState(data) {
  const requiredTracks = new Set();

  if (Array.isArray(data?.padPositions)) {
    data.padPositions.forEach(pp => {
      const track = parsePadTrackId(pp?.id);
      if (track >= 0) requiredTracks.add(track);
    });
  }

  if (Array.isArray(data?.cables)) {
    data.cables.forEach(cd => {
      const fromTrack = parsePadTrackId(cd?.from);
      const toTrack = parsePadTrackId(cd?.to);
      if (fromTrack >= 0) requiredTracks.add(fromTrack);
      if (toTrack >= 0) requiredTracks.add(toTrack);
    });
  }

  requiredTracks.forEach(track => {
    if (!nodes.find(n => n.type === 'pad' && n.track === track)) {
      createPadNode(track, 60, 60 + track * 120);
    }
  });
}

/* ──────────── WEBSOCKET ──────────── */
function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(proto + '://' + location.host + '/ws');
  ws.onopen = () => {
    wsConnected = true;
    document.getElementById('pbWsStatus').classList.add('connected');
    const status = document.getElementById('pbStatus');
    if (status) status.textContent = 'Sincronizando rutas…';
    flushWsQueue();
    syncConnectionsAfterReconnect();
    sendCmd('getTrackVolumes', {});
  };
  ws.onclose = () => {
    wsConnected = false;
    document.getElementById('pbWsStatus').classList.remove('connected');
    const status = document.getElementById('pbStatus');
    if (status) status.textContent = 'OFFLINE · reconectando…';
    queuedWsPayloads.length = 0;
    if (wsFlushTimer) {
      clearTimeout(wsFlushTimer);
      wsFlushTimer = null;
    }
    setTimeout(connectWS, 3000);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = (e) => {
    if (e.data instanceof ArrayBuffer) {
      ingestAudioLevelBuffer(new Uint8Array(e.data));
      return;
    }
    if (typeof e.data !== 'string') return;
    try { handleWSMessage(JSON.parse(e.data)); } catch(ex) {}
  };
}

/* Reset all per-track FX in firmware to match an empty patchbay canvas.
   Sent staggered to avoid WS burst on reconnect. */
function resetFirmwareFX() {
  for (let t = 0; t < 16; t++) {
    const track = t;
    const delay = t * WS_BOOT_SYNC_DELAY_MS;
    setTimeout(() => {
      sendCmd('clearTrackFilter',   { track });
      sendCmd('setTrackDistortion', { track, amount: 0, mode: 0 });
      sendCmd('setTrackBitCrush',   { track, value: 16 });
      sendCmd('setTrackEcho',       { track, active: false, time: 200, feedback: 40, mix: 50 });
      sendCmd('setTrackFlanger',    { track, active: false, rate: 30, depth: 50, feedback: 40 });
      sendCmd('setTrackCompressor', { track, active: false, threshold: 60, ratio: 4 });
    }, delay);
  }
  console.log('[PATCH] Firmware FX reset enviado (canvas vacío).');
}

function syncConnectionsAfterReconnect() {
  /* Descarta cola pre-conexión (los sendCmd antes del WS open se descartaron) */
  queuedWsPayloads.length = 0;
  if (wsFlushTimer) { clearTimeout(wsFlushTimer); wsFlushTimer = null; }

  /* Recalcular por slots reales, no por número de cables. Esto cubre grafos
     grandes y evita que dos filtros/Echos visuales compitan silenciosamente. */
  appliedRouteSlots.forEach(slots => slots.clear());
  scheduleRouteReconcile(true);
  console.log(`[PATCH] Re-sync determinista de ${cables.length} cables.`);
}

function flushWsQueue() {
  wsFlushTimer = null;
  if (!ws || ws.readyState !== 1) return;
  if (!queuedWsPayloads.length) return;

  const now = performance.now();
  const elapsed = now - lastWsSendTs;
  if (elapsed < WS_MIN_SEND_GAP_MS) {
    wsFlushTimer = setTimeout(flushWsQueue, WS_MIN_SEND_GAP_MS - elapsed);
    return;
  }

  const payload = queuedWsPayloads.shift();
  ws.send(payload);
  lastWsSendTs = performance.now();

  if (queuedWsPayloads.length) {
    wsFlushTimer = setTimeout(flushWsQueue, WS_MIN_SEND_GAP_MS);
  }
}

function sendCmd(cmd, data) {
  if (!ws || ws.readyState !== 1) {
    console.warn('[PATCH] sendCmd DROPPED (WS not connected):', cmd, data);
    return;
  }
  const payload = JSON.stringify(Object.assign({ cmd }, data));
  console.log('[PATCH] sendCmd:', cmd, JSON.stringify(data));

  if (queuedWsPayloads.length >= WS_MAX_QUEUE) {
    queuedWsPayloads.shift();
  }
  queuedWsPayloads.push(payload);

  if (!wsFlushTimer) {
    flushWsQueue();
  }
}

function sendCmdThrottled(key, cmd, data, delay = 70) {
  const prev = throttledCmdTimers.get(key);
  if (prev) clearTimeout(prev);
  const tid = setTimeout(() => {
    throttledCmdTimers.delete(key);
    sendCmd(cmd, data);
  }, Math.max(0, delay));
  throttledCmdTimers.set(key, tid);
}

function getMacroSliderValues() {
  const values = [];
  for (let i = 1; i <= 4; i++) {
    const el = document.getElementById(`pbMacro${i}`);
    values.push(el ? parseInt(el.value || '0', 10) : 0);
  }
  return values.map(v => Number.isNaN(v) ? 0 : Math.max(0, Math.min(100, v)));
}

function setMacroSliderValues(values) {
  if (!Array.isArray(values)) return;
  for (let i = 1; i <= 4; i++) {
    const el = document.getElementById(`pbMacro${i}`);
    if (!el) continue;
    const v = Math.max(0, Math.min(100, parseInt(values[i - 1] ?? 0, 10) || 0));
    el.value = String(v);
  }
}

function updateMacroSceneButtons() {
  ['A','B','C','D'].forEach(scene => {
    const btn = document.getElementById(`pbMacroScene${scene}`);
    if (!btn) return;
    btn.classList.toggle('is-active', scene === activeMacroScene);
  });
}

function saveMacroScenes() {
  try {
    localStorage.setItem('pb_macro_scenes', JSON.stringify({ active: activeMacroScene, scenes: macroScenes }));
  } catch(ex) {}
}

function loadMacroScenes() {
  try {
    const raw = localStorage.getItem('pb_macro_scenes');
    if (!raw) return;
    const parsed = JSON.parse(raw);
    if (parsed && parsed.scenes) {
      ['A','B','C','D'].forEach(scene => {
        if (Array.isArray(parsed.scenes[scene]) && parsed.scenes[scene].length >= 4) {
          macroScenes[scene] = parsed.scenes[scene].slice(0, 4);
        }
      });
    }
    if (parsed && ['A','B','C','D'].includes(parsed.active)) {
      activeMacroScene = parsed.active;
    }
  } catch(ex) {}
}

function applyMacroValue(index, value) {
  if (!macrosEnabled) return; /* Macros disabled */
  const v = Math.max(0, Math.min(100, value));
  if (index === 1) {
    const cutoff = Math.round(200 + (v / 100) * 11800);
    sendCmdThrottled('macro:m1', 'setFilterCutoff', { value: cutoff }, 60);
  } else if (index === 2) {
    sendCmdThrottled('macro:m2:active', 'setDelayActive', { value: v > 0 }, 80);
    sendCmdThrottled('macro:m2:mix', 'setDelayMix', { value: v }, 80);
  } else if (index === 3) {
    const threshold = -50 + (v / 100) * 44;
    sendCmdThrottled('macro:m3:active', 'setCompressorActive', { value: v > 0 }, 90);
    sendCmdThrottled('macro:m3:thr', 'setCompressorThreshold', { value: threshold }, 90);
  } else if (index === 4) {
    sendCmdThrottled('macro:m4:sidechain', 'setSidechainPro', {
      active: v > 0,
      source: 0,
      destinations: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
      amount: v,
      attack: 6,
      release: 180,
      knee: 0.45
    }, 130);
  }
}

function applyMacroSceneValues(values) {
  const safe = (Array.isArray(values) ? values : [0,0,0,0]).slice(0, 4);
  setMacroSliderValues(safe);
  for (let i = 0; i < 4; i++) {
    applyMacroValue(i + 1, parseInt(safe[i], 10) || 0);
  }
}

function initMacroPanel() {
  // Only initialise UI — do NOT send WS commands on page load.
  // applyMacroSceneValues() will be called explicitly by the user via pbSelectMacroScene/pbLoadMacroScene.
  setMacroSliderValues(macroScenes[activeMacroScene]);
  updateMacroSceneButtons();

  for (let i = 1; i <= 4; i++) {
    const el = document.getElementById(`pbMacro${i}`);
    if (!el) continue;
    el.addEventListener('input', () => {
      const values = getMacroSliderValues();
      macroScenes[activeMacroScene] = values;
      applyMacroValue(i, values[i - 1]);
      saveMacroScenes();
    });
  }
  // Do NOT call applyMacroSceneValues here — avoid sending sidechain/FX commands on every page load.
}

window.pbSelectMacroScene = function(scene) {
  if (!['A','B','C','D'].includes(scene)) return;
  activeMacroScene = scene;
  updateMacroSceneButtons();
  applyMacroSceneValues(macroScenes[scene]);
  saveMacroScenes();
};

window.pbSaveMacroScene = function() {
  macroScenes[activeMacroScene] = getMacroSliderValues();
  saveMacroScenes();
};

window.pbLoadMacroScene = function() {
  applyMacroSceneValues(macroScenes[activeMacroScene]);
};

window.pbToggleMacros = function() {
  macrosEnabled = !macrosEnabled;
  const btn = document.getElementById('pbMacroToggleBtn');
  if (btn) {
    btn.textContent = macrosEnabled ? '🔊 MACROS ON' : '🔇 MACROS OFF';
    btn.style.background = macrosEnabled ? 'rgba(105,240,174,0.2)' : 'rgba(255,45,45,0.25)';
    btn.style.borderColor = macrosEnabled ? 'rgba(105,240,174,0.5)' : 'rgba(255,45,45,0.5)';
    btn.style.color = macrosEnabled ? '#69f0ae' : '#ff8a8a';
  }
  if (!macrosEnabled) {
    /* Send reset commands to firmware — clear all macro effects */
    sendCmd('setFilterCutoff', { value: 16000 });
    sendCmd('setDelayActive', { value: false });
    sendCmd('setCompressorActive', { value: false });
    sendCmd('setSidechainPro', { active: false, source: 0, destinations: [], amount: 0, attack: 6, release: 180, knee: 0.45 });
    console.log('[MACRO] Macros DISABLED — all master FX reset');
  } else {
    /* Re-apply current scene */
    applyMacroSceneValues(macroScenes[activeMacroScene]);
    console.log('[MACRO] Macros ENABLED — scene', activeMacroScene, 'applied');
  }
};

function handleWSMessage(msg) {
  if (msg.type === 'error') {
    const status = document.getElementById('pbStatus');
    if (status) {
      status.classList.add('is-warning');
      status.textContent = `ERROR · ${msg.cmd || msg.code || msg.msg || 'comando rechazado'}`;
    }
  }
  if ((msg.type === 'playState' || msg.type === 'sequencerState' || msg.type === 'status' || msg.type === 'state') && typeof msg.playing !== 'undefined') {
    isPlaying = !!msg.playing;
    serverPlaying = !!msg.playing;
    if (!serverPlaying) {
      stepDrivenPlaying = false;
      if (stepDrivenTimer) {
        clearTimeout(stepDrivenTimer);
        stepDrivenTimer = null;
      }
    }
    updatePlayingVisualState();
  }

  if (typeof msg.tempo !== 'undefined') {
    const parsedTempo = parseFloat(msg.tempo);
    if (!Number.isNaN(parsedTempo)) {
      currentTempo = parsedTempo;
      applyTempoVisuals();
    }
  }

  if (typeof msg.step !== 'undefined') {
    const parsedStep = parseInt(msg.step, 10);
    if (!Number.isNaN(parsedStep) && parsedStep !== currentStep) {
      currentStep = parsedStep;
      triggerStepPulse();
      if (pendingSceneState && parsedStep % 4 === 0) {
        applyQueuedSceneState();
      }
      if (serverPlaying !== true) {
        stepDrivenPlaying = true;
        updatePlayingVisualState();
        if (stepDrivenTimer) clearTimeout(stepDrivenTimer);
        stepDrivenTimer = setTimeout(() => {
          stepDrivenPlaying = false;
          updatePlayingVisualState();
        }, 850);
      }
    }
  }

  if (Array.isArray(msg.trackVolumes)) {
    ingestTrackVolumes(msg.trackVolumes);
  }

  if (msg.type === 'trackVolumes' && Array.isArray(msg.volumes)) {
    ingestTrackVolumes(msg.volumes);
  }

  if ((msg.type === 'trackVolumeSet' || msg.type === 'trackVolume') && typeof msg.track !== 'undefined' && typeof msg.volume !== 'undefined') {
    setTrackVolume(msg.track, msg.volume);
  }

  if (msg.type === 'masterFx') {
    applyMasterFxMessage(msg.param, msg.value);
  }

  /* Update sample names on pads if available */
  if (msg.type === 'sampleLoaded' || msg.type === 'kitLoaded') {
    // Could update pad labels here
  }

  /* Per-track filter feedback — warn if firmware rejected (max 8 active) */
  if (msg.type === 'trackFilterSet' && msg.success === false) {
    const statusEl = document.getElementById('pbStatus');
    if (statusEl) {
      statusEl.textContent = '⚠ Límite: máx 8 filtros de track activos simultáneos';
      statusEl.style.color = '#ff8a50';
      setTimeout(() => { statusEl.style.color = ''; updateStatus(); }, 4000);
    }
    const winner = getTrackRouteWinners(parseInt(msg.track, 10)).winners.get('filter');
    const winnerEl = winner ? document.getElementById('node-' + winner.id) : null;
    if (winnerEl) winnerEl.classList.add('is-rejected');
    appliedRouteSlots[parseInt(msg.track, 10)]?.delete('filter');
    console.warn('[PATCH] setTrackFilter rechazado por firmware (límite 8 activos)');
  }

  /* Sync: firmware tells us a filter was set (from sequencer or another client) */
  if (msg.type === 'trackFilterSet' && msg.success === true) {
    const winner = getTrackRouteWinners(parseInt(msg.track, 10)).winners.get('filter');
    const winnerEl = winner ? document.getElementById('node-' + winner.id) : null;
    if (winnerEl) winnerEl.classList.remove('is-rejected');
    syncFxFromFirmware(msg.track, 'filter', {
      filterType: msg.filterType,
      cutoff: msg.cutoff,
      resonance: msg.resonance
    });
  }
  if (msg.type === 'trackFilterCleared') {
    syncFxFromFirmware(msg.track, 'filterClear', {});
  }

  /* Sync: firmware tells us live FX state (echo/flanger/compressor) */
  if (msg.type === 'trackLiveFx') {
    syncFxFromFirmware(msg.track, msg.fx, {
      active: msg.active,
      time: msg.time, feedback: msg.feedback, mix: msg.mix,
      rate: msg.rate, depth: msg.depth,
      threshold: msg.threshold, ratio: msg.ratio
    });
  }

  /* Track mute state */
  if (msg.type === 'trackMuted' && typeof msg.track !== 'undefined') {
    const idx = parseInt(msg.track, 10);
    if (idx >= 0 && idx < 16) {
      trackMuted[idx] = !!msg.muted;
      console.log('[PATCH] trackMuted:', idx, trackMuted[idx], '| all:', trackMuted.map((m,i) => m ? i : null).filter(x => x !== null));
      renderCables();
    }
  }

  /* Initial state: bulk mute array */
  if (Array.isArray(msg.trackMuted)) {
    msg.trackMuted.forEach((m, i) => {
      if (i < 16) trackMuted[i] = !!m;
    });
    console.log('[PATCH] bulk trackMuted:', trackMuted.map((m,i) => m ? i : null).filter(x => x !== null));
    renderCables();
  }

  /* Initial state: bulk track filter array — save for sync */
  if (Array.isArray(msg.trackFilters)) {
    saveFirmwareFiltersToShared(msg.trackFilters);
  }
}

function postSyncModuleUI(mod) {
  const btn = document.getElementById(postToggleId(mod));
  const modEl = btn ? btn.closest('.post-module') : null;
  if (btn) {
    btn.textContent = postState[mod] ? 'ON' : 'OFF';
    btn.classList.toggle('on', postState[mod]);
  }
  if (modEl) modEl.classList.toggle('active', postState[mod]);
}

function applyMasterFxMessage(param, value) {
  const setSlider = (id, val, textId = null, suffix = '') => {
    const el = document.getElementById(id);
    if (!el) return;
    el.value = val;
    if (textId) {
      const tx = document.getElementById(textId);
      if (tx) tx.textContent = `${val}${suffix}`;
    }
  };

  switch(param) {
    case 'filterType': {
      const sel = document.getElementById('postFilterType');
      if (sel) sel.value = parseInt(value, 10);
      postState.filter = parseInt(value, 10) > 0;
      postSyncModuleUI('filter');
      break;
    }
    case 'filterCutoff':
      setSlider('postFilterCutoff', Math.round(value), 'postFilterCutoffVal');
      break;
    case 'filterResonance': {
      const reson = Math.round(parseFloat(value) * 100);
      setSlider('postFilterReso', reson, 'postFilterResoVal', '');
      const tx = document.getElementById('postFilterResoVal');
      if (tx) tx.textContent = parseFloat(value).toFixed(1);
      break;
    }

    case 'distortion':
      setSlider('postDistAmount', Math.round(value), 'postDistAmountVal');
      break;
    case 'distortionMode': {
      const sel = document.getElementById('postDistMode');
      if (sel) sel.value = parseInt(value, 10);
      break;
    }

    case 'bitCrush':
      setSlider('postBitBits', parseInt(value, 10), 'postBitBitsVal');
      break;
    case 'sampleRate': {
      const sr = parseInt(value, 10);
      setSlider('postBitRate', sr, 'postBitRateVal');
      const tx = document.getElementById('postBitRateVal');
      if (tx) tx.textContent = sr >= 1000 ? (sr / 1000).toFixed(1) + 'k' : String(sr);
      break;
    }

    case 'phaserActive':
      postState.phaser = !!value;
      postSyncModuleUI('phaser');
      break;
    case 'phaserRate':
      setSlider('postPhaserRate', Math.round(value), 'postPhaserRateVal');
      break;
    case 'phaserDepth':
      setSlider('postPhaserDepth', Math.round(value), 'postPhaserDepthVal');
      break;
    case 'phaserFeedback':
      setSlider('postPhaserFb', Math.round(value), 'postPhaserFbVal');
      break;

    case 'flangerActive':
      postState.flanger = !!value;
      postSyncModuleUI('flanger');
      break;
    case 'flangerRate':
      setSlider('postFlangerRate', Math.round(value), 'postFlangerRateVal');
      break;
    case 'flangerDepth':
      setSlider('postFlangerDepth', Math.round(value), 'postFlangerDepthVal');
      break;
    case 'flangerFeedback':
      setSlider('postFlangerFb', Math.round(value), 'postFlangerFbVal');
      break;
    case 'flangerMix':
      setSlider('postFlangerMix', Math.round(value), 'postFlangerMixVal');
      break;

    case 'delayActive':
      postState.delay = !!value;
      postSyncModuleUI('delay');
      break;
    case 'delayTime': {
      const ms = Math.round(value);
      setSlider('postDelayTime', ms, 'postDelayTimeVal');
      const tx = document.getElementById('postDelayTimeVal');
      if (tx) tx.textContent = ms + 'ms';
      break;
    }
    case 'delayFeedback':
      setSlider('postDelayFb', Math.round(value), 'postDelayFbVal');
      break;
    case 'delayMix':
      setSlider('postDelayMix', Math.round(value), 'postDelayMixVal');
      break;

    case 'compressorActive':
      postState.compressor = !!value;
      postSyncModuleUI('compressor');
      break;
    case 'compressorThreshold': {
      const t = Math.round(value);
      setSlider('postCompThresh', t, 'postCompThreshVal');
      const tx = document.getElementById('postCompThreshVal');
      if (tx) tx.textContent = t + 'dB';
      break;
    }
    case 'compressorRatio': {
      const ratio = parseFloat(value);
      const ratioRaw = Math.round(ratio * 10);
      setSlider('postCompRatio', ratioRaw, 'postCompRatioVal');
      const tx = document.getElementById('postCompRatioVal');
      if (tx) tx.textContent = ratio.toFixed(1);
      break;
    }
    case 'compressorAttack': {
      const a = Math.round(value);
      setSlider('postCompAtk', a, 'postCompAtkVal');
      const tx = document.getElementById('postCompAtkVal');
      if (tx) tx.textContent = a + 'ms';
      break;
    }
    case 'compressorRelease': {
      const r = Math.round(value);
      setSlider('postCompRel', r, 'postCompRelVal');
      const tx = document.getElementById('postCompRelVal');
      if (tx) tx.textContent = r + 'ms';
      break;
    }
    case 'compressorMakeupGain': {
      const g = Math.round(value);
      setSlider('postCompGain', g, 'postCompGainVal');
      const tx = document.getElementById('postCompGainVal');
      if (tx) tx.textContent = g + 'dB';
      break;
    }
  }
  updateStatus();
}

function applyTempoVisuals() {
  const bpm = Math.max(40, Math.min(300, Number(currentTempo) || 120));
  const beat = 60 / bpm;
  const flowDuration = Math.max(0.22, Math.min(1.4, beat / 2));
  const pulseDuration = Math.max(0.18, Math.min(0.95, beat / 4));
  const root = document.getElementById('patchbay');
  if (!root) return;
  root.style.setProperty('--pb-flow-duration', `${flowDuration.toFixed(3)}s`);
  root.style.setProperty('--pb-pulse-duration', `${pulseDuration.toFixed(3)}s`);
  /* Update BPM display in transport */
  const bpmEl = document.getElementById('pbBpmDisplay');
  if (bpmEl) bpmEl.textContent = Math.round(bpm);
}

function updatePlayingVisualState() {
  const root = document.getElementById('patchbay');
  if (!root) return;
  const active = !!(signalDemoMode || serverPlaying || stepDrivenPlaying || isPlaying);
  root.classList.toggle('pb-playing', active);
  /* Update transport button */
  const playBtn = document.getElementById('pbPlayBtn');
  if (playBtn) {
    playBtn.classList.toggle('is-playing', active);
    playBtn.textContent = active ? '⏸' : '▶';
    playBtn.title = active ? 'Pause' : 'Play';
  }
}

window.pbTogglePlay = function() {
  sendCmd(isPlaying ? 'stop' : 'start', {});
};

window.pbStop = function() {
  sendCmd('stop', {});
};

function triggerStepPulse() {
  const root = document.getElementById('patchbay');
  if (!root) return;
  root.classList.remove('pb-step-hit');
  void root.offsetWidth;
  root.classList.add('pb-step-hit');
  if (stepPulseTimer) clearTimeout(stepPulseTimer);
  stepPulseTimer = setTimeout(() => root.classList.remove('pb-step-hit'), 110);
}

function clampTrackVolume(v) {
  const n = Number(v);
  if (Number.isNaN(n)) return 100;
  return Math.max(0, Math.min(127, n));
}

function setTrackVolume(track, volume) {
  const idx = parseInt(track, 10);
  if (Number.isNaN(idx) || idx < 0 || idx >= 16) return;
  trackVolumes[idx] = clampTrackVolume(volume);
  renderCables();
}

function ingestTrackVolumes(volumes) {
  if (!Array.isArray(volumes)) return;
  for (let i = 0; i < 16; i++) {
    if (typeof volumes[i] !== 'undefined') {
      trackVolumes[i] = clampTrackVolume(volumes[i]);
    }
  }
  renderCables();
}

function ingestAudioLevelBuffer(buf) {
  if (!buf || buf.length < 18) return;
  if (buf[0] !== 0xAA) return;
  for (let i = 0; i < 16; i++) {
    trackPeaks[i] = Math.max(0, Math.min(1, (buf[i + 1] || 0) / 255));
  }
  renderCables();
}

/* ──────────── NODE CREATION ──────────── */
function createPadNode(track, x, y) {
  const id = 'pad-' + track;
  if (nodes.find(n => n.id === id)) return;
  const node = {
    id, type: 'pad', track,
    x: snap(x), y: snap(y),
    label: 'PAD ' + (track + 1),
    sub: TRACK_LABELS[track] || TRACK_NAMES[track],
    color: PAD_COLORS[track]
  };
  nodes.push(node);
  renderNode(node);
  return node;
}

function createFxNode(fxType, x, y) {
  const def = FX_DEFS[fxType];
  if (!def) return null;
  const id = 'fx-' + nextId++;
  const params = {};
  def.params.forEach(p => { params[p.id] = p.def; });
  const node = {
    id, type: 'fx', fxType,
    x: snap(x), y: snap(y),
    label: def.label,
    color: def.color,
    params,
    bypass: false
  };
  nodes.push(node);
  renderNode(node);
  updateStatus();
  saveState();
  return node;
}

function createMasterNode(x, y) {
  const id = 'master';
  if (nodes.find(n => n.id === id)) return;
  const node = {
    id, type: 'master',
    x: snap(x), y: snap(y),
    label: 'MASTER OUT',
    sub: '🔊 STEREO',
    color: 'gold'
  };
  nodes.push(node);
  renderNode(node);
}

function createBusNode(id, label, x, y, color) {
  if (nodes.find(n => n.id === id)) return;
  const node = {
    id,
    type: 'bus',
    x: snap(x),
    y: snap(y),
    label,
    sub: 'SUB MIX',
    color: color || 'teal'
  };
  nodes.push(node);
  renderNode(node);
}

/* ──────────── NODE RENDERING ──────────── */
function renderNode(node) {
  let el = document.getElementById('node-' + node.id);
  if (el) el.remove();

  el = document.createElement('div');
  el.id = 'node-' + node.id;
  el.className = 'pb-node';
  el.dataset.nodeId = node.id;
  el.style.left = node.x + 'px';
  el.style.top  = node.y + 'px';
  if (selectedNodeIds.has(node.id)) {
    el.classList.add('is-selected');
  }

  if (node.type === 'pad') {
    el.classList.add('pb-pad');
    el.style.borderColor = node.color + '66';
    el.innerHTML = `
      <div class="pb-node-header" style="color:${node.color}">${node.label}</div>
      <div class="pb-node-sub">${node.sub}</div>
      <div class="pb-connector out" data-node="${node.id}" data-dir="out" style="background:${node.color};border-color:${node.color};box-shadow:0 0 8px ${node.color}80"></div>
    `;
  } else if (node.type === 'fx') {
    const def = FX_DEFS[node.fxType];
    const slot = FX_SLOT_BY_TYPE[node.fxType];
    const scopeLabel = UNSUPPORTED_PRE_FX.has(node.fxType) ? 'MASTER · POST' : `TRACK · ${(slot || 'FX').toUpperCase()}`;
    el.classList.add('pb-fx');
    if (node.bypass) el.classList.add('is-bypassed');
    el.setAttribute('data-color', def.color);
    const paramText = def.params.map(p => {
      const v = node.params[p.id];
      const disp = p.type === 'select' ? p.options[v] : (p.unit ? v + p.unit : v);
      return p.label + ': ' + disp;
    }).join('  ·  ');
    el.innerHTML = `
      <button class="pb-node-delete" aria-label="Eliminar ${def.label}" data-node="${node.id}" onclick="event.stopPropagation();pbDeleteNode('${node.id}')">✕</button>
      <button class="pb-node-bypass ${node.bypass ? 'active' : ''}" aria-label="Bypass ${def.label}" data-node="${node.id}" onclick="event.stopPropagation();pbBypassNode('${node.id}')" title="Bypass FX">B</button>
      <button class="pb-node-edit" aria-label="Editar ${def.label}" data-node="${node.id}" onclick="event.stopPropagation();pbEditNode('${node.id}')">⚙</button>
      <div class="pb-node-header">${def.icon} ${node.label}${node.bypass ? ' <span style="color:#ff9100;font-size:10px">[BYP]</span>' : ''}</div>
      <div class="pb-node-scope">${scopeLabel}</div>
      <div class="pb-node-params">${paramText}</div>
      <div class="pb-connector in" data-node="${node.id}" data-dir="in"></div>
      <div class="pb-connector out" data-node="${node.id}" data-dir="out"></div>
    `;
  } else if (node.type === 'master') {
    el.classList.add('pb-master');
    el.setAttribute('data-color', 'gold');
    el.innerHTML = `
      <div class="pb-node-header">${node.label}</div>
      <div class="pb-node-sub">${node.sub}</div>
      <div class="pb-connector in" data-node="${node.id}" data-dir="in"></div>
    `;
  } else if (node.type === 'bus') {
    el.classList.add('pb-fx');
    el.setAttribute('data-color', node.color || 'teal');
    el.innerHTML = `
      <div class="pb-node-header">⎍ ${node.label}</div>
      <div class="pb-node-sub">${node.sub || 'SUB MIX'}</div>
      <div class="pb-connector in" data-node="${node.id}" data-dir="in"></div>
      <div class="pb-connector out" data-node="${node.id}" data-dir="out"></div>
    `;
  }

  nodesEl.appendChild(el);
  scheduleRouteReconcile();
}

function updateNodeDisplay(node) {
  if (node.type !== 'fx') return;
  const el = document.getElementById('node-' + node.id);
  if (!el) return;
  const def = FX_DEFS[node.fxType];
  const paramEl = el.querySelector('.pb-node-params');
  if (paramEl) {
    paramEl.textContent = def.params.map(p => {
      const v = node.params[p.id];
      const disp = p.type === 'select' ? p.options[v] : (p.unit ? Math.round(v) + p.unit : Math.round(v*100)/100);
      return p.label + ': ' + disp;
    }).join('  ·  ');
  }
}

/* ──────────── CABLE RENDERING ──────────── */
function getConnectorPos(nodeId, dir) {
  const el = document.querySelector(`#node-${CSS.escape(nodeId)} .pb-connector.${dir}`);
  if (!el) return null;
  const nodeEl = el.parentElement;
  const nx = parseFloat(nodeEl.style.left);
  const ny = parseFloat(nodeEl.style.top);
  const cr = 8;
  if (dir === 'out') {
    return { x: nx + nodeEl.offsetWidth + cr - 1, y: ny + nodeEl.offsetHeight / 2 };
  } else {
    return { x: nx - cr + 1, y: ny + nodeEl.offsetHeight / 2 };
  }
}

function getCableBezier(x1, y1, x2, y2) {
  const dx = Math.abs(x2 - x1);
  const cp = Math.max(80, dx * 0.45);
  return `M${x1},${y1} C${x1+cp},${y1} ${x2-cp},${y2} ${x2},${y2}`;
}

function renderCables() {
  /* Remove old cable paths */
  svgEl.querySelectorAll('path:not(.cable-preview)').forEach(p => p.remove());

  /* Remove cable info tooltip if no cable selected */
  if (!selectedCable) {
    const oldInfo = document.getElementById('pbCableInfo');
    if (oldInfo) oldInfo.remove();
  }

  cables.forEach((cable, index) => {
    const fromPos = getConnectorPos(cable.from, 'out');
    const toPos   = getConnectorPos(cable.to, 'in');
    if (!fromPos || !toPos) return;

    const d = getCableBezier(fromPos.x, fromPos.y, toPos.x, toPos.y);

    /* Invisible wider hitbox path for easier clicking (20px) */
    const hitPath = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    hitPath.setAttribute('d', d);
    hitPath.setAttribute('stroke', 'rgba(255,255,255,0.001)');
    hitPath.setAttribute('stroke-width', '20');
    hitPath.setAttribute('fill', 'none');
    hitPath.setAttribute('stroke-linecap', 'round');
    hitPath.dataset.cableId = cable.id;
    hitPath.classList.add('pb-cable-hit');
    hitPath.style.pointerEvents = 'visibleStroke';
    hitPath.style.cursor = 'pointer';
    hitPath.addEventListener('pointerdown', (e) => {
      e.stopPropagation();
      onCableClick(cable.id);
    });
    /* Double-click / double-tap to delete cable */
    hitPath.addEventListener('dblclick', (e) => {
      e.stopPropagation();
      e.preventDefault();
      removeCable(cable.id);
      if (selectedCable === cable.id) selectedCable = null;
    });
    svgEl.appendChild(hitPath);

    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    const baseWidth = getCableStrokeWidth(cable);
    const levelNorm = getCableLevelNorm(cable);
    const cableStroke = isHeatActive() ? getCableHeatColor(cable.color, levelNorm) : cable.color;
    path.setAttribute('d', d);
    path.setAttribute('stroke', cableStroke);
    path.setAttribute('stroke-width', String(baseWidth.toFixed(2)));
    path.setAttribute('fill', 'none');
    path.setAttribute('stroke-linecap', 'round');
    path.setAttribute('filter', 'url(#cableGlow)');
    path.dataset.cableId = cable.id;
    path.classList.add('pb-cable');
    path.style.setProperty('--flow-delay', `${(index % 8) * 0.09}s`);
    path.style.pointerEvents = 'none';
    path.style.cursor = 'pointer';

    /* Mute state: if ALL source tracks are muted, mark cable as muted */
    const sourceTracks = getCableSourceTracks(cable);
    const allMuted = sourceTracks.length > 0 && sourceTracks.every(t => trackMuted[t]);
    if (allMuted) {
      path.classList.add('is-muted');
    }

    if (selectedCable === cable.id) {
      path.classList.add('is-selected');
      path.setAttribute('stroke-width', String((baseWidth + 1.8).toFixed(2)));
      path.setAttribute('stroke-dasharray', '8 4');
      /* Show cable info tooltip */
      showCableInfo(cable, fromPos, toPos);
    }
    svgEl.appendChild(path);
  });
}

function showCableInfo(cable, fromPos, toPos) {
  let info = document.getElementById('pbCableInfo');
  if (!info) {
    info = document.createElement('div');
    info.id = 'pbCableInfo';
    info.style.cssText = 'position:absolute;padding:6px 12px;background:rgba(255,64,129,0.95);color:#fff;border-radius:8px;font-size:12px;font-weight:700;pointer-events:auto;z-index:200;white-space:nowrap;box-shadow:0 2px 12px rgba(0,0,0,0.6);display:flex;align-items:center;gap:8px;';
    worldEl.appendChild(info);
  }
  const fromNode = nodes.find(n => n.id === cable.from);
  const toNode = nodes.find(n => n.id === cable.to);
  const fromLabel = fromNode ? fromNode.label : cable.from;
  const toLabel = toNode ? toNode.label : cable.to;
  info.innerHTML = `<span>${fromLabel} → ${toLabel}</span><button style="padding:4px 12px;background:#ff1744;color:#fff;border:none;border-radius:5px;font-size:12px;font-weight:700;cursor:pointer;pointer-events:auto" onclick="event.stopPropagation();pbDeleteCable('${cable.id}')">✕ DELETE</button>`;
  info.style.left = ((fromPos.x + toPos.x) / 2 - 100) + 'px';
  info.style.top = ((fromPos.y + toPos.y) / 2 - 32) + 'px';
}

function getCableSourceTracks(cable) {
  const fromNode = nodes.find(n => n.id === cable.from);
  if (!fromNode) return [];
  if (fromNode.type === 'pad') return [fromNode.track];
  if (fromNode.type === 'fx') return getTracksForNode(fromNode.id);
  if (fromNode.type === 'bus') return getTracksForNode(fromNode.id);
  return [];
}

function getCableStrokeWidth(cable) {
  const avg = getCableAverageVolume(cable);
  if (avg < 0) return 2.4;
  return 1.6 + (avg / 127) * 4.4;
}

function getCableAverageVolume(cable) {
  const tracks = getCableSourceTracks(cable);
  if (!tracks.length) return -1;
  const sum = tracks.reduce((acc, track) => acc + clampTrackVolume(trackVolumes[track]), 0);
  return sum / tracks.length;
}

function getCableAveragePeak(cable) {
  const tracks = getCableSourceTracks(cable);
  if (!tracks.length) return -1;
  const sum = tracks.reduce((acc, track) => acc + (trackPeaks[track] || 0), 0);
  return sum / tracks.length;
}

function getCableLevelNorm(cable) {
  const avg = getCableAverageVolume(cable);
  const p = getCableAveragePeak(cable);
  if (avg < 0 && p < 0) return 0.5;
  const volNorm = avg < 0 ? 0.5 : Math.max(0, Math.min(1, avg / 127));
  const peakNorm = p < 0 ? 0.0 : Math.max(0, Math.min(1, p));
  return Math.max(0, Math.min(1, volNorm * 0.35 + peakNorm * 0.65));
}

function getCableHeatColor(baseHex, levelNorm) {
  const c = hexToRgb(baseHex);
  if (!c) return baseHex;

  const cool = { r: 90, g: 170, b: 255 };
  const hot = { r: 255, g: 62, b: 72 };

  const warmMix = Math.max(0, Math.min(1, levelNorm));
  const target = {
    r: Math.round(cool.r + (hot.r - cool.r) * warmMix),
    g: Math.round(cool.g + (hot.g - cool.g) * warmMix),
    b: Math.round(cool.b + (hot.b - cool.b) * warmMix)
  };

  const blendBase = 0.45;
  const out = {
    r: Math.round(c.r * (1 - blendBase) + target.r * blendBase),
    g: Math.round(c.g * (1 - blendBase) + target.g * blendBase),
    b: Math.round(c.b * (1 - blendBase) + target.b * blendBase)
  };
  return rgbToHex(out.r, out.g, out.b);
}

function hexToRgb(hex) {
  if (typeof hex !== 'string') return null;
  const clean = hex.trim().replace('#', '');
  if (!/^[0-9a-fA-F]{6}$/.test(clean)) return null;
  const num = parseInt(clean, 16);
  return {
    r: (num >> 16) & 255,
    g: (num >> 8) & 255,
    b: num & 255
  };
}

function rgbToHex(r, g, b) {
  const rr = Math.max(0, Math.min(255, r)) | 0;
  const gg = Math.max(0, Math.min(255, g)) | 0;
  const bb = Math.max(0, Math.min(255, b)) | 0;
  const v = (rr << 16) | (gg << 8) | bb;
  return '#' + v.toString(16).padStart(6, '0');
}

function renderPreviewCable(x1, y1, x2, y2, color) {
  let pv = svgEl.querySelector('.cable-preview');
  if (!pv) {
    pv = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    pv.classList.add('cable-preview');
    pv.setAttribute('fill', 'none');
    pv.setAttribute('stroke-width', '2.5');
    pv.setAttribute('stroke-linecap', 'round');
    svgEl.appendChild(pv);
  }
  pv.setAttribute('d', getCableBezier(x1, y1, x2, y2));
  pv.setAttribute('stroke', color);
}

function clearPreviewCable() {
  const pv = svgEl.querySelector('.cable-preview');
  if (pv) pv.remove();
}

/* ──────────── CABLE LOGIC ──────────── */
function addCable(fromId, toId) {
  if (!fromId || !toId) return null;
  if (fromId === toId) return null;

  const fromNode = nodes.find(n => n.id === fromId);
  const toNode = nodes.find(n => n.id === toId);
  if (!fromNode || !toNode) return null;

  if (fromNode.type === 'master') return null;
  if (toNode.type === 'pad') return null;

  /* Validate: no duplicate */
  if (cables.find(c => c.from === fromId && c.to === toId)) return null;
  if (wouldCreateCycle(fromId, toId)) {
    alert('Conexión no válida: crearía un bucle infinito en el routing');
    return null;
  }

  /* Get color from source node */
  let color = '#ffffff';
  if (fromNode && fromNode.type === 'pad') {
    color = fromNode.color;
  } else if (fromNode && fromNode.type === 'fx') {
    const def = FX_DEFS[fromNode.fxType];
    const cmap = {cyan:'#00e5ff',orange:'#ff9100',yellow:'#ffd600',purple:'#b388ff',pink:'#ff4081',green:'#69f0ae',teal:'#26c6da',violet:'#ea80fc',blue:'#448aff',gold:'#ffd700'};
    color = cmap[def.color] || '#ffffff';
  } else if (fromNode && fromNode.type === 'bus') {
    color = getColorHex(fromNode.color || 'teal');
  }

  const cable = { id: 'cable-' + nextId++, from: fromId, to: toId, color };
  cables.push(cable);
  renderCables();
  updateStatus();
  applyConnection(cable);
  saveState();
  return cable;
}

function removeCable(id) {
  const idx = cables.findIndex(c => c.id === id);
  if (idx < 0) return;
  const cable = cables[idx];
  clearConnection(cable);
  cables.splice(idx, 1);
  renderCables();
  updateStatus();
  saveState();
}

function onCableClick(id) {
  selectedCable = (selectedCable === id) ? null : id;
  renderCables();
}

/* ──────────── EFFECT APPLICATION ──────────── */
function getTracksForNode(nodeId) {
  /* Find all pads connected upstream to this node */
  const tracks = [];
  const visited = new Set();
  function walkUp(nid) {
    if (visited.has(nid)) return;
    visited.add(nid);
    cables.filter(c => c.to === nid).forEach(c => {
      const fromNode = nodes.find(n => n.id === c.from);
      if (fromNode && fromNode.type === 'pad') {
        if (!tracks.includes(fromNode.track)) tracks.push(fromNode.track);
      } else if (fromNode) {
        walkUp(fromNode.id);
      }
    });
  }
  walkUp(nodeId);
  return tracks;
}

function hasPath(startId, targetId, visited = new Set()) {
  if (startId === targetId) return true;
  if (visited.has(startId)) return false;
  visited.add(startId);
  const nextNodes = cables.filter(c => c.from === startId).map(c => c.to);
  for (const nid of nextNodes) {
    if (hasPath(nid, targetId, visited)) return true;
  }
  return false;
}

function wouldCreateCycle(fromId, toId) {
  return hasPath(toId, fromId);
}

function getReachableFxForTrack(track) {
  const source = nodes.find(n => n.type === 'pad' && n.track === track);
  if (!source) return [];

  const bestDepth = new Map([[source.id, 0]]);
  const queue = [{ id: source.id, depth: 0 }];
  const candidates = [];
  while (queue.length) {
    const current = queue.shift();
    if (!current || current.depth > nodes.length + 2) continue;
    cables.filter(c => c.from === current.id).forEach(cable => {
      const next = nodes.find(n => n.id === cable.to);
      if (!next) return;
      const depth = current.depth + 1;
      if ((bestDepth.get(next.id) ?? -1) >= depth) return;
      bestDepth.set(next.id, depth);
      queue.push({ id: next.id, depth });
      /* A node only affects audio when it belongs to a complete source →
         master path. Dangling editor nodes stay visibly unrouted. */
      if (next.type === 'fx' && hasPath(next.id, 'master')) {
        candidates.push({ node: next, depth });
      }
    });
  }
  return candidates;
}

function getTrackRouteWinners(track) {
  const candidates = getReachableFxForTrack(track)
    .filter(item => !item.node.bypass && FX_SLOT_BY_TYPE[item.node.fxType])
    .sort((a, b) => (a.depth - b.depth) || a.node.id.localeCompare(b.node.id));
  const winners = new Map();
  candidates.forEach(item => winners.set(FX_SLOT_BY_TYPE[item.node.fxType], item.node));
  return { candidates, winners };
}

function fxRouteSignature(node) {
  return node ? JSON.stringify([node.id, node.fxType, node.params || {}]) : '';
}

function clearFxSlot(track, slot) {
  switch (slot) {
    case 'filter':
      sendCmd('clearTrackFilter', { track });
      break;
    case 'distortion':
      sendCmd('setTrackDistortion', { track, amount: 0, mode: 0 });
      break;
    case 'bitcrusher':
      sendCmd('setTrackBitCrush', { track, value: 16 });
      break;
    case 'echo':
      sendCmd('setTrackEcho', { track, active: false, time: 100, feedback: 40, mix: 0 });
      break;
    case 'flanger':
      sendCmd('setTrackFlanger', { track, active: false, rate: 30, depth: 0, feedback: 0 });
      break;
    case 'compressor':
      sendCmd('setTrackCompressor', { track, active: false, threshold: -20, ratio: 1 });
      break;
  }
}

function updateRouteDiagnostics(routeData) {
  const winnerIds = new Set();
  const routedIds = new Set();
  const shadowedIds = new Set();
  let activeSlots = 0;

  routeData.forEach(({ candidates, winners }) => {
    const bySlot = new Map();
    candidates.forEach(item => {
      routedIds.add(item.node.id);
      const slot = FX_SLOT_BY_TYPE[item.node.fxType];
      if (!bySlot.has(slot)) bySlot.set(slot, []);
      bySlot.get(slot).push(item.node.id);
    });
    winners.forEach(node => { winnerIds.add(node.id); activeSlots++; });
    bySlot.forEach(ids => {
      if (ids.length > 1) ids.slice(0, -1).forEach(id => shadowedIds.add(id));
    });
  });

  nodes.filter(n => n.type === 'fx').forEach(node => {
    const el = document.getElementById('node-' + node.id);
    if (!el) return;
    const unsupported = UNSUPPORTED_PRE_FX.has(node.fxType);
    el.classList.toggle('is-route-active', winnerIds.has(node.id));
    el.classList.toggle('is-shadowed', shadowedIds.has(node.id) && !winnerIds.has(node.id));
    el.classList.toggle('is-unrouted', !routedIds.has(node.id) && !unsupported);
    el.classList.toggle('is-unsupported', unsupported);
    if (unsupported) el.title = 'Este efecto es global. Usa la pestaña POST.';
    else if (shadowedIds.has(node.id) && !winnerIds.has(node.id)) el.title = 'Otro nodo posterior ocupa el mismo slot de Daisy.';
    else if (!routedIds.has(node.id)) el.title = 'Conecta una fuente para activar este nodo.';
    else el.removeAttribute('title');
  });

  const status = document.getElementById('pbStatus');
  if (!status) return;
  const conflicts = [...shadowedIds].filter(id => !winnerIds.has(id)).length;
  const unsupported = nodes.filter(n => n.type === 'fx' && UNSUPPORTED_PRE_FX.has(n.fxType)).length;
  status.classList.toggle('is-warning', conflicts > 0 || unsupported > 0);
  status.textContent = conflicts || unsupported
    ? `RUTA · ${activeSlots} slots · ${conflicts} solapados · ${unsupported} globales`
    : `RUTA OK · ${activeSlots} slots activos`;
}

function reconcileAllTrackRoutes(force = false) {
  const routeData = Array.from({ length: 16 }, (_, track) => getTrackRouteWinners(track));
  updateRouteDiagnostics(routeData);
  if (!wsConnected) return;

  routeData.forEach(({ winners }, track) => {
    const applied = appliedRouteSlots[track];
    TRACK_FX_SLOTS.forEach(slot => {
      const nextNode = winners.get(slot) || null;
      const nextSignature = fxRouteSignature(nextNode);
      const previous = applied.get(slot);
      if (!force && previous?.signature === nextSignature) return;
      if (previous) clearFxSlot(track, slot);
      if (nextNode) {
        applyFxToTrack(track, nextNode);
        applied.set(slot, { nodeId: nextNode.id, signature: nextSignature });
      } else {
        applied.delete(slot);
      }
    });
  });
}

function scheduleRouteReconcile(force = false) {
  routeReconcileForce = routeReconcileForce || force;
  if (routeReconcileTimer) return;
  routeReconcileTimer = setTimeout(() => {
    routeReconcileTimer = null;
    const runForce = routeReconcileForce;
    routeReconcileForce = false;
    reconcileAllTrackRoutes(runForce);
  }, 24);
}

function applyConnection(cable, _visited) {
  if (!cable) return;
  scheduleRouteReconcile();
  updateSharedFxState();
}

function clearConnection(cable) {
  if (!cable) return;
  scheduleRouteReconcile();
  updateSharedFxState();
}

function applyFxToTrack(track, fxNode) {
  const p = fxNode.params;
  const ft = fxNode.fxType;
  console.log('[PATCH] applyFxToTrack: track', track, 'fx', ft, 'params', JSON.stringify(p));

  switch(ft) {
    case 'lowpass':
    case 'highpass':
    case 'bandpass':
    case 'notch':
    case 'allpass':
    case 'resonant':
      sendCmd('setTrackFilter', { track, filterType: FILTER_TYPE_MAP[ft], cutoff: p.cutoff, resonance: p.resonance || p['Q'] || 0.707 });
      break;
    case 'peaking':
    case 'lowshelf':
    case 'highshelf':
      sendCmd('setTrackFilter', { track, filterType: FILTER_TYPE_MAP[ft], cutoff: p.cutoff, resonance: p.resonance || 1, gain: p.gain || 0 });
      break;
    case 'bitcrusher':
      sendCmd('setTrackBitCrush', { track, value: Math.round(p.bits) });
      break;
    case 'distortion':
      sendCmd('setTrackDistortion', { track, amount: p.amount, mode: p.mode || 0 });
      break;
    case 'echo':
    case 'delay':
      sendCmd('setTrackEcho', { track, active: true, time: p.time, feedback: p.feedback, mix: p.mix });
      break;
    case 'flanger':
      sendCmd('setTrackFlanger', { track, active: true, rate: p.rate, depth: p.depth, feedback: p.feedback });
      break;
    case 'compressor': {
      /* UI threshold is 0-100%; firmware expects dB (-60 to 0) */
      const thresholdDb = -60 + (p.threshold / 100) * 60;
      sendCmd('setTrackCompressor', { track, active: true, threshold: thresholdDb, ratio: p.ratio });
      break;
    }
    case 'phaser':
      console.warn('[PATCH] Phaser es master-only; usa POST para aplicarlo.');
      break;
  }
}

function clearFxFromTrack(track, fxNode) {
  const ft = fxNode.fxType;
  switch(ft) {
    case 'lowpass':
    case 'highpass':
    case 'bandpass':
    case 'notch':
    case 'allpass':
    case 'peaking':
    case 'lowshelf':
    case 'highshelf':
    case 'resonant':
      sendCmd('clearTrackFilter', { track });
      break;
    case 'bitcrusher':
      sendCmd('setTrackBitCrush', { track, value: 16 });
      break;
    case 'distortion':
      sendCmd('setTrackDistortion', { track, amount: 0, mode: 0 });
      break;
    case 'echo':
    case 'delay':
      sendCmd('setTrackEcho', { track, active: false });
      break;
    case 'flanger':
      sendCmd('setTrackFlanger', { track, active: false });
      break;
    case 'compressor':
      sendCmd('setTrackCompressor', { track, active: false });
      break;
    case 'phaser':
      console.warn('[PATCH] Phaser PRE ignorado; el módulo global se controla en POST.');
      break;
  }
}

/* ──────────── SYNC: FIRMWARE → PATCHBAY ──────────── */
const FILTER_TYPE_REVERSE = {};
Object.entries(FILTER_TYPE_MAP).forEach(([k, v]) => { FILTER_TYPE_REVERSE[v] = k; });

function syncFxFromFirmware(track, fxCategory, params) {
  /* When firmware broadcasts FX changes (e.g. from sequencer), update
     any matching patchbay FX node connected to that track */
  if (fxCategory === 'filterClear') {
    /* Check if we have a filter node connected to this track */
    const filterTypes = Object.keys(FILTER_TYPE_MAP);
    nodes.filter(n => n.type === 'fx' && filterTypes.includes(n.fxType)).forEach(n => {
      const tracks = getTracksForNode(n.id);
      if (tracks.includes(track)) {
        console.log('[SYNC] Filter cleared on track', track, '→ node', n.label);
      }
    });
    updateSharedFxState();
    return;
  }

  if (fxCategory === 'filter') {
    const fxType = FILTER_TYPE_REVERSE[params.filterType];
    if (!fxType) return;
    /* Update matching filter node connected to this track */
    nodes.filter(n => n.type === 'fx' && n.fxType === fxType).forEach(n => {
      const tracks = getTracksForNode(n.id);
      if (tracks.includes(track)) {
        if (params.cutoff != null) n.params.cutoff = params.cutoff;
        if (params.resonance != null) n.params.resonance = params.resonance;
        updateNodeDisplay(n);
        console.log('[SYNC] Filter updated on track', track, '→', n.label, n.params);
      }
    });
    updateSharedFxState();
    return;
  }

  /* Live FX: echo, flanger, compressor */
  const fxMap = { echo: ['echo', 'delay'], flanger: ['flanger'], compressor: ['compressor'] };
  const matchTypes = fxMap[fxCategory];
  if (!matchTypes) return;

  nodes.filter(n => n.type === 'fx' && matchTypes.includes(n.fxType)).forEach(n => {
    const tracks = getTracksForNode(n.id);
    if (tracks.includes(track)) {
      if (fxCategory === 'echo') {
        if (params.time != null) n.params.time = params.time;
        if (params.feedback != null) n.params.feedback = params.feedback;
        if (params.mix != null) n.params.mix = params.mix;
      } else if (fxCategory === 'flanger') {
        if (params.rate != null) n.params.rate = params.rate;
        if (params.depth != null) n.params.depth = params.depth;
        if (params.feedback != null) n.params.feedback = params.feedback;
      } else if (fxCategory === 'compressor') {
        if (params.threshold != null) n.params.threshold = params.threshold;
        if (params.ratio != null) n.params.ratio = params.ratio;
      }
      updateNodeDisplay(n);
      console.log('[SYNC] FX', fxCategory, 'updated on track', track, '→', n.label);
    }
  });
  updateSharedFxState();
}

/* ──────────── SHARED FX STATE (localStorage sync between pages) ──────────── */
function updateSharedFxState() {
  /* Share the effective Daisy state, not every visible editor node.
     This keeps the sequencer consistent with complete source → master paths
     and with the last-node-wins rule for each hardware slot. */
  const state = {};
  for (let track = 0; track < 16; track++) {
    const { winners } = getTrackRouteWinners(track);
    winners.forEach((node, slot) => {
      if (!state[track]) state[track] = [];
      state[track].push({
        slot,
        fxType: node.fxType === 'echo' ? 'delay' : node.fxType,
        params: { ...node.params }
      });
    });
  }
  try {
    localStorage.setItem('r808_shared_fx', JSON.stringify(state));
  } catch(ex) {}
}

function saveFirmwareFiltersToShared(trackFilters) {
  /* Save firmware's track filter state for sequencer to read */
  try {
    localStorage.setItem('r808_firmware_filters', JSON.stringify(trackFilters));
  } catch(ex) {}
}

/* ──────────── PARAMETER EDITOR ──────────── */
function showParamEditor(nodeId, anchorX, anchorY) {
  const node = nodes.find(n => n.id === nodeId);
  if (!node || node.type !== 'fx') return;
  editingNode = nodeId;
  let _realtimeFxTimer = null; // shared debounce timer for all sliders in this editor

  const def = FX_DEFS[node.fxType];
  const editor = document.getElementById('pbParamEditor');
  const title = document.getElementById('pbParamTitle');
  const body  = document.getElementById('pbParamBody');

  title.textContent = def.label;
  title.style.color = getColorHex(def.color);
  body.innerHTML = '';

  def.params.forEach(p => {
    const row = document.createElement('div');
    row.className = 'pb-param-row';
    const val = node.params[p.id];

    if (p.type === 'select') {
      row.innerHTML = `
        <div class="pb-param-label"><span>${p.label}</span></div>
        <select class="pb-param-select" data-param="${p.id}">
          ${p.options.map((o,i) => `<option value="${i}" ${i===val?'selected':''}>${o}</option>`).join('')}
        </select>
      `;
      row.querySelector('select').addEventListener('change', (e) => {
        node.params[p.id] = parseInt(e.target.value);
        updateNodeDisplay(node);
        resendFxNode(node);
        saveState();
      });
    } else {
      const min = p.min || 0;
      const max = p.max || 100;
      const step = p.step || (max - min > 100 ? 1 : 0.1);
      const unit = p.unit || '';
      const dispVal = p.log ? Math.round(val) : (Number.isInteger(step) ? Math.round(val) : (Math.round(val*100)/100));
      row.innerHTML = `
        <div class="pb-param-label"><span>${p.label}</span><span class="pb-param-val" id="pv-${p.id}">${dispVal}${unit}</span></div>
        <input type="range" class="pb-param-range" data-param="${p.id}" min="${min}" max="${max}" step="${step}" value="${val}">
      `;
      const range = row.querySelector('input');
      range.addEventListener('input', (e) => {
        const v = parseFloat(e.target.value);
        node.params[p.id] = v;
        const dv = p.log ? Math.round(v) : (Number.isInteger(step) ? Math.round(v) : (Math.round(v*100)/100));
        document.getElementById('pv-' + p.id).textContent = dv + unit;
        updateNodeDisplay(node);
        /* Realtime audio update — throttled 35 ms to avoid WS flood */
        if (_realtimeFxTimer) clearTimeout(_realtimeFxTimer);
        _realtimeFxTimer = setTimeout(() => { _realtimeFxTimer = null; resendFxNode(node); }, 35);
      });
      range.addEventListener('change', () => {
        if (_realtimeFxTimer) { clearTimeout(_realtimeFxTimer); _realtimeFxTimer = null; }
        resendFxNode(node);
        saveState();
      });
    }
    body.appendChild(row);
  });

  /* Position editor near the node */
  editor.classList.remove('hidden');
  let ex = anchorX + 12;
  let ey = anchorY - 20;
  /* Keep on screen */
  const ew = editor.offsetWidth || 340;
  const eh = editor.offsetHeight || 260;
  if (ex + ew > window.innerWidth - 12) ex = anchorX - ew - 12;
  if (ey + eh > window.innerHeight - 12) ey = window.innerHeight - eh - 12;
  ex = Math.max(12, ex);
  ey = Math.max(12, ey);
  editor.style.left = ex + 'px';
  editor.style.top  = ey + 'px';
}

function closeParamEditor() {
  document.getElementById('pbParamEditor').classList.add('hidden');
  editingNode = null;
}

function resendFxNode(fxNode) {
  if (fxNode.bypass) return; /* Bypassed nodes don't apply FX */
  scheduleRouteReconcile();
}

function getColorHex(name) {
  const m = {cyan:'#00e5ff',orange:'#ff9100',yellow:'#ffd600',purple:'#b388ff',pink:'#ff4081',green:'#69f0ae',teal:'#26c6da',violet:'#ea80fc',blue:'#448aff',gold:'#ffd700',silver:'#aab0c0',slate:'#78909c',amber:'#ffab40',lime:'#c6ff00',lavender:'#ce93d8',hotpink:'#f06292'};
  return m[name] || '#fff';
}

/* ──────────── POINTER EVENTS ──────────── */
function getCanvasPos(e) {
  const r = canvasEl.getBoundingClientRect();
  return {
    x: (e.clientX - r.left + canvasEl.scrollLeft) / viewZoom,
    y: (e.clientY - r.top  + canvasEl.scrollTop) / viewZoom
  };
}

function onPointerDown(e) {
  const t = e.target;
  closeParamEditor();

  /* Deselect cable — walk up DOM manually for SVG compatibility */
  /* BUT skip if clicking inside the cable info tooltip (#pbCableInfo) */
  if (selectedCable) {
    const inCableInfo = t.closest && t.closest('#pbCableInfo');
    if (!inCableInfo) {
      let hasCableId = false;
      let el = t;
      while (el && el !== document.body) {
        if (el.dataset && el.dataset.cableId) { hasCableId = true; break; }
        el = el.parentElement || el.parentNode;
      }
      if (!hasCableId) {
        selectedCable = null;
        renderCables();
      }
    }
  }

  if (activeTool === 'hand') {
    if (t.closest('#pbViewControls')) return;
    e.preventDefault();
    drag = {
      type: 'pan',
      startClientX: e.clientX,
      startClientY: e.clientY,
      startScrollLeft: canvasEl.scrollLeft,
      startScrollTop: canvasEl.scrollTop
    };
    canvasEl.setPointerCapture(e.pointerId);
    return;
  }

  if (activeTool === 'select') {
    if (t.closest('#pbViewControls')) return;
    if (t.classList.contains('pb-connector') || t.classList.contains('pb-node-delete') || t.classList.contains('pb-node-edit') || t.classList.contains('pb-node-bypass')) return;

    const nodeEl = t.closest('.pb-node');
    const isNodeTarget = nodeEl && !t.classList.contains('pb-connector') && !t.classList.contains('pb-node-delete') && !t.classList.contains('pb-node-edit') && !t.classList.contains('pb-node-bypass');
    if (isNodeTarget) {
      e.preventDefault();
      const nodeId = nodeEl.dataset.nodeId;
      if (e.shiftKey) {
        if (selectedNodeIds.has(nodeId)) selectedNodeIds.delete(nodeId);
        else selectedNodeIds.add(nodeId);
      } else if (!selectedNodeIds.has(nodeId)) {
        selectedNodeIds.clear();
        selectedNodeIds.add(nodeId);
      }
      renderNodeSelection();

      const pos = getCanvasPos(e);
      const selectedNodes = Array.from(selectedNodeIds)
        .map(id => nodes.find(n => n.id === id))
        .filter(Boolean);
      selectedNodes.forEach(node => {
        const el = document.getElementById('node-' + node.id);
        if (el) el.classList.add('dragging');
      });
      drag = {
        type: 'multi-node',
        startX: pos.x,
        startY: pos.y,
        moved: false,
        origins: selectedNodes.map(node => ({ id: node.id, x: node.x, y: node.y }))
      };
      canvasEl.setPointerCapture(e.pointerId);
      return;
    }

    e.preventDefault();
    const pos = getCanvasPos(e);
    drag = {
      type: 'select-box',
      startX: pos.x,
      startY: pos.y,
      currentX: pos.x,
      currentY: pos.y,
      additive: !!e.shiftKey
    };
    if (!drag.additive) clearNodeSelection();
    showSelectionBoxWorldRect(pos.x, pos.y, pos.x, pos.y);
    canvasEl.setPointerCapture(e.pointerId);
    return;
  }

  /* Connector drag → cable creation */
  if (t.classList.contains('pb-connector')) {
    e.preventDefault();
    e.stopPropagation();
    const nodeId = t.dataset.node;
    const dir = t.dataset.dir;
    const pos = getCanvasPos(e);
    drag = { type: 'cable', fromId: nodeId, fromDir: dir, startX: pos.x, startY: pos.y };
    canvasEl.setPointerCapture(e.pointerId);
    return;
  }

  /* Node drag — exclude delete, bypass and edit buttons */
  const nodeEl = t.closest('.pb-node');
  if (nodeEl && !t.classList.contains('pb-node-delete') && !t.classList.contains('pb-node-bypass') && !t.classList.contains('pb-node-edit')) {
    e.preventDefault();
    const nodeId = nodeEl.dataset.nodeId;
    const pos = getCanvasPos(e);
    const nx = parseFloat(nodeEl.style.left);
    const ny = parseFloat(nodeEl.style.top);
    drag = { type: 'node', nodeId, offsetX: pos.x - nx, offsetY: pos.y - ny, moved: false };
    nodeEl.classList.add('dragging');
    canvasEl.setPointerCapture(e.pointerId);
  }
}

function onPointerMove(e) {
  if (!drag) return;
  e.preventDefault();

  if (drag.type === 'pan') {
    const dx = e.clientX - drag.startClientX;
    const dy = e.clientY - drag.startClientY;
    canvasEl.scrollLeft = drag.startScrollLeft - dx;
    canvasEl.scrollTop = drag.startScrollTop - dy;
    return;
  }

  if (drag.type === 'select-box') {
    const pos = getCanvasPos(e);
    drag.currentX = pos.x;
    drag.currentY = pos.y;
    showSelectionBoxWorldRect(drag.startX, drag.startY, pos.x, pos.y);
    selectNodesInWorldRect(drag.startX, drag.startY, pos.x, pos.y, drag.additive);
    return;
  }

  if (drag.type === 'multi-node') {
    const pos = getCanvasPos(e);
    const dx = pos.x - drag.startX;
    const dy = pos.y - drag.startY;
    drag.moved = true;

    drag.origins.forEach(origin => {
      const node = nodes.find(n => n.id === origin.id);
      if (!node) return;
      node.x = snap(Math.max(0, Math.min(CANVAS_W - 170, origin.x + dx)));
      node.y = snap(Math.max(0, Math.min(CANVAS_H - 80, origin.y + dy)));
      const el = document.getElementById('node-' + node.id);
      if (el) {
        el.style.left = node.x + 'px';
        el.style.top = node.y + 'px';
      }
    });
    renderCables();
    return;
  }

  const pos = getCanvasPos(e);

  if (drag.type === 'node') {
    drag.moved = true;
    const node = nodes.find(n => n.id === drag.nodeId);
    if (!node) return;
    node.x = snap(Math.max(0, Math.min(CANVAS_W - 170, pos.x - drag.offsetX)));
    node.y = snap(Math.max(0, Math.min(CANVAS_H - 80,  pos.y - drag.offsetY)));
    const el = document.getElementById('node-' + node.id);
    if (el) {
      el.style.left = node.x + 'px';
      el.style.top  = node.y + 'px';
    }
    renderCables();
  }

  else if (drag.type === 'cable') {
    const fromNode = nodes.find(n => n.id === drag.fromId);
    let color = '#888';
    if (fromNode) {
      if (fromNode.type === 'pad') color = fromNode.color;
      else if (fromNode.type === 'fx') color = getColorHex(FX_DEFS[fromNode.fxType]?.color);
      else if (fromNode.type === 'bus') color = getColorHex(fromNode.color || 'teal');
      else if (fromNode.type === 'master') color = '#ffd700';
    }

    if (drag.fromDir === 'out') {
      const fp = getConnectorPos(drag.fromId, 'out');
      if (fp) renderPreviewCable(fp.x, fp.y, pos.x, pos.y, color);
    } else {
      const fp = getConnectorPos(drag.fromId, 'in');
      if (fp) renderPreviewCable(pos.x, pos.y, fp.x, fp.y, color);
    }

    /* Highlight valid targets */
    highlightTargets(drag.fromId, drag.fromDir, pos);
  }
}

function onPointerUp(e) {
  if (!drag) return;

  if (drag.type === 'node') {
    const el = document.getElementById('node-' + drag.nodeId);
    if (el) el.classList.remove('dragging');

    /* If not moved, treat as double-click → param editor */
    if (!drag.moved) {
      const node = nodes.find(n => n.id === drag.nodeId);
      if (node && node.type === 'fx') {
        showParamEditor(node.id, e.clientX, e.clientY);
      }
    } else {
      saveState();
    }
  }

  else if (drag.type === 'multi-node') {
    drag.origins.forEach(origin => {
      const el = document.getElementById('node-' + origin.id);
      if (el) el.classList.remove('dragging');
    });
    if (drag.moved) saveState();
  }

  else if (drag.type === 'select-box') {
    hideSelectionBox();
  }

  else if (drag.type === 'pan') {
    /* no-op */
  }

  else if (drag.type === 'cable') {
    clearPreviewCable();
    clearHighlights();

    /* Find target connector under pointer */
    const pos = getCanvasPos(e);
    const target = findConnectorAt(pos.x, pos.y, drag.fromId, drag.fromDir);
    if (target) {
      if (drag.fromDir === 'out') {
        addCable(drag.fromId, target);
      } else {
        addCable(target, drag.fromId);
      }
    }
  }

  drag = null;
  try { canvasEl.releasePointerCapture(e.pointerId); } catch(ex){}
}

function findConnectorAt(x, y, excludeId, fromDir) {
  const targetDir = fromDir === 'out' ? 'in' : 'out';
  const connectors = document.querySelectorAll(`.pb-connector.${targetDir}`);
  let best = null, bestDist = 40; // snap radius

  connectors.forEach(c => {
    const nodeId = c.dataset.node;
    if (nodeId === excludeId) return;
    const cp = getConnectorPos(nodeId, targetDir);
    if (!cp) return;
    const d = Math.hypot(cp.x - x, cp.y - y);
    if (d < bestDist) { bestDist = d; best = nodeId; }
  });
  return best;
}

function highlightTargets(fromId, fromDir, pos) {
  const targetDir = fromDir === 'out' ? 'in' : 'out';
  document.querySelectorAll('.pb-connector').forEach(c => c.classList.remove('active'));
  document.querySelectorAll(`.pb-connector.${targetDir}`).forEach(c => {
    if (c.dataset.node !== fromId) {
      const cp = getConnectorPos(c.dataset.node, targetDir);
      if (cp && Math.hypot(cp.x - pos.x, cp.y - pos.y) < 60) {
        c.classList.add('active');
      }
    }
  });
}

function clearHighlights() {
  document.querySelectorAll('.pb-connector.active').forEach(c => c.classList.remove('active'));
}

/* ──────────── KEYBOARD ──────────── */
function onKeyDown(e) {
  const tag = (e.target?.tagName || '').toUpperCase();

  /* Spacebar → toggle play/pause */
  if (e.key === ' ' && tag !== 'INPUT' && tag !== 'TEXTAREA' && tag !== 'SELECT') {
    e.preventDefault();
    sendCmd(isPlaying ? 'stop' : 'start', {});
    return;
  }
  const isFormField = tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT';
  if (isFormField) return;

  if (e.key === 'Delete' || e.key === 'Backspace') {
    if (selectedCable) {
      e.preventDefault();
      removeCable(selectedCable);
      selectedCable = null;
      renderCables();
    } else if (selectedNodeIds.size) {
      e.preventDefault();
      [...selectedNodeIds].forEach(nodeId => window.pbDeleteNode(nodeId));
      clearNodeSelection();
    }
  }
  if (e.key === 'Escape') {
    closeParamEditor();
    selectedCable = null;
    clearNodeSelection();
    hideSelectionBox();
    renderCables();
  }
}

function onDocClick(e) {
  if (!e.target.closest('#pbMenu') && !e.target.closest('#pbMenuBtn')) {
    setPBMenuOpen(false);
  }
  if (!e.target.closest('#pbPresetPanel') && !e.target.closest('#pbMenu')) {
    const panel = document.getElementById('pbPresetPanel');
    if (panel && !panel.classList.contains('hidden') && !e.target.closest('[onclick="pbOpenPresetPanel()"]')) {
      panel.classList.add('hidden');
    }
  }
}

/* ──────────── PUBLIC API (window) ──────────── */
window.goBack = function() {
  window.location.href = '/';
};

window.togglePBMenu = function() {
  const menu = document.getElementById('pbMenu');
  if (!menu) return;
  const willOpen = menu.classList.contains('hidden');
  if (willOpen) {
    const preset = document.getElementById('pbPresetPanel');
    if (preset) preset.classList.add('hidden');
  }
  setPBMenuOpen(willOpen);
};

window.pbAddEffect = function(fxType) {
  setPBMenuOpen(false);
  if (fxType === 'echo') fxType = 'delay';
  if (UNSUPPORTED_PRE_FX.has(fxType)) {
    window.pbSwitchTab('post');
    const status = document.getElementById('pbStatus');
    if (status) status.textContent = 'Phaser es global · configúralo en POST';
    return;
  }
  /* Place new node at center of visible viewport */
  const r = canvasEl.getBoundingClientRect();
  const sameTypeCount = nodes.filter(n => n.type === 'fx' && n.fxType === fxType).length;
  const offset = (sameTypeCount % 4) * 36;
  const cx = (canvasEl.scrollLeft + r.width / 2) / viewZoom - 160 + offset;
  const cy = (canvasEl.scrollTop + r.height / 2) / viewZoom - 70 + offset;
  const node = createFxNode(fxType, cx, cy);
  if (node) {
    /* Scroll to make node visible */
    const el = document.getElementById('node-' + node.id);
    if (el) el.scrollIntoView({ behavior: 'smooth', block: 'center', inline: 'center' });
  }
};

window.pbAddSelectedSource = function() {
  const picker = document.getElementById('pbSourcePicker');
  if (!picker || picker.disabled) return;
  const track = parseInt(picker.value, 10);
  if (!Number.isInteger(track)) return;
  addPadSource(track);
};

window.pbRemoveSelectedSource = function() {
  const picker = document.getElementById('pbActiveSourcePicker');
  if (!picker || picker.disabled) return;
  const track = parseInt(picker.value, 10);
  if (!Number.isInteger(track)) return;
  removePadSource(track);
};

window.pbAddAllSources = function() {
  const missing = getMissingPadTracks();
  if (!missing.length) return;
  missing.forEach(track => {
    if (!nodes.find(n => n.type === 'pad' && n.track === track)) {
      createPadNode(track, 60, getNextPadSlotY());
    }
  });
  updateStatus();
  saveState();
  refreshSourcePicker();
};

window.pbZoomIn = function() {
  setZoom(viewZoom + 0.12);
};

window.pbToggleHandTool = function() {
  setActiveTool('hand');
};

window.pbToggleSelectTool = function() {
  setActiveTool('select');
};

window.pbZoomOut = function() {
  setZoom(viewZoom - 0.12);
};

window.pbZoomReset = function() {
  setZoom(1);
};

window.pbToggleGrid = function() {
  gridVisible = !gridVisible;
  applyGridVisibility();
  saveViewPrefs();
};

window.pbToggleHeatMode = function() {
  if (heatMode === 'off') heatMode = 'on';
  else if (heatMode === 'on') heatMode = 'auto';
  else heatMode = 'off';
  updateHeatModeButton();
  renderCables();
  saveViewPrefs();
};

window.pbToggleSceneQuantize = function() {
  sceneQuantizeEnabled = !sceneQuantizeEnabled;
  if (!sceneQuantizeEnabled && pendingSceneState) {
    applyQueuedSceneState();
  }
  updateQuantizeButton();
  saveViewPrefs();
};

window.pbSaveSnapshot = function(slot) {
  const key = String(slot || 'A').toUpperCase() === 'B' ? 'B' : 'A';
  localStorage.setItem(`pb_snapshot_${key}`, JSON.stringify(exportState()));
};

window.pbLoadSnapshot = function(slot) {
  const key = String(slot || 'A').toUpperCase() === 'B' ? 'B' : 'A';
  const raw = localStorage.getItem(`pb_snapshot_${key}`);
  if (!raw) return;
  try {
    const state = JSON.parse(raw);
    scheduleSceneStateApply(state, `Snapshot ${key}`);
  } catch(ex) {}
};

window.pbDeleteNode = function(nodeId) {
  const node = nodes.find(n => n.id === nodeId);
  if (!node || node.type === 'pad' || node.type === 'master' || node.type === 'bus') return;

  /* Remove all cables connected to this node */
  const connectedCables = cables.filter(c => c.from === nodeId || c.to === nodeId);
  connectedCables.forEach(c => {
    clearConnection(c);
  });
  cables = cables.filter(c => c.from !== nodeId && c.to !== nodeId);

  /* Remove node */
  nodes = nodes.filter(n => n.id !== nodeId);
  selectedNodeIds.delete(nodeId);
  const el = document.getElementById('node-' + nodeId);
  if (el) el.remove();

  renderNodeSelection();
  renderCables();
  updateStatus();
  saveState();
};

window.pbClearAll = function() {
  setPBMenuOpen(false);
  /* Clear all cables */
  cables.forEach(c => clearConnection(c));
  cables = [];
  /* Remove PAD + FX nodes, keep master/buses */
  nodes.filter(n => n.type === 'fx' || n.type === 'pad').forEach(n => {
    const el = document.getElementById('node-' + n.id);
    if (el) el.remove();
  });
  nodes = nodes.filter(n => n.type !== 'fx' && n.type !== 'pad');
  clearNodeSelection();
  renderCables();
  updateStatus();
  refreshSourcePicker();
  saveState();
  /* Reset firmware FX state to match the now-empty canvas */
  resetFirmwareFX();
  appliedRouteSlots.forEach(slots => slots.clear());
  scheduleRouteReconcile();
};

window.pbResetLayout = function() {
  setPBMenuOpen(false);
  /* Reset pad positions */
  nodes.filter(n => n.type === 'pad').forEach((n, i) => {
    n.x = 60; n.y = 60 + i * 120;
    const el = document.getElementById('node-' + n.id);
    if (el) { el.style.left = n.x + 'px'; el.style.top = n.y + 'px'; }
  });
  /* Reset master position */
  const master = nodes.find(n => n.type === 'master');
  if (master) {
    master.x = CANVAS_W - 260; master.y = 500;
    const el = document.getElementById('node-' + master.id);
    if (el) { el.style.left = master.x + 'px'; el.style.top = master.y + 'px'; }
  }
  const busA = nodes.find(n => n.id === 'bus-a');
  if (busA) {
    busA.x = CANVAS_W - 560; busA.y = 380;
    const el = document.getElementById('node-' + busA.id);
    if (el) { el.style.left = busA.x + 'px'; el.style.top = busA.y + 'px'; }
  }
  const busB = nodes.find(n => n.id === 'bus-b');
  if (busB) {
    busB.x = CANVAS_W - 560; busB.y = 640;
    const el = document.getElementById('node-' + busB.id);
    if (el) { el.style.left = busB.x + 'px'; el.style.top = busB.y + 'px'; }
  }
  renderCables();
  saveState();
};

window.pbAutoRoute = function() {
  setPBMenuOpen(false);
  /* Auto: each pad → master out */
  nodes.filter(n => n.type === 'pad').forEach(pad => {
    if (!cables.find(c => c.from === pad.id && c.to === 'master')) {
      addCable(pad.id, 'master');
    }
  });
};

window.pbBuildChain = function() {
  setPBMenuOpen(false);
  const fxNodes = nodes
    .filter(n => n.type === 'fx' && !UNSUPPORTED_PRE_FX.has(n.fxType))
    .sort((a, b) => (a.x - b.x) || (a.y - b.y));
  if (fxNodes.length === 0) return;

  cables.forEach(c => clearConnection(c));
  cables = [];

  const firstFx = fxNodes[0];
  nodes.filter(n => n.type === 'pad').forEach(pad => addCable(pad.id, firstFx.id));
  for (let i = 0; i < fxNodes.length - 1; i++) {
    addCable(fxNodes[i].id, fxNodes[i + 1].id);
  }
  addCable(fxNodes[fxNodes.length - 1].id, 'master');

  renderCables();
  updateStatus();
  saveState();
};

window.pbOrganizeNodes = function() {
  setPBMenuOpen(false);
  const fxNodes = nodes.filter(n => n.type === 'fx').sort((a, b) => (a.x - b.x) || (a.y - b.y));
  fxNodes.forEach((node, idx) => {
    const col = idx % 4;
    const row = Math.floor(idx / 4);
    node.x = snap(760 + col * 280);
    node.y = snap(280 + row * 220);
    const el = document.getElementById('node-' + node.id);
    if (el) {
      el.style.left = node.x + 'px';
      el.style.top = node.y + 'px';
    }
  });

  const master = nodes.find(n => n.type === 'master');
  if (master) {
    master.x = snap(CANVAS_W - 260);
    master.y = snap(520);
    const el = document.getElementById('node-' + master.id);
    if (el) {
      el.style.left = master.x + 'px';
      el.style.top = master.y + 'px';
    }
  }

  renderCables();
  saveState();
};

window.pbToggleSignalDemo = function() {
  setPBMenuOpen(false);
  signalDemoMode = !signalDemoMode;
  updatePlayingVisualState();
};

window.pbSavePreset = function() {
  window.pbOpenPresetPanel();
  const input = document.getElementById('pbPresetNameInput');
  if (input) {
    input.focus();
    input.select();
  }
};

window.pbFactoryPresets = function() {
  window.pbOpenPresetPanel();
};

window.pbLoadPreset = function() {
  window.pbOpenPresetPanel();
};

window.pbOpenPresetPanel = function() {
  setPBMenuOpen(false);
  const panel = document.getElementById('pbPresetPanel');
  if (!panel) return;
  renderPresetPanel();
  panel.classList.remove('hidden');
};

window.pbClosePresetPanel = function() {
  const panel = document.getElementById('pbPresetPanel');
  if (panel) panel.classList.add('hidden');
};

window.pbSavePresetFromPanel = function() {
  const input = document.getElementById('pbPresetNameInput');
  const baseName = input?.value?.trim();
  const name = baseName || ('Patchbay ' + new Date().toLocaleTimeString());
  const presets = getUserPresets();
  presets[name] = exportState();
  setUserPresets(presets);
  if (input) input.value = '';
  renderPresetPanel();
};

window.pbEditNode = function(nodeId) {
  const node = nodes.find(n => n.id === nodeId);
  if (!node || node.type !== 'fx') return;
  const el = document.getElementById('node-' + nodeId);
  if (!el) return;
  const rect = el.getBoundingClientRect();
  showParamEditor(nodeId, rect.right - 10, rect.top + 8);
};

window.pbBypassNode = function(nodeId) {
  const node = nodes.find(n => n.id === nodeId);
  if (!node || node.type !== 'fx') return;
  node.bypass = !node.bypass;
  console.log('[PATCH] Bypass', node.label, ':', node.bypass ? 'ON' : 'OFF');

  /* Re-render the full node to update [BYP] tag and classes */
  renderNode(node);

  scheduleRouteReconcile();

  saveState();
};

window.closeParamEditor = closeParamEditor;

window.pbDeleteCable = function(cableId) {
  removeCable(cableId);
  if (selectedCable === cableId) selectedCable = null;
};

/* ──────────── PERSISTENCE ──────────── */
function exportState() {
  return {
    nodes: nodes.filter(n => n.type === 'fx').map(n => ({
      id: n.id, fxType: n.fxType, x: n.x, y: n.y, params: {...n.params}, bypass: !!n.bypass
    })),
    padPositions: nodes.filter(n => n.type === 'pad').map(n => ({ id: n.id, x: n.x, y: n.y })),
    busPositions: nodes.filter(n => n.type === 'bus').map(n => ({ id: n.id, x: n.x, y: n.y })),
    masterPos: (() => { const m = nodes.find(n => n.type === 'master'); return m ? {x: m.x, y: m.y} : null; })(),
    cables: cables.map(c => ({ from: c.from, to: c.to })),
    nextId
  };
}

function importState(data) {
  clearNodeSelection();

  /* Clear everything */
  cables.forEach(c => clearConnection(c));
  cables = [];
  nodes.filter(n => n.type === 'fx').forEach(n => {
    const el = document.getElementById('node-' + n.id);
    if (el) el.remove();
  });
  nodes = nodes.filter(n => n.type !== 'fx');

  ensurePadNodesForState(data);

  /* Restore pad positions */
  if (data.padPositions) {
    data.padPositions.forEach(pp => {
      const node = nodes.find(n => n.id === pp.id);
      if (node) {
        node.x = pp.x; node.y = pp.y;
        const el = document.getElementById('node-' + node.id);
        if (el) { el.style.left = pp.x + 'px'; el.style.top = pp.y + 'px'; }
      }
    });
  }

  if (data.busPositions) {
    data.busPositions.forEach(bp => {
      const node = nodes.find(n => n.id === bp.id && n.type === 'bus');
      if (node) {
        node.x = bp.x;
        node.y = bp.y;
        const el = document.getElementById('node-' + node.id);
        if (el) { el.style.left = bp.x + 'px'; el.style.top = bp.y + 'px'; }
      }
    });
  }

  /* Restore master position */
  if (data.masterPos) {
    const m = nodes.find(n => n.type === 'master');
    if (m) {
      m.x = data.masterPos.x; m.y = data.masterPos.y;
      const el = document.getElementById('node-' + m.id);
      if (el) { el.style.left = m.x + 'px'; el.style.top = m.y + 'px'; }
    }
  }

  /* Recreate FX nodes */
  if (data.nodes) {
    data.nodes.forEach(nd => {
      const node = createFxNode(nd.fxType, nd.x, nd.y);
      if (node && nd.params) {
        Object.assign(node.params, nd.params);
        updateNodeDisplay(node);
      }
      /* Restore bypass state */
      if (node && nd.bypass) {
        node.bypass = true;
        const byEl = document.getElementById('node-' + node.id);
        if (byEl) {
          byEl.classList.add('is-bypassed');
          const byBtn = byEl.querySelector('.pb-node-bypass');
          if (byBtn) byBtn.classList.add('active');
        }
      }
      /* Remap ID */
      if (node) {
        const oldId = node.id;
        node.id = nd.id;
        const el = document.getElementById('node-' + oldId);
        if (el) { el.id = 'node-' + nd.id; el.dataset.nodeId = nd.id; }
        /* Update connector data-node attrs */
        el?.querySelectorAll('.pb-connector').forEach(c => c.dataset.node = nd.id);
        el?.querySelector('.pb-node-delete')?.setAttribute('onclick', `event.stopPropagation();pbDeleteNode('${nd.id}')`);
        el?.querySelector('.pb-node-delete')?.setAttribute('data-node', nd.id);
        el?.querySelector('.pb-node-bypass')?.setAttribute('onclick', `event.stopPropagation();pbBypassNode('${nd.id}')`);
        el?.querySelector('.pb-node-bypass')?.setAttribute('data-node', nd.id);
        el?.querySelector('.pb-node-edit')?.setAttribute('onclick', `event.stopPropagation();pbEditNode('${nd.id}')`);
        el?.querySelector('.pb-node-edit')?.setAttribute('data-node', nd.id);
      }
    });
  }

  nextId = data.nextId || nextId;

  /* Recreate cables */
  if (data.cables) {
    data.cables.forEach(cd => addCable(cd.from, cd.to));
  }

  renderCables();
  updateStatus();
  refreshSourcePicker();
}

function saveState() {
  try {
    localStorage.setItem('pb_state', JSON.stringify(exportState()));
  } catch(ex) {}
}

function loadState() {
  try {
    const raw = localStorage.getItem('pb_state');
    if (raw) {
      const data = JSON.parse(raw);
      importState(data);
    }
  } catch(ex) {
    console.warn('Patchbay: could not load state', ex);
  }

  /* After loading patchbay state, sync sequencer FX if patchbay is empty */
  syncFromSequencerFx();
}

function syncFromSequencerFx() {
  /* If patchbay has no FX nodes, check if sequencer saved FX and create nodes+cables */
  const hasFxNodes = nodes.some(n => n.type === 'fx');
  if (hasFxNodes) return; /* Patchbay already has its own state */

  try {
    const raw = localStorage.getItem('r808_seq_fx');
    if (!raw) return;
    const state = JSON.parse(raw);
    if (!Object.keys(state).length) return;

    console.log('[SYNC] Creating patchbay nodes from sequencer FX state...');

    Object.entries(state).forEach(([trackStr, fxList]) => {
      const track = parseInt(trackStr, 10);
      if (isNaN(track) || track < 0 || track >= 16) return;
      const padNode = nodes.find(n => n.type === 'pad' && n.track === track);
      if (!padNode) return;

      fxList.forEach((fx, idx) => {
        const fxType = fx.fxType;
        if (!FX_DEFS[fxType]) return;
        const xOffset = 400 + idx * 360;
        const node = createFxNode(fxType, padNode.x + xOffset, padNode.y);
        if (node && fx.params) {
          Object.assign(node.params, fx.params);
          updateNodeDisplay(node);
        }
        if (node) addCable(padNode.id, node.id);
      });
    });

    saveState();
  } catch(ex) {
    console.warn('[SYNC] Could not sync from sequencer FX:', ex);
  }
}

function setZoom(nextZoom) {
  const clamped = Math.max(0.5, Math.min(2.5, nextZoom));
  if (Math.abs(clamped - viewZoom) < 0.001) return;

  const oldZoom = viewZoom;
  const rect = canvasEl.getBoundingClientRect();
  const centerX = canvasEl.scrollLeft + rect.width / 2;
  const centerY = canvasEl.scrollTop + rect.height / 2;
  const worldX = centerX / oldZoom;
  const worldY = centerY / oldZoom;

  viewZoom = clamped;
  applyZoom();

  canvasEl.scrollLeft = worldX * viewZoom - rect.width / 2;
  canvasEl.scrollTop  = worldY * viewZoom - rect.height / 2;

  saveViewPrefs();
}

function applyZoom() {
  if (!worldEl || !svgEl || !nodesEl || !canvasEl) return;
  worldEl.style.width = (CANVAS_W * viewZoom) + 'px';
  worldEl.style.height = (CANVAS_H * viewZoom) + 'px';
  svgEl.style.transform = `scale(${viewZoom})`;
  nodesEl.style.transform = `scale(${viewZoom})`;
  canvasEl.style.setProperty('--pb-grid-zoom', String(viewZoom));

  const zoomLabel = document.getElementById('pbZoomValue');
  if (zoomLabel) zoomLabel.textContent = `${Math.round(viewZoom * 100)}%`;

  renderCables();
}

function applyGridVisibility() {
  if (!canvasEl) return;
  canvasEl.classList.toggle('pb-grid-off', !gridVisible);
  const btn = document.getElementById('pbGridToggleBtn');
  if (btn) {
    btn.classList.toggle('is-off', !gridVisible);
    btn.textContent = gridVisible ? 'GRID' : 'NO GRID';
  }
}

function updateHeatModeButton() {
  const btn = document.getElementById('pbHeatToggleBtn');
  if (!btn) return;
  const labels = {
    off: '🌡 Heat OFF',
    on: '🌡 Heat ON',
    auto: '🌡 Heat AUTO'
  };
  btn.textContent = labels[heatMode] || '🌡 Heat ON';
}

function updateQuantizeButton() {
  const btn = document.getElementById('pbQuantizeBtn');
  if (!btn) return;
  if (!sceneQuantizeEnabled) {
    btn.textContent = '⏱ Quantize OFF';
    return;
  }
  btn.textContent = pendingSceneState
    ? `⏱ Quantize ON • ${pendingSceneLabel || 'Queued'}`
    : '⏱ Quantize ON';
}

function cloneSceneState(state) {
  try {
    return JSON.parse(JSON.stringify(state));
  } catch(ex) {
    return null;
  }
}

function applySceneStateNow(state) {
  const cloned = cloneSceneState(state);
  if (!cloned) return;
  importState(cloned);
  saveState();
}

function applyQueuedSceneState() {
  if (!pendingSceneState) return;
  const queued = pendingSceneState;
  pendingSceneState = null;
  pendingSceneLabel = '';
  applySceneStateNow(queued);
  updateQuantizeButton();
}

function scheduleSceneStateApply(state, label = '') {
  const cloned = cloneSceneState(state);
  if (!cloned) return;

  const running = !!(serverPlaying || stepDrivenPlaying || isPlaying);
  if (!sceneQuantizeEnabled || !running) {
    pendingSceneState = null;
    pendingSceneLabel = '';
    applySceneStateNow(cloned);
    updateQuantizeButton();
    return;
  }

  pendingSceneState = cloned;
  pendingSceneLabel = label || 'Queued';
  updateQuantizeButton();
}

function isHeatActive() {
  if (heatMode === 'on') return true;
  if (heatMode === 'off') return false;
  const isRunning = !!(signalDemoMode || serverPlaying || stepDrivenPlaying || isPlaying);
  return isRunning;
}

function loadViewPrefs() {
  try {
    const raw = localStorage.getItem('pb_view');
    if (!raw) return;
    const cfg = JSON.parse(raw);
    if (typeof cfg.zoom === 'number') viewZoom = Math.max(0.5, Math.min(2.5, cfg.zoom));
    if (typeof cfg.gridVisible === 'boolean') gridVisible = cfg.gridVisible;
    if (typeof cfg.heatMode === 'string' && ['off', 'on', 'auto'].includes(cfg.heatMode)) {
      heatMode = cfg.heatMode;
    } else if (typeof cfg.heatModeEnabled === 'boolean') {
      heatMode = cfg.heatModeEnabled ? 'on' : 'off';
    }
    if (typeof cfg.sceneQuantizeEnabled === 'boolean') sceneQuantizeEnabled = cfg.sceneQuantizeEnabled;
  } catch(ex) {}
}

function saveViewPrefs() {
  try {
    localStorage.setItem('pb_view', JSON.stringify({ zoom: viewZoom, gridVisible, heatMode, sceneQuantizeEnabled }));
  } catch(ex) {}
}

function createFactoryPresetState(preset) {
  if (!preset || !Array.isArray(preset.chain) || preset.chain.length === 0) return null;

  const current = exportState();
  let idCounter = nextId;
  const keyToId = {};

  /* Factory presets predate the final Daisy slot contract. Remove master-only
     Phaser nodes and keep only the last node for each per-track hardware slot. */
  const migrated = preset.chain
    .filter(def => !UNSUPPORTED_PRE_FX.has(def.fxType))
    .map(def => ({ ...def, fxType: def.fxType === 'echo' ? 'delay' : def.fxType }));
  const lastSlotIndex = new Map();
  migrated.forEach((def, index) => lastSlotIndex.set(FX_SLOT_BY_TYPE[def.fxType] || def.key, index));
  const effectiveChain = migrated.filter((def, index) =>
    lastSlotIndex.get(FX_SLOT_BY_TYPE[def.fxType] || def.key) === index
  );
  if (!effectiveChain.length) return null;

  const fxNodes = effectiveChain.map(def => {
    const id = `fx-${idCounter++}`;
    keyToId[def.key] = id;
    return {
      id,
      fxType: def.fxType,
      x: def.x,
      y: def.y,
      params: {...(def.params || {})}
    };
  });

  const chainCables = [];
  const firstFxId = keyToId[effectiveChain[0].key];
  const lastFxId = keyToId[effectiveChain[effectiveChain.length - 1].key];

  if (firstFxId) {
    current.padPositions.forEach(pad => {
      chainCables.push({ from: pad.id, to: firstFxId });
    });
  }

  for (let i = 0; i < effectiveChain.length - 1; i++) {
    const fromId = keyToId[effectiveChain[i].key];
    const toId = keyToId[effectiveChain[i + 1].key];
    if (fromId && toId) chainCables.push({ from: fromId, to: toId });
  }

  if (lastFxId) chainCables.push({ from: lastFxId, to: 'master' });

  return {
    nodes: fxNodes,
    padPositions: current.padPositions,
    busPositions: current.busPositions,
    masterPos: current.masterPos,
    cables: chainCables,
    nextId: idCounter
  };
}

function applyFactoryPreset(preset) {
  const state = createFactoryPresetState(preset);
  if (!state) return;
  scheduleSceneStateApply(state, preset.name || 'Factory');
}

function getUserPresets() {
  try {
    const parsed = JSON.parse(localStorage.getItem('pb_presets') || '{}');
    if (parsed && typeof parsed === 'object') return parsed;
  } catch(ex) {}
  return {};
}

function getPresetMeta() {
  try {
    const parsed = JSON.parse(localStorage.getItem('pb_preset_meta') || '{}');
    if (parsed && typeof parsed === 'object') return parsed;
  } catch(ex) {}
  return {};
}

function setPresetMeta(meta) {
  try {
    localStorage.setItem('pb_preset_meta', JSON.stringify(meta || {}));
  } catch(ex) {}
}

function getPresetSearchTerm() {
  const input = document.getElementById('pbPresetSearchInput');
  return (input?.value || '').trim().toLowerCase();
}

function isFavOnlyEnabled() {
  const cb = document.getElementById('pbPresetFavOnly');
  return !!cb?.checked;
}

function matchesPresetFilter(name, description, tags, favorite) {
  if (isFavOnlyEnabled() && !favorite) return false;
  const term = getPresetSearchTerm();
  if (!term) return true;
  const hay = `${name || ''} ${description || ''} ${tags || ''}`.toLowerCase();
  return hay.includes(term);
}

window.pbRefreshPresetBrowser = function() {
  renderPresetPanel();
};

function setUserPresets(presets) {
  localStorage.setItem('pb_presets', JSON.stringify(presets || {}));
}

function renderPresetPanel() {
  const factoryList = document.getElementById('pbFactoryPresetList');
  const userList = document.getElementById('pbUserPresetList');
  if (!factoryList || !userList) return;
  const meta = getPresetMeta();

  factoryList.innerHTML = '';
  FACTORY_PRESETS.forEach((preset) => {
    const pmeta = meta[`factory:${preset.id}`] || {};
    const tags = pmeta.tags || '';
    const favorite = !!pmeta.favorite;
    if (!matchesPresetFilter(preset.name, preset.description, tags, favorite)) return;
    const card = document.createElement('article');
    card.className = 'pb-preset-card';
    const stars = '★'.repeat(Math.max(0, Math.min(5, parseInt(pmeta.rating || 0, 10)))) || '☆☆☆☆☆';
    card.innerHTML = `
      <div class="pb-preset-card-head">
        <span class="pb-preset-name">${preset.name}</span>
        <div class="pb-preset-actions">
          <button class="pb-load">Cargar</button>
        </div>
      </div>
      <div class="pb-preset-desc">${preset.description}</div>
      <div class="pb-preset-desc">${favorite ? '⭐ ' : ''}${stars} ${tags ? '· ' + tags : ''}</div>
      <div class="pb-preset-meta">
        <button class="pb-fav">${favorite ? '★' : '☆'}</button>
        <select class="pb-rating">
          <option value="0">0★</option><option value="1">1★</option><option value="2">2★</option>
          <option value="3">3★</option><option value="4">4★</option><option value="5">5★</option>
        </select>
        <input class="pb-tags" type="text" maxlength="42" placeholder="tags" value="${tags}">
      </div>
    `;
    const key = `factory:${preset.id}`;
    const ratingSel = card.querySelector('.pb-rating');
    if (ratingSel) ratingSel.value = String(parseInt(pmeta.rating || 0, 10));
    card.querySelector('.pb-load')?.addEventListener('click', () => {
      applyFactoryPreset(preset);
    });
    card.querySelector('.pb-fav')?.addEventListener('click', () => {
      const m = getPresetMeta();
      const cur = m[key] || {};
      cur.favorite = !cur.favorite;
      m[key] = cur;
      setPresetMeta(m);
      renderPresetPanel();
    });
    card.querySelector('.pb-rating')?.addEventListener('change', (ev) => {
      const m = getPresetMeta();
      const cur = m[key] || {};
      cur.rating = Math.max(0, Math.min(5, parseInt(ev.target.value || '0', 10) || 0));
      m[key] = cur;
      setPresetMeta(m);
      renderPresetPanel();
    });
    card.querySelector('.pb-tags')?.addEventListener('change', (ev) => {
      const m = getPresetMeta();
      const cur = m[key] || {};
      cur.tags = String(ev.target.value || '').trim();
      m[key] = cur;
      setPresetMeta(m);
      renderPresetPanel();
    });
    factoryList.appendChild(card);
  });

  userList.innerHTML = '';
  const userPresets = getUserPresets();
  const names = Object.keys(userPresets)
    .filter((name) => {
      const pmeta = meta[`user:${name}`] || {};
      return matchesPresetFilter(name, 'Preset usuario', pmeta.tags || '', !!pmeta.favorite);
    })
    .sort((a, b) => a.localeCompare(b));
  if (names.length === 0) {
    const empty = document.createElement('article');
    empty.className = 'pb-preset-card';
    empty.innerHTML = '<div class="pb-preset-desc">Sin presets guardados todavía.</div>';
    userList.appendChild(empty);
    return;
  }

  names.forEach((name) => {
    const pmeta = meta[`user:${name}`] || {};
    const tags = pmeta.tags || '';
    const favorite = !!pmeta.favorite;
    const stars = '★'.repeat(Math.max(0, Math.min(5, parseInt(pmeta.rating || 0, 10)))) || '☆☆☆☆☆';
    const card = document.createElement('article');
    card.className = 'pb-preset-card';
    card.innerHTML = `
      <div class="pb-preset-card-head">
        <span class="pb-preset-name">${name}</span>
        <div class="pb-preset-actions">
          <button class="pb-load">Cargar</button>
          <button class="pb-delete">Borrar</button>
        </div>
      </div>
      <div class="pb-preset-desc">Preset usuario ${favorite ? '⭐' : ''} ${stars}</div>
      <div class="pb-preset-desc">${tags ? 'tags: ' + tags : ''}</div>
      <div class="pb-preset-meta">
        <button class="pb-fav">${favorite ? '★' : '☆'}</button>
        <select class="pb-rating">
          <option value="0">0★</option><option value="1">1★</option><option value="2">2★</option>
          <option value="3">3★</option><option value="4">4★</option><option value="5">5★</option>
        </select>
        <input class="pb-tags" type="text" maxlength="42" placeholder="tags" value="${tags}">
      </div>
    `;
    const key = `user:${name}`;
    const ratingSel = card.querySelector('.pb-rating');
    if (ratingSel) ratingSel.value = String(parseInt(pmeta.rating || 0, 10));
    card.querySelector('.pb-load')?.addEventListener('click', () => {
      scheduleSceneStateApply(userPresets[name], name);
    });
    card.querySelector('.pb-fav')?.addEventListener('click', () => {
      const m = getPresetMeta();
      const cur = m[key] || {};
      cur.favorite = !cur.favorite;
      m[key] = cur;
      setPresetMeta(m);
      renderPresetPanel();
    });
    card.querySelector('.pb-rating')?.addEventListener('change', (ev) => {
      const m = getPresetMeta();
      const cur = m[key] || {};
      cur.rating = Math.max(0, Math.min(5, parseInt(ev.target.value || '0', 10) || 0));
      m[key] = cur;
      setPresetMeta(m);
      renderPresetPanel();
    });
    card.querySelector('.pb-tags')?.addEventListener('change', (ev) => {
      const m = getPresetMeta();
      const cur = m[key] || {};
      cur.tags = String(ev.target.value || '').trim();
      m[key] = cur;
      setPresetMeta(m);
      renderPresetPanel();
    });
    card.querySelector('.pb-delete')?.addEventListener('click', () => {
      const presets = getUserPresets();
      delete presets[name];
      setUserPresets(presets);
      const m = getPresetMeta();
      delete m[key];
      setPresetMeta(m);
      renderPresetPanel();
    });
    userList.appendChild(card);
  });
}

/* ──────────── UTILS ──────────── */
function snap(v) { return Math.round(v / SNAP) * SNAP; }

/* ──────────── FX DEBUG TESTER ──────────── */
let testerAllTracks = false;

function testerLog(msg, cls = '') {
  const log = document.getElementById('pbTestLog');
  if (!log) return;
  const time = new Date().toLocaleTimeString('es', {hour:'2-digit',minute:'2-digit',second:'2-digit'});
  log.innerHTML += `<div class="${cls}">[${time}] ${msg}</div>`;
  log.scrollTop = log.scrollHeight;
}

window.pbOpenFxTester = function() {
  togglePBMenu();
  const panel = document.getElementById('pbFxTester');
  if (!panel) return;
  panel.classList.remove('hidden');

  /* Populate track selector */
  const trackSel = document.getElementById('pbTestTrack');
  if (trackSel && trackSel.options.length === 0) {
    for (let i = 0; i < 16; i++) {
      const opt = document.createElement('option');
      opt.value = i;
      opt.textContent = `PAD ${i + 1} — ${TRACK_NAMES[i]}`;
      trackSel.appendChild(opt);
    }
  }

  /* Populate FX type selector */
  const fxSel = document.getElementById('pbTestFxType');
  if (fxSel && fxSel.options.length === 0) {
    Object.entries(FX_DEFS)
      .filter(([key]) => key !== 'echo' && !UNSUPPORTED_PRE_FX.has(key))
      .forEach(([key, def]) => {
      const opt = document.createElement('option');
      opt.value = key;
      opt.textContent = `${def.icon} ${def.label}`;
      fxSel.appendChild(opt);
    });
  }

  pbTestBuildParams();
  testerLog('FX Tester abierto — selecciona track y FX', 'log-ok');
};

window.pbCloseFxTester = function() {
  const panel = document.getElementById('pbFxTester');
  if (panel) panel.classList.add('hidden');
};

window.pbTestToggleAllTracks = function() {
  testerAllTracks = !testerAllTracks;
  const btn = document.getElementById('pbTestAllTracksBtn');
  if (btn) {
    btn.textContent = testerAllTracks ? 'ALL ✓' : 'ALL';
    btn.style.background = testerAllTracks ? 'rgba(0,229,255,0.25)' : 'rgba(255,255,255,0.08)';
    btn.style.borderColor = testerAllTracks ? 'rgba(0,229,255,0.5)' : 'rgba(255,255,255,0.2)';
    btn.style.color = testerAllTracks ? '#00e5ff' : '#dff7ff';
  }
  testerLog(testerAllTracks ? 'Modo ALL TRACKS activado' : 'Modo single track', 'log-warn');
};

window.pbTestBuildParams = function() {
  const container = document.getElementById('pbTestParams');
  const fxSel = document.getElementById('pbTestFxType');
  if (!container || !fxSel) return;

  const fxType = fxSel.value;
  const def = FX_DEFS[fxType];
  if (!def) { container.innerHTML = ''; return; }

  container.innerHTML = '';
  def.params.forEach(param => {
    if (param.type === 'select') {
      const row = document.createElement('div');
      row.className = 'pb-tester-param-row';
      row.innerHTML = `
        <div class="pb-tester-param-label">
          <span>${param.label.toUpperCase()}</span>
          <span class="pb-tester-param-val" id="pbTestVal_${param.id}">—</span>
        </div>
        <select id="pbTestParam_${param.id}" style="height:38px;border:1.5px solid var(--pb-border);border-radius:8px;background:rgba(0,0,0,0.3);color:var(--pb-text);font-size:13px;padding:0 10px">
          ${param.options.map((o, i) => `<option value="${i}">${o}</option>`).join('')}
        </select>`;
      container.appendChild(row);
      const sel = document.getElementById(`pbTestParam_${param.id}`);
      sel.value = param.def;
      document.getElementById(`pbTestVal_${param.id}`).textContent = param.options[param.def];
      sel.addEventListener('change', () => {
        document.getElementById(`pbTestVal_${param.id}`).textContent = param.options[sel.value];
      });
    } else {
      const step = param.step || 1;
      const unit = param.unit || '';
      const row = document.createElement('div');
      row.className = 'pb-tester-param-row';
      row.innerHTML = `
        <div class="pb-tester-param-label">
          <span>${param.label.toUpperCase()} ${unit ? '(' + unit + ')' : ''}</span>
          <span class="pb-tester-param-val" id="pbTestVal_${param.id}">${param.def}${unit}</span>
        </div>
        <input type="range" class="pb-tester-param-range" id="pbTestParam_${param.id}"
          min="${param.min}" max="${param.max}" value="${param.def}" step="${step}">`;
      container.appendChild(row);
      const slider = document.getElementById(`pbTestParam_${param.id}`);
      slider.addEventListener('input', () => {
        const v = parseFloat(slider.value);
        document.getElementById(`pbTestVal_${param.id}`).textContent = `${v}${unit}`;
      });
    }
  });
};

function testerGetParams() {
  const fxSel = document.getElementById('pbTestFxType');
  const fxType = fxSel ? fxSel.value : '';
  const def = FX_DEFS[fxType];
  if (!def) return {};

  const params = {};
  def.params.forEach(param => {
    const el = document.getElementById(`pbTestParam_${param.id}`);
    if (!el) return;
    if (param.type === 'select') {
      params[param.id] = parseInt(el.value, 10);
    } else {
      params[param.id] = parseFloat(el.value);
    }
  });
  return params;
}

function testerGetTracks() {
  if (testerAllTracks) return Array.from({length: 16}, (_, i) => i);
  const sel = document.getElementById('pbTestTrack');
  return sel ? [parseInt(sel.value, 10)] : [0];
}

window.pbTestSend = function() {
  const fxSel = document.getElementById('pbTestFxType');
  const fxType = fxSel ? fxSel.value : '';
  const def = FX_DEFS[fxType];
  if (!def) return;

  const params = testerGetParams();
  const tracks = testerGetTracks();
  const fakeNode = { fxType, params };

  testerLog(`━━━ SEND: ${def.label} → Track${testerAllTracks ? 's 1-16' : ' ' + (tracks[0]+1) + ' (' + TRACK_NAMES[tracks[0]] + ')'}`, 'log-cmd');
  testerLog(`Params: ${JSON.stringify(params)}`, 'log-cmd');

  tracks.forEach(track => {
    applyFxToTrack(track, fakeNode);
  });

  testerLog(`✓ Enviado correctamente a ${tracks.length} track(s)`, 'log-ok');

  /* Flash the send button */
  const btn = document.querySelector('.pb-tester-send');
  if (btn) {
    btn.style.boxShadow = '0 0 30px rgba(105,240,174,0.5)';
    setTimeout(() => { btn.style.boxShadow = ''; }, 400);
  }
};

window.pbTestClear = function() {
  const fxSel = document.getElementById('pbTestFxType');
  const fxType = fxSel ? fxSel.value : '';
  const def = FX_DEFS[fxType];
  if (!def) return;

  const tracks = testerGetTracks();
  const fakeNode = { fxType };

  testerLog(`━━━ CLEAR: ${def.label} → Track${testerAllTracks ? 's 1-16' : ' ' + (tracks[0]+1)}`, 'log-warn');

  tracks.forEach(track => {
    clearFxFromTrack(track, fakeNode);
  });

  testerLog(`✓ FX limpiado en ${tracks.length} track(s)`, 'log-ok');
};

window.pbTestClearAll = function() {
  const tracks = testerGetTracks();
  testerLog(`━━━ CLEAR ALL FX → Track${testerAllTracks ? 's 1-16' : ' ' + (tracks[0]+1)}`, 'log-err');

  tracks.forEach(track => {
    sendCmd('clearTrackFilter', { track });
    sendCmd('setTrackDistortion', { track, amount: 0, mode: 0 });
    sendCmd('setTrackBitCrush', { track, value: 16 });
    sendCmd('setTrackEcho', { track, active: false });
    sendCmd('setTrackFlanger', { track, active: false });
    sendCmd('setTrackCompressor', { track, active: false });
  });

  testerLog(`✓ TODOS los FX limpiados en ${tracks.length} track(s)`, 'log-ok');
};

function updateStatus() {
  document.getElementById('pbNodeCount').textContent = nodes.length;
  document.getElementById('pbCableCount').textContent = cables.length;

  /* Count effective hardware-slot winners, not merely visible connected nodes. */
  const activeNodeIds = new Set();
  for (let track = 0; track < 16; track++) {
    getTrackRouteWinners(track).winners.forEach(node => activeNodeIds.add(node.id));
  }
  let activeFx = activeNodeIds.size;
  /* Also count active POST master FX */
  activeFx += postActiveCount();
  const fxEl = document.getElementById('pbFxActive');
  const fxStatus = document.getElementById('pbFxStatus');
  if (fxEl) fxEl.textContent = activeFx;
  if (fxStatus) fxStatus.style.color = activeFx > 0 ? '#69f0ae' : '#777';
}

/* ══════════════════════════════════════════════════════════
   POST MASTER FX — Tab & Controls
   ══════════════════════════════════════════════════════════ */

/* --- Tab switching --- */
window.pbSwitchTab = function(tab) {
  const tabs = document.querySelectorAll('.pb-tab');
  tabs.forEach(t => t.classList.toggle('active', t.dataset.tab === tab));
  const canvas = document.getElementById('pbCanvas');
  const post = document.getElementById('pbPostPanel');
  const toolbar = document.getElementById('pbToolbar');
  if (tab === 'post') {
    canvas.style.display = 'none';
    post.classList.remove('hidden');
    toolbar.style.display = 'none';
  } else {
    canvas.style.display = '';
    post.classList.add('hidden');
    toolbar.style.display = '';
  }
}

/* --- Module active state tracking --- */
const postState = {
  filter: false, distortion: false, bitcrusher: false,
  phaser: false, flanger: false, delay: false, compressor: false
};

window.postActiveCount = function() {
  return Object.values(postState).filter(Boolean).length;
}

window.postToggle = function(mod) {
  postState[mod] = !postState[mod];
  const btn = document.getElementById(postToggleId(mod));
  const modEl = btn ? btn.closest('.post-module') : null;
  if (btn) {
    btn.textContent = postState[mod] ? 'ON' : 'OFF';
    btn.classList.toggle('on', postState[mod]);
  }
  if (modEl) modEl.classList.toggle('active', postState[mod]);

  /* Send activate/deactivate to firmware */
  switch(mod) {
    case 'filter':    postSendFilter(); break;
    case 'distortion': postSendDist(); break;
    case 'bitcrusher': postSendBit(); break;
    case 'phaser':    sendCmd('setPhaserActive', { value: postState.phaser }); break;
    case 'flanger':   sendCmd('setFlangerActive', { value: postState.flanger }); break;
    case 'delay':     sendCmd('setDelayActive', { value: postState.delay }); break;
    case 'compressor':sendCmd('setCompressorActive', { value: postState.compressor }); break;
  }
  updateStatus();
}

window.postToggleId = function(mod) {
  const map = { filter:'postFilterToggle', distortion:'postDistToggle', bitcrusher:'postBitToggle',
    phaser:'postPhaserToggle', flanger:'postFlangerToggle', delay:'postDelayToggle', compressor:'postCompToggle' };
  return map[mod];
}

/* --- Filter --- */
window.postSendFilter = function() {
  const type = parseInt(document.getElementById('postFilterType').value);
  const cutoff = parseFloat(document.getElementById('postFilterCutoff').value);
  const resoRaw = parseFloat(document.getElementById('postFilterReso').value);
  const reso = resoRaw / 100;
  document.getElementById('postFilterCutoffVal').textContent = cutoff >= 1000 ? (cutoff/1000).toFixed(1)+'k' : cutoff;
  document.getElementById('postFilterResoVal').textContent = reso.toFixed(1);
  if (!postState.filter) { sendCmd('setFilter', { type: 0 }); return; }
  sendCmd('setFilter', { type });
  sendCmd('setFilterCutoff', { value: cutoff });
  sendCmd('setFilterResonance', { value: reso });
}

/* --- Distortion --- */
window.postSendDist = function() {
  const amount = parseInt(document.getElementById('postDistAmount').value);
  const mode = parseInt(document.getElementById('postDistMode').value);
  document.getElementById('postDistAmountVal').textContent = amount;
  if (!postState.distortion) { sendCmd('setDistortion', { value: 0 }); return; }
  sendCmd('setDistortion', { value: amount });
  sendCmd('setDistortionMode', { value: mode });
}

/* --- Bitcrusher --- */
window.postSendBit = function() {
  const bits = parseInt(document.getElementById('postBitBits').value);
  const rate = parseInt(document.getElementById('postBitRate').value);
  document.getElementById('postBitBitsVal').textContent = bits;
  document.getElementById('postBitRateVal').textContent = rate >= 1000 ? (rate/1000).toFixed(1)+'k' : rate;
  if (!postState.bitcrusher) {
    sendCmd('setBitCrush', { value: 16 });
    sendCmd('setSampleRate', { value: 44100 });
    return;
  }
  sendCmd('setBitCrush', { value: bits });
  sendCmd('setSampleRate', { value: rate });
}

/* --- Phaser --- */
window.postSendPhaser = function() {
  const rate = parseInt(document.getElementById('postPhaserRate').value);
  const depth = parseInt(document.getElementById('postPhaserDepth').value);
  const fb = parseInt(document.getElementById('postPhaserFb').value);
  document.getElementById('postPhaserRateVal').textContent = rate;
  document.getElementById('postPhaserDepthVal').textContent = depth;
  document.getElementById('postPhaserFbVal').textContent = fb;
  if (!postState.phaser) return;
  sendCmd('setPhaserRate', { value: rate });
  sendCmd('setPhaserDepth', { value: depth });
  sendCmd('setPhaserFeedback', { value: fb });
}

/* --- Flanger --- */
window.postSendFlanger = function() {
  const rate = parseInt(document.getElementById('postFlangerRate').value);
  const depth = parseInt(document.getElementById('postFlangerDepth').value);
  const fb = parseInt(document.getElementById('postFlangerFb').value);
  const mix = parseInt(document.getElementById('postFlangerMix').value);
  document.getElementById('postFlangerRateVal').textContent = rate;
  document.getElementById('postFlangerDepthVal').textContent = depth;
  document.getElementById('postFlangerFbVal').textContent = fb;
  document.getElementById('postFlangerMixVal').textContent = mix;
  if (!postState.flanger) return;
  sendCmd('setFlangerRate', { value: rate });
  sendCmd('setFlangerDepth', { value: depth });
  sendCmd('setFlangerFeedback', { value: fb });
  sendCmd('setFlangerMix', { value: mix });
}

/* --- Delay --- */
window.postSendDelay = function() {
  const time = parseInt(document.getElementById('postDelayTime').value);
  const fb = parseInt(document.getElementById('postDelayFb').value);
  const mix = parseInt(document.getElementById('postDelayMix').value);
  document.getElementById('postDelayTimeVal').textContent = time + 'ms';
  document.getElementById('postDelayFbVal').textContent = fb;
  document.getElementById('postDelayMixVal').textContent = mix;
  if (!postState.delay) return;
  sendCmd('setDelayTime', { value: time });
  sendCmd('setDelayFeedback', { value: fb });
  sendCmd('setDelayMix', { value: mix });
}

/* --- Compressor --- */
window.postSendComp = function() {
  const thresh = parseInt(document.getElementById('postCompThresh').value);
  const ratioRaw = parseInt(document.getElementById('postCompRatio').value);
  const ratio = ratioRaw / 10;
  const atk = parseInt(document.getElementById('postCompAtk').value);
  const rel = parseInt(document.getElementById('postCompRel').value);
  const gain = parseInt(document.getElementById('postCompGain').value);
  document.getElementById('postCompThreshVal').textContent = thresh + 'dB';
  document.getElementById('postCompRatioVal').textContent = ratio.toFixed(1);
  document.getElementById('postCompAtkVal').textContent = atk + 'ms';
  document.getElementById('postCompRelVal').textContent = rel + 'ms';
  document.getElementById('postCompGainVal').textContent = gain + 'dB';
  if (!postState.compressor) return;
  sendCmd('setCompressorThreshold', { value: thresh });
  sendCmd('setCompressorRatio', { value: ratio });
  sendCmd('setCompressorAttack', { value: atk });
  sendCmd('setCompressorRelease', { value: rel });
  sendCmd('setCompressorMakeupGain', { value: gain });
}

/* --- Clear All Master FX --- */
window.postClearAll = function() {
  /* Reset all modules */
  Object.keys(postState).forEach(mod => {
    postState[mod] = false;
    const btn = document.getElementById(postToggleId(mod));
    const modEl = btn ? btn.closest('.post-module') : null;
    if (btn) { btn.textContent = 'OFF'; btn.classList.remove('on'); }
    if (modEl) modEl.classList.remove('active');
  });
  /* Send resets to firmware */
  sendCmd('setFilter', { type: 0 });
  sendCmd('setDistortion', { value: 0 });
  sendCmd('setBitCrush', { value: 16 });
  sendCmd('setSampleRate', { value: 44100 });
  sendCmd('setPhaserActive', { value: false });
  sendCmd('setFlangerActive', { value: false });
  sendCmd('setDelayActive', { value: false });
  sendCmd('setCompressorActive', { value: false });
  updateStatus();
}

/* ──────────── BOOT ──────────── */
document.addEventListener('DOMContentLoaded', init);

})();
