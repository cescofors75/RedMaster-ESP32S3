'use strict';

// ══════════════════════════════════════════════════════
// Mapeo de sonidos a índices de pad del master
// Ajusta estos índices según tu configuración
// ══════════════════════════════════════════════════════
const SOUND_PAD_MAP = {
  kick:  0,  // BD (Bass Drum)
  snare: 1,  // SD (Snare)
  hihat: 2,  // CH (Closed Hi-Hat)
  clap:  5,  // CP (Clap)
  tom:   8,  // LT (Low Tom)
  bass:  6,  // RS (puedes cambiar)
  oh:    3,  // OH (Open Hi-Hat)
  ride:  4,  // CY (Cymbal)
};

const MOBILE_MODE = window.matchMedia('(max-width: 900px)').matches;

// WebSocket independiente para gesture page (sin depender de app.js)
let gestureWs = null;
let wsReconnectTimer = null;

function apiBaseUrl() {
  return window.location.origin;
}

function wsUrl() {
  const proto = 'ws:';
  return `${proto}//${window.location.host}/ws`;
}

function scheduleWsReconnect() {
  if (wsReconnectTimer) return;
  wsReconnectTimer = setTimeout(() => {
    wsReconnectTimer = null;
    initGestureWebSocket();
  }, 1200);
}

function initGestureWebSocket() {
  try {
    if (gestureWs && (gestureWs.readyState === WebSocket.OPEN || gestureWs.readyState === WebSocket.CONNECTING)) {
      return;
    }

    gestureWs = new WebSocket(wsUrl());

    gestureWs.onopen = () => {
      setCameraStatus('WS OPEN');
    };

    gestureWs.onclose = () => {
      setCameraStatus('WS CLOSED');
      scheduleWsReconnect();
    };

    gestureWs.onerror = () => {
      setCameraStatus('WS ERROR');
    };
  } catch (err) {
    setCameraStatus('WS INIT ERROR');
    scheduleWsReconnect();
  }
}

async function triggerSoundHttp(padIdx) {
  try {
    const response = await fetch(`${apiBaseUrl()}/api/trigger?pad=${padIdx}`, {
      method: 'GET',
      cache: 'no-store'
    });
    return response.ok;
  } catch (err) {
    console.warn('[GESTURE] HTTP trigger failed', err);
    return false;
  }
}

// ── GESTURE DETECTION ────────────────────────────────
const GCD = MOBILE_MODE
  ? { kick: 150, snare: 120, hihat: 65, clap: 120, tom: 120, bass: 150, oh: 90, ride: 95, mute: 500, unmute: 500 }
  : { kick: 200, snare: 150, hihat: 80, clap: 145, tom: 145, bass: 180, oh: 100, ride: 110, mute: 600, unmute: 600 };
const gLastT = {};
Object.keys(GCD).forEach(k => gLastT[k] = 0);

const CONFIRM_FRAMES = MOBILE_MODE ? 2 : 3;
let rBuf = [], lBuf = [];
let prevRightWrist = null;
let lastVelocity = 96;

function handSize(lm) {
  return Math.hypot(lm[0].x - lm[9].x, lm[0].y - lm[9].y) || 0.1;
}

function fingerUp(tip, pip, sz) {
  return (pip.y - tip.y) > sz * 0.12;
}

function thumbUp(tt, ti, sz) {
  return (ti.y - tt.y) > sz * 0.08;
}

function detectR(lm) {
  const sz = handSize(lm);
  const tt = lm[4], ti = lm[3];
  const it = lm[8], ip = lm[6];
  const mt = lm[12], mp = lm[10];
  const rt = lm[16], rp = lm[14];
  const pt = lm[20], pp = lm[18];
  const tU = thumbUp(tt, ti, sz);
  const iU = fingerUp(it, ip, sz);
  const mU = fingerUp(mt, mp, sz);
  const rU = fingerUp(rt, rp, sz);
  const pU = fingerUp(pt, pp, sz);
  const ext = [iU, mU, rU, pU].filter(Boolean).length;
  const pinch = Math.hypot(tt.x - it.x, tt.y - it.y) < sz * 0.5;

  if (pinch && ext <= 1) return 'clap';
  if (tU && pU && !iU && !mU && !rU) return 'tom';
  if (iU && pU && !mU && !rU) return 'bass';
  if (iU && mU && rU && !pU) return 'ride';
  if (iU && mU && !rU && !pU) return 'oh';
  if (ext >= 4) return 'hihat';
  if (iU && !mU && !rU && !pU) return 'snare';
  if (ext === 0 && !tU) return 'kick';
  return null;
}

function detectL(lm) {
  const sz = handSize(lm);
  const it = lm[8], ip = lm[6];
  const mt = lm[12], mp = lm[10];
  const rt = lm[16], rp = lm[14];
  const pt = lm[20], pp = lm[18];
  const iU = fingerUp(it, ip, sz);
  const mU = fingerUp(mt, mp, sz);
  const rU = fingerUp(rt, rp, sz);
  const pU = fingerUp(pt, pp, sz);
  const ext = [iU, mU, rU, pU].filter(Boolean).length;
  if (ext === 0) return { action: 'mute' };
  if (ext >= 4) return { action: 'unmute' };
  return { action: 'fx', x: lm[0].x, y: lm[0].y };
}

function updateHudGesture(name, velocity = lastVelocity) {
  const gestureName = (name || '--').toUpperCase();
  const hudGesture = document.getElementById('hudGesture');
  const perfGesture = document.getElementById('perfGesture');
  const metricPad = document.getElementById('metricPad');
  const perfIntensity = document.getElementById('perfIntensity');
  if (hudGesture) hudGesture.textContent = gestureName;
  if (perfGesture) perfGesture.textContent = gestureName;
  if (perfIntensity) perfIntensity.textContent = `${velocity}`;
  if (metricPad) metricPad.textContent = SOUND_PAD_MAP[name] !== undefined ? `${SOUND_PAD_MAP[name]}` : '--';
}

function updateVelocityHud(velocity) {
  lastVelocity = velocity;
  const hudVelocity = document.getElementById('hudVelocity');
  if (hudVelocity) hudVelocity.textContent = `VEL ${String(velocity).padStart(3, '0')}`;
}

function pulseStage(name) {
  const pulse = document.getElementById('stagePulse');
  if (pulse) {
    pulse.classList.remove('blast');
    void pulse.offsetWidth;
    pulse.classList.add('blast');
    clearTimeout(pulse._t);
    pulse._t = setTimeout(() => pulse.classList.remove('blast'), 120);
  }

  const ref = document.getElementById(`ref-${name}`);
  if (ref) {
    ref.classList.add('lit');
    clearTimeout(ref._t);
    ref._t = setTimeout(() => ref.classList.remove('lit'), 200);
  }
}

function wristEnergy(lm, prev) {
  if (!lm || !lm[0]) return { velocity: 96, next: prev };
  const wrist = lm[0];
  if (!prev) return { velocity: 96, next: { x: wrist.x, y: wrist.y } };
  const dist = Math.hypot(wrist.x - prev.x, wrist.y - prev.y);
  const velocity = Math.max(55, Math.min(127, Math.round(62 + dist * 950)));
  return { velocity, next: { x: wrist.x, y: wrist.y } };
}

function confirmGesture(buf, newG, n) {
  buf.push(newG);
  if (buf.length > n) buf.shift();
  if (buf.length < n) return null;
  const last = buf[buf.length - 1];
  if (!last) return null;
  return buf.every(g => g === last) ? last : null;
}

function tryFire(name, vel = 0.8) {
  const now = Date.now();
  const cd = GCD[name] || 250;
  if (now - gLastT[name] < cd) return false;
  gLastT[name] = now;
  if (name === 'mute') return true;
  if (name === 'unmute') return true;
  triggerSound(name);
  return true;
}

// ══════════════════════════════════════════════════════
// TRIGGER via WebSocket al Master
// ══════════════════════════════════════════════════════

async function triggerSound(name, velocity = 127) {
  const padIdx = SOUND_PAD_MAP[name];
  if (padIdx === undefined) {
    console.warn(`[GESTURE] Sound "${name}" not in SOUND_PAD_MAP`);
    return;
  }

  let sent = false;
  if (gestureWs && gestureWs.readyState === WebSocket.OPEN) {
    // Enviar comando binario 0x90 PADINDEX VELOCITY al master
    const data = new Uint8Array(3);
    data[0] = 0x90;
    data[1] = padIdx;
    data[2] = velocity;
    gestureWs.send(data);
    sent = true;
  } else {
    console.warn(`[GESTURE] WebSocket not ready (state=${gestureWs?.readyState}), trying HTTP fallback`);
    sent = await triggerSoundHttp(padIdx);
  }

  if (!sent) return;

  updateVelocityHud(velocity);
  updateHudGesture(name, velocity);
  flashGs(name);
  showFlash(name.toUpperCase());
  flashTrig();
  pulseStage(name);
}

function mirrorLandmarks(lm) {
  return lm.map((point) => ({
    ...point,
    x: 1 - point.x
  }));
}

// ══════════════════════════════════════════════════════
// VISUAL FEEDBACK
// ══════════════════════════════════════════════════════

const _gt = {};
function flashGs(n) {
  const e = document.getElementById('gs-' + n);
  if (!e) return;
  e.classList.add('lit');
  clearTimeout(_gt[n]);
  _gt[n] = setTimeout(() => e.classList.remove('lit'), 240);
}

function flashTrig() {
  const e = document.getElementById('ledTrig');
  e.className = 'tled r';
  clearTimeout(e._t);
  e._t = setTimeout(() => e.className = 'tled', 110);
}

let _ft;
function showFlash(t) {
  const e = document.getElementById('gFlash');
  e.textContent = t;
  e.classList.add('show');
  clearTimeout(_ft);
  _ft = setTimeout(() => e.classList.remove('show'), 350);
}

// ══════════════════════════════════════════════════════
// CAMERA + MEDIAPIPE
// ══════════════════════════════════════════════════════

let useCamera = false, camStream = null, mpHands = null;
let liveLmR = null, liveLmL = null, liveGR = null, liveGL = null;
let frameLoopHandle = 0;
let frameLoopBusy = false;

function setCameraStatus(msg) {
  const status = document.getElementById('wsStatus');
  if (status) status.textContent = msg;
  const hudWs = document.getElementById('hudWs');
  if (hudWs) hudWs.textContent = msg.replace('WS ', '').replace('CAM ', '');
}

function getUserMediaCompat(constraints) {
  if (navigator.mediaDevices && typeof navigator.mediaDevices.getUserMedia === 'function') {
    return navigator.mediaDevices.getUserMedia(constraints);
  }

  const legacy = navigator.getUserMedia || navigator.webkitGetUserMedia || navigator.mozGetUserMedia;
  if (!legacy) {
    return Promise.reject(new Error('getUserMedia_not_supported'));
  }

  return new Promise((resolve, reject) => {
    legacy.call(navigator, constraints, resolve, reject);
  });
}

function waitForVideoReady(video) {
  if (video.readyState >= 2) {
    return Promise.resolve();
  }

  return new Promise((resolve) => {
    const onReady = () => {
      video.removeEventListener('loadedmetadata', onReady);
      video.removeEventListener('canplay', onReady);
      resolve();
    };
    video.addEventListener('loadedmetadata', onReady, { once: true });
    video.addEventListener('canplay', onReady, { once: true });
  });
}

function startHandFrameLoop(video) {
  const tick = async () => {
    if (!useCamera || !mpHands || !video || video.readyState < 2) {
      frameLoopHandle = requestAnimationFrame(tick);
      return;
    }

    if (!frameLoopBusy) {
      frameLoopBusy = true;
      try {
        await mpHands.send({ image: video });
      } catch (err) {
        console.warn('[GESTURE] mpHands.send failed', err);
      } finally {
        frameLoopBusy = false;
      }
    }

    frameLoopHandle = requestAnimationFrame(tick);
  };

  if (frameLoopHandle) {
    cancelAnimationFrame(frameLoopHandle);
  }
  frameLoopHandle = requestAnimationFrame(tick);
}

async function toggleCamera() {
  const btn = document.getElementById('camBtn');
  if (useCamera) {
    useCamera = false;
    liveLmR = null;
    liveLmL = null;
    if (frameLoopHandle) {
      cancelAnimationFrame(frameLoopHandle);
      frameLoopHandle = 0;
    }
    frameLoopBusy = false;
    if (camStream) {
      camStream.getTracks().forEach(t => t.stop());
      camStream = null;
    }
    mpHands = null;
    document.getElementById('videoEl').classList.remove('visible');
    document.getElementById('app')?.classList.remove('cam-active');
    ['ledL', 'ledR'].forEach(id => document.getElementById(id).className = 'led tled');
    const btnText = document.getElementById('camBtnText');
    if (btnText) btnText.textContent = 'Cámara'; else btn.textContent = '▶ ACTIVAR CÁMARA';
    btn.classList.remove('active');
    setCameraStatus('WS OPEN');
    return;
  }

  setCameraStatus('CAM DISABLED: HTTP LOCAL');
  alert('La cámara está desactivada: RED808 funciona sólo por HTTP local y sin dependencias externas.');
}

function onHandResults(res) {
  liveLmR = null;
  liveLmL = null;

  if (!res.multiHandLandmarks?.length) {
    ['ledL', 'ledR'].forEach(id => document.getElementById(id).className = 'tled');
    const metricRight = document.getElementById('metricRight');
    const metricLeft = document.getElementById('metricLeft');
    if (metricRight) metricRight.textContent = 'idle';
    if (metricLeft) metricLeft.textContent = 'idle';
    rBuf = [];
    lBuf = [];
    return;
  }

  for (let i = 0; i < res.multiHandLandmarks.length; i++) {
    const lm = res.multiHandLandmarks[i];
    const label = res.multiHandedness?.[i]?.label || 'Right';
    if (label === 'Right') liveLmR = lm;
    else liveLmL = lm;
  }

  // RIGHT HAND
  if (liveLmR) {
    document.getElementById('ledR').className = 'tled r';
    const metricRight = document.getElementById('metricRight');
    if (metricRight) metricRight.textContent = 'tracking';
    const raw = detectR(liveLmR);
    const energy = wristEnergy(liveLmR, prevRightWrist);
    prevRightWrist = energy.next;
    const confirmed = confirmGesture(rBuf, raw, CONFIRM_FRAMES);
    if (confirmed) {
      const now = Date.now();
      const cd = GCD[confirmed] || 250;
      if (now - gLastT[confirmed] >= cd) {
        gLastT[confirmed] = now;
        triggerSound(confirmed, energy.velocity);
      }
    }
    liveGR = raw;
    updateHudGesture(raw || 'ready', energy.velocity);
    updateVelocityHud(energy.velocity);
  } else {
    document.getElementById('ledR').className = 'tled';
    const metricRight = document.getElementById('metricRight');
    if (metricRight) metricRight.textContent = 'idle';
    liveGR = null;
    prevRightWrist = null;
    rBuf = [];
  }

  // LEFT HAND
  if (liveLmL) {
    document.getElementById('ledL').className = 'tled b';
    const metricLeft = document.getElementById('metricLeft');
    if (metricLeft) metricLeft.textContent = 'fx';
    const res2 = detectL(liveLmL);
    liveGL = res2.action;
    const confirmed = confirmGesture(lBuf, res2.action, CONFIRM_FRAMES);
    if (confirmed === 'mute') {
      console.log('[GESTURE] Mute detected');
      flashGs('mute');
    } else if (confirmed === 'unmute') {
      console.log('[GESTURE] Unmute detected');
      flashGs('unmute');
    }
  } else {
    document.getElementById('ledL').className = 'tled';
    const metricLeft = document.getElementById('metricLeft');
    if (metricLeft) metricLeft.textContent = 'idle';
    liveGL = null;
    lBuf = [];
  }
}

function loadScripts(urls) {
  return Promise.all(urls.map(url =>
    new Promise((res, rej) => {
      if (document.querySelector(`script[src="${url}"]`)) return res();
      const s = document.createElement('script');
      s.src = url;
      s.onload = res;
      s.onerror = rej;
      document.head.appendChild(s);
    })
  ));
}

// ══════════════════════════════════════════════════════
// MAIN LOOP
// ══════════════════════════════════════════════════════

function loop(ts) {
  try {
    const canvas = document.getElementById('mainCanvas');
    const view = document.getElementById('stage') || document.getElementById('cam-view');
    const W = Math.floor(view ? view.clientWidth : window.innerWidth) || 800;
    const H = Math.floor(view ? view.clientHeight : window.innerHeight) || 400;

    if (!isFinite(W) || !isFinite(H) || W <= 0 || H <= 0) {
      requestAnimationFrame(loop);
      return;
    }

    if (canvas.width !== W) canvas.width = W;
    if (canvas.height !== H) canvas.height = H;

    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, W, H);

    if (useCamera && liveLmR) {
      drawHand(ctx, mirrorLandmarks(liveLmR), W, H, 'rgba(192,57,43,.85)', 'rgba(255,107,107,1)', liveGR);
    }
    if (useCamera && liveLmL) {
      drawHand(ctx, mirrorLandmarks(liveLmL), W, H, 'rgba(41,128,185,.75)', 'rgba(100,180,255,1)', liveGL !== 'fx');
    }
  } catch (e) {
    console.warn('Loop error:', e);
  }

  requestAnimationFrame(loop);
}

// ── DRAW HAND ────────────────────────────────────────
const CONN = [
  [0, 1], [1, 2], [2, 3], [3, 4], [0, 5], [5, 6], [6, 7], [7, 8],
  [0, 9], [9, 10], [10, 11], [11, 12], [0, 13], [13, 14], [14, 15], [15, 16],
  [0, 17], [17, 18], [18, 19], [19, 20], [5, 9], [9, 13], [13, 17],
];

function drawHand(ctx, lm, W, H, color, glowColor, gesture) {
  ctx.save();
  if (gesture) {
    ctx.shadowColor = glowColor;
    ctx.shadowBlur = 22;
  }
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  for (const [a, b] of CONN) {
    if (!lm[a] || !lm[b]) continue;
    ctx.beginPath();
    ctx.moveTo(lm[a].x * W, lm[a].y * H);
    ctx.lineTo(lm[b].x * W, lm[b].y * H);
    ctx.stroke();
  }
  ctx.shadowBlur = 0;
  for (let i = 0; i < lm.length; i++) {
    const p = lm[i];
    if (!p) continue;
    const tip = [4, 8, 12, 16, 20].includes(i);
    const r = tip ? (gesture ? 7 : 5) : 3.5;
    ctx.beginPath();
    ctx.arc(p.x * W, p.y * H, r, 0, Math.PI * 2);
    ctx.fillStyle = tip && gesture ? glowColor : color;
    if (tip && gesture) {
      ctx.shadowColor = glowColor;
      ctx.shadowBlur = 12;
    }
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.strokeStyle = 'rgba(0,0,0,.6)';
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
  }
  ctx.restore();
}

// ── KEYBOARD ─────────────────────────────────────────
const KB = {
  q: 'kick', w: 'snare', e: 'hihat', r: 'clap',
  a: 'tom', s: 'bass', d: 'ride', f: 'oh'
};

document.addEventListener('keydown', e => {
  if (e.repeat) return;
  const k = KB[e.key?.toLowerCase()];
  if (k) triggerSound(k, 120);
});

// ── WebSocket Status Monitor ─────────────────────────
setInterval(() => {
  if (document.hidden) return;  // no trabajar en pestañas ocultas
  const status = document.getElementById('wsStatus');
  if (!status) return;
  if (!gestureWs) {
    status.textContent = 'WS not started';
    return;
  }
  const states = {
    0: 'CONNECTING',
    1: 'OPEN',
    2: 'CLOSING',
    3: 'CLOSED'
  };
  status.textContent = `WS ${states[gestureWs.readyState] || 'UNKNOWN'}`;
}, 1000);

// ── Init ─────────────────────────────────────────────
document.getElementById('ledAudio').className = 'tb-dot';
window.addEventListener('load', () => {
  if (MOBILE_MODE) document.body.classList.add('mobile-mode');
  updateHudGesture('--', lastVelocity);
  updateVelocityHud(lastVelocity);
  initGestureWebSocket();
  requestAnimationFrame(loop);
});
