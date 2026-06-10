// ═══════════════════════════════════════════════════════
// RED808 Melody Editor — Per-step note editor for synth tracks
// Loaded as deferred module by app.js
// ═══════════════════════════════════════════════════════

(function() {
'use strict';

// ── Client-side storage for melody data ──
// stepNotesData[track][step] = MIDI note (0 = rest)
// stepFlagsData[track][step] = flags (bit0=accent, bit1=slide)
const stepNotesData = Array.from({length: 16}, () => new Uint8Array(64));
const stepFlagsData = Array.from({length: 16}, () => new Uint8Array(64));

// MIDI note names
const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
function midiNoteName(n) {
  if (n < 1 || n > 127) return '—';
  return NOTE_NAMES[n % 12] + Math.floor(n / 12 - 1);
}

// Piano range: C2 (36) to C5 (72) = 3 octaves, good for bass/lead
const PIANO_LOW = 36;  // C2
const PIANO_HIGH = 72; // C5
const OCTAVE_LABELS = ['C2','C3','C4','C5'];
const LIVE_PIANO_LOW = 48;  // C3
const LIVE_PIANO_HIGH = 72; // C5
const BLACK_NOTES = [1, 3, 6, 8, 10];
const ASSIGN_STORAGE_KEY = 'red808_pad_melody_assignments_v1';

let selectedLiveNote = 60;
let melodyRecordEnabled = false;
let melodyRecordStep = 0;
let padMelodyAssignments = loadPadMelodyAssignments();

function isBlackNote(note) {
  return BLACK_NOTES.includes(note % 12);
}

function loadPadMelodyAssignments() {
  try {
    const saved = JSON.parse(localStorage.getItem(ASSIGN_STORAGE_KEY) || '[]');
    if (Array.isArray(saved)) return Array.from({length: 16}, (_, i) => saved[i] || null);
  } catch (_) {}
  return new Array(16).fill(null);
}

function savePadMelodyAssignments() {
  try { localStorage.setItem(ASSIGN_STORAGE_KEY, JSON.stringify(padMelodyAssignments)); } catch (_) {}
}

// Check if a track has a melodic synth engine (303/WT/SH101/FM)
function isMelodicTrack(track) {
  const eng = (window.padSynthEngine && window.padSynthEngine[track]);
  return typeof eng === 'number' && eng >= 3;
}

// ── Melody Presets ──
let melodyPresets = [];
function loadMelodyPresets() {
  fetch('/api/melody/list').then(r => r.json()).then(list => {
    melodyPresets = list || [];
  }).catch(() => { melodyPresets = []; });
}

function applyMelodyPreset(preset) {
  if (!preset || !preset.notes) return;
  const track = melodyEditorTrack;
  if (track < 0) return;
  const notes = preset.notes;
  const accents = preset.accents || [];
  const slides = preset.slides || [];
  const steps = notes.length;
  for (let s = 0; s < steps && s < 64; s++) {
    const note = notes[s] || 0;
    const flags = ((accents[s] ? 1 : 0) | (slides[s] ? 2 : 0));
    stepNotesData[track][s] = note;
    stepFlagsData[track][s] = flags;
    sendWS({cmd:'setStepNote', track, step: s, note, flags, silent: true});
  }
  updateNoteIndicators(track);
  if (window.showToast) window.showToast('Melody applied \u2014 activate steps in sequencer to play', window.TOAST_TYPES?.SUCCESS || 'success', 2000);
}

// ── Editor State ──
let melodyEditorTrack = -1;
let melodyEditorVisible = false;

// ── Send WebSocket helper ──
function sendWS(data) {
  if (window.ws && window.ws.readyState === 1) {
    window.ws.send(JSON.stringify(data));
  }
}

// ── Preview: play a single synth note ──
let _previewNoteOffTimer = null;

function previewNote(track, note, accent, slide) {
  if (track < 0 || track >= 16 || !note || note < 1) return;
  const eng = window.padSynthEngine ? window.padSynthEngine[track] : -1;
  if (eng < 0) { console.warn('[MelodyPreview] No engine for track', track); return; }
  console.log('[MelodyPreview] track:', track, 'engine:', eng, 'note:', note);
  sendWS({cmd: 'synthNoteOff', track, engine: eng});
  sendWS({cmd: 'synthNoteOnEx', engine: eng, note: note, velocity: 100,
          accent: !!accent, slide: !!slide});
  if (!_previewPlaying) {
    if (_previewNoteOffTimer) clearTimeout(_previewNoteOffTimer);
    _previewNoteOffTimer = setTimeout(() => {
      sendWS({cmd: 'synthNoteOff', track, engine: eng});
      _previewNoteOffTimer = null;
    }, 350);
  }
}

// ── Melody Preview: play the melody note-by-note ──
let _previewTimer = null;
let _previewPlaying = false;

function stopMelodyPreview() {
  if (_previewTimer) { clearTimeout(_previewTimer); _previewTimer = null; }
  if (_previewNoteOffTimer) { clearTimeout(_previewNoteOffTimer); _previewNoteOffTimer = null; }
  _previewPlaying = false;
  const offTrack = melodyEditorTrack >= 0 ? melodyEditorTrack : melodyTabTrack;
  const offEngine = offTrack >= 0 && window.padSynthEngine ? window.padSynthEngine[offTrack] : -1;
  if (offEngine >= 3) sendWS({cmd: 'synthNoteOff', track: offTrack, engine: offEngine});
  document.querySelectorAll('.pr-cell.pr-preview').forEach(c => c.classList.remove('pr-preview'));
  const pb1 = document.getElementById('melody-play-btn');
  const pb2 = document.getElementById('melodyTabPlay');
  if (pb1) pb1.textContent = '\u25b6 Preview';
  if (pb2) pb2.textContent = '\u25b6 Preview';
}

function playMelodyPreview(track) {
  if (_previewPlaying) { stopMelodyPreview(); return; }
  if (track < 0 || track >= 16) return;
  const slider = document.getElementById('tempoSlider');
  const bpm = slider ? parseFloat(slider.value) || 120 : 120;
  const stepMs = Math.round(60000 / bpm / 4);
  let hasNotes = false;
  for (let s = 0; s < 64; s++) { if (stepNotesData[track][s] > 0) { hasNotes = true; break; } }
  if (!hasNotes) {
    if (window.showToast) window.showToast('No notes to preview', 'info', 1000);
    return;
  }
  _previewPlaying = true;
  const pb1 = document.getElementById('melody-play-btn');
  const pb2 = document.getElementById('melodyTabPlay');
  if (pb1) pb1.textContent = '\u23f9 Stop';
  if (pb2) pb2.textContent = '\u23f9 Stop';
  let idx = 0;
  function playNext() {
    if (!_previewPlaying || idx >= 64) { stopMelodyPreview(); return; }
    const note = stepNotesData[track][idx];
    const flags = stepFlagsData[track][idx];
    const eng = window.padSynthEngine ? window.padSynthEngine[track] : -1;
    if (note > 0) {
      previewNote(track, note, !!(flags & 1), !!(flags & 2));
    } else if (eng >= 3) {
      sendWS({cmd: 'synthNoteOff', track, engine: eng});
    }
    document.querySelectorAll('.pr-cell.pr-preview').forEach(c => c.classList.remove('pr-preview'));
    document.querySelectorAll('.pr-cell[data-step="' + idx + '"]').forEach(c => c.classList.add('pr-preview'));
    idx++;
    _previewTimer = setTimeout(playNext, stepMs);
  }
  playNext();
}

// ═══════════════════════════════════════════════════════
// NOTE INDICATORS on step grid
// Shows mini note label on steps that have melody notes
// ═══════════════════════════════════════════════════════
function updateNoteIndicators(track) {
  for (let s = 0; s < 64; s++) {
    const el = document.querySelector(`[data-track="${track}"][data-step="${s}"]`);
    if (!el) continue;
    let badge = el.querySelector('.step-note-badge');
    const note = stepNotesData[track][s];
    const flags = stepFlagsData[track][s];
    if (note > 0 && isMelodicTrack(track)) {
      if (!badge) {
        badge = document.createElement('div');
        badge.className = 'step-note-badge';
        el.appendChild(badge);
      }
      let label = NOTE_NAMES[note % 12] + (note % 12 > 4 ? '' : '') ;
      badge.textContent = label;
      badge.classList.toggle('accent', !!(flags & 1));
      badge.classList.toggle('slide', !!(flags & 2));
    } else {
      if (badge) badge.remove();
    }
  }
}

function updateAllNoteIndicators() {
  for (let t = 0; t < 16; t++) {
    if (isMelodicTrack(t)) updateNoteIndicators(t);
  }
}

// ═══════════════════════════════════════════════════════
// MELODY EDITOR PANEL
// Full-width panel below sequencer grid with piano-roll style
// ═══════════════════════════════════════════════════════

function showMelodyEditor(track) {
  if (track < 0 || track >= 16) return;
  melodyEditorTrack = track;
  melodyEditorVisible = true;

  let panel = document.getElementById('melody-editor-panel');
  if (!panel) panel = createMelodyEditorPanel();

  const trackNames = ['BD','SD','CH','OH','CY','CP','RS','CB',
                      'LT','MT','HT','MA','CL','HC','MC','LC'];
  const engNames = ['808','909','505','303','WT','SH101','FM2Op'];
  const eng = window.padSynthEngine ? window.padSynthEngine[track] : -1;
  const engLabel = eng >= 0 && eng < engNames.length ? engNames[eng] : '?';

  panel.querySelector('#melody-editor-title').textContent =
      `🎹 Melody — Track ${track + 1} (${trackNames[track]}) — ${engLabel}`;

  renderPianoRoll(panel, track);
  renderPresetButtons(panel);

  // Show
  panel.style.display = 'block';
  requestAnimationFrame(() => panel.classList.add('visible'));
}

function hideMelodyEditor() {
  const panel = document.getElementById('melody-editor-panel');
  if (panel) {
    panel.classList.remove('visible');
    setTimeout(() => { panel.style.display = 'none'; }, 200);
  }
  melodyEditorVisible = false;
  melodyEditorTrack = -1;
}

function createMelodyEditorPanel() {
  const panel = document.createElement('div');
  panel.id = 'melody-editor-panel';
  panel.className = 'melody-editor-panel';
  panel.innerHTML = `
    <div class="melody-editor-header">
      <span id="melody-editor-title">🎹 Melody Editor</span>
      <div class="melody-editor-controls">
        <button id="melody-play-btn" class="melody-ctrl-btn" title="Preview melody">▶ Preview</button>
        <button id="melody-clear-btn" class="melody-ctrl-btn" title="Clear all notes">🗑️ Clear</button>
        <button id="melody-close-btn" class="melody-ctrl-btn" title="Close">✕</button>
      </div>
    </div>
    <div id="melody-presets-row" class="melody-presets-row"></div>
    <div id="melody-piano-roll" class="melody-piano-roll"></div>
    <div class="melody-editor-footer">
      <label class="melody-flag-label">
        <input type="checkbox" id="melody-accent-chk"> Accent
      </label>
      <label class="melody-flag-label">
        <input type="checkbox" id="melody-slide-chk"> Slide
      </label>
      <span class="melody-hint">Click step → pick note on piano | Right-click = rest</span>
    </div>
  `;
  document.body.appendChild(panel);

  panel.querySelector('#melody-close-btn').addEventListener('click', hideMelodyEditor);
  panel.querySelector('#melody-play-btn').addEventListener('click', () => {
    playMelodyPreview(melodyEditorTrack);
  });
  panel.querySelector('#melody-clear-btn').addEventListener('click', () => {
    if (melodyEditorTrack < 0) return;
    for (let s = 0; s < 64; s++) {
      stepNotesData[melodyEditorTrack][s] = 0;
      stepFlagsData[melodyEditorTrack][s] = 0;
      sendWS({cmd:'setStepNote', track: melodyEditorTrack, step: s, note: 0, flags: 0, silent: true});
    }
    updateNoteIndicators(melodyEditorTrack);
    renderPianoRoll(panel, melodyEditorTrack);
    if (window.showToast) window.showToast('Notes cleared', window.TOAST_TYPES?.INFO || 'info', 1000);
  });
  panel.addEventListener('click', e => e.stopPropagation());

  // Piano roll click handlers (event delegation — added once)
  const roll = panel.querySelector('#melody-piano-roll');
  roll.addEventListener('click', handlePianoRollClick);
  roll.addEventListener('contextmenu', handlePianoRollRightClick);

  // Preset click handler (event delegation — added once)
  panel.querySelector('#melody-presets-row').addEventListener('click', e => {
    const btn = e.target.closest('.melody-preset-btn');
    if (!btn) return;
    const idx = parseInt(btn.dataset.idx);
    if (idx >= 0 && idx < melodyPresets.length) {
      applyMelodyPreset(melodyPresets[idx]);
      const p = document.getElementById('melody-editor-panel');
      if (p) renderPianoRoll(p, melodyEditorTrack);
    }
  });

  return panel;
}

// ── Piano Roll Rendering ──
function renderPianoRoll(panel, track) {
  const container = panel.querySelector('#melody-piano-roll');
  const stepCount = window.currentStepCount || 16;

  // Build grid: rows = notes (high to low), cols = steps
  let html = '<div class="pr-grid">';

  // Header row: step numbers
  html += '<div class="pr-row pr-header-row"><div class="pr-label"></div>';
  for (let s = 0; s < stepCount; s++) {
    const beat = (s % 4 === 0) ? ' pr-beat' : '';
    html += `<div class="pr-cell pr-step-hdr${beat}">${s + 1}</div>`;
  }
  html += '</div>';

  // Note rows (from high to low)
  for (let n = PIANO_HIGH; n >= PIANO_LOW; n--) {
    const isBlack = [1,3,6,8,10].includes(n % 12);
    const isC = (n % 12 === 0);
    const rowClass = isBlack ? 'pr-row pr-black' : 'pr-row pr-white';
    const label = isC ? midiNoteName(n) : (isBlack ? '' : NOTE_NAMES[n % 12]);

    html += `<div class="${rowClass}${isC ? ' pr-octave' : ''}">`;
    html += `<div class="pr-label${isBlack ? ' pr-label-black' : ''}">${label}</div>`;

    for (let s = 0; s < stepCount; s++) {
      const active = (stepNotesData[track][s] === n);
      const flags = stepFlagsData[track][s];
      const beat = (s % 4 === 0) ? ' pr-beat' : '';
      const accent = (active && (flags & 1)) ? ' pr-accent' : '';
      const slide = (active && (flags & 2)) ? ' pr-slide' : '';
      html += `<div class="pr-cell${beat}${active ? ' pr-active' : ''}${accent}${slide}" `
            + `data-note="${n}" data-step="${s}"></div>`;
    }
    html += '</div>';
  }
  html += '</div>';
  container.innerHTML = html;
}

function handlePianoRollClick(e) {
  const cell = e.target.closest('.pr-cell[data-note]');
  if (!cell || melodyEditorTrack < 0) return;
  const note = parseInt(cell.dataset.note);
  const step = parseInt(cell.dataset.step);
  if (isNaN(note) || isNaN(step)) return;

  const track = melodyEditorTrack;
  const currentNote = stepNotesData[track][step];

  // Toggle: if same note clicked, remove it
  if (currentNote === note) {
    stepNotesData[track][step] = 0;
    stepFlagsData[track][step] = 0;
    sendWS({cmd:'setStepNote', track, step, note: 0, flags: 0});
  } else {
    // Set new note
    const accentChk = document.getElementById('melody-accent-chk');
    const slideChk = document.getElementById('melody-slide-chk');
    const flags = ((accentChk && accentChk.checked) ? 1 : 0) |
                  ((slideChk && slideChk.checked) ? 2 : 0);
    stepNotesData[track][step] = note;
    stepFlagsData[track][step] = flags;
    sendWS({cmd:'setStepNote', track, step, note, flags});
    // Preview the note sound
    previewNote(track, note, !!(flags & 1), !!(flags & 2));
  }

  // Re-render the piano roll column efficiently
  updatePianoRollColumn(step, track);
  updateNoteIndicators(track);
}

function handlePianoRollRightClick(e) {
  e.preventDefault();
  const cell = e.target.closest('.pr-cell[data-note]');
  if (!cell || melodyEditorTrack < 0) return;
  const step = parseInt(cell.dataset.step);
  if (isNaN(step)) return;
  const track = melodyEditorTrack;
  // Clear note (rest)
  stepNotesData[track][step] = 0;
  stepFlagsData[track][step] = 0;
  sendWS({cmd:'setStepNote', track, step, note: 0, flags: 0});
  updatePianoRollColumn(step, track);
  updateNoteIndicators(track);
}

function updatePianoRollColumn(step, track) {
  const container = document.getElementById('melody-piano-roll');
  if (!container) return;
  const cells = container.querySelectorAll(`.pr-cell[data-step="${step}"]`);
  cells.forEach(cell => {
    const n = parseInt(cell.dataset.note);
    if (isNaN(n)) return;
    const active = (stepNotesData[track][step] === n);
    const flags = stepFlagsData[track][step];
    cell.classList.toggle('pr-active', active);
    cell.classList.toggle('pr-accent', active && !!(flags & 1));
    cell.classList.toggle('pr-slide', active && !!(flags & 2));
  });
}

// ── Preset Buttons ──
function renderPresetButtons(panel) {
  const row = panel.querySelector('#melody-presets-row');
  if (!row) return;
  if (melodyPresets.length === 0) {
    row.innerHTML = '<span class="melody-preset-empty">No presets available</span>';
    return;
  }
  row.innerHTML = melodyPresets.map((p, i) =>
    `<button class="melody-preset-btn" data-idx="${i}" title="${p.name}">${p.name}</button>`
  ).join('');
}

// ═══════════════════════════════════════════════════════
// INTEGRATION: Hook into sequencer step clicks
// ═══════════════════════════════════════════════════════

// Override selectCell to also show melody editor for synth tracks
const _origSelectCell = window.selectCell;
window.selectCell = function(track, step) {
  if (_origSelectCell) _origSelectCell(track, step);
  // If melodic track and editor not visible, open it
  if (isMelodicTrack(track) && !melodyEditorVisible) {
    showMelodyEditor(track);
  } else if (melodyEditorVisible && track !== melodyEditorTrack) {
    if (isMelodicTrack(track)) {
      showMelodyEditor(track); // Switch track
    }
  }
};

// Handle pattern data load — store stepNotes and stepFlags
const _origLoadPatternData = window.loadPatternData;
window.loadPatternData = function(data) {
  if (_origLoadPatternData) _origLoadPatternData(data);
  // Load melody data
  if (data.stepNotes) {
    for (let t = 0; t < 16; t++) {
      const arr = data.stepNotes[t] || data.stepNotes[t.toString()];
      if (arr) arr.forEach((n, s) => { if (s < 64) stepNotesData[t][s] = n; });
    }
  }
  if (data.stepFlags) {
    for (let t = 0; t < 16; t++) {
      const arr = data.stepFlags[t] || data.stepFlags[t.toString()];
      if (arr) arr.forEach((f, s) => { if (s < 64) stepFlagsData[t][s] = f; });
    }
  }
  updateAllNoteIndicators();
};

// Handle WS message for stepNoteSet
const _origHandleWSMsg = window.handleWebSocketMessage;
if (typeof _origHandleWSMsg === 'function') {
  window.handleWebSocketMessage = function(data) {
    _origHandleWSMsg(data);
    if (data.type === 'stepNoteSet') {
      const t = data.track, s = data.step;
      if (t >= 0 && t < 16 && s >= 0 && s < 64) {
        stepNotesData[t][s] = data.note || 0;
        if (data.flags !== undefined) stepFlagsData[t][s] = data.flags;
        updateNoteIndicators(t);
        if (melodyEditorVisible && melodyEditorTrack === t) {
          updatePianoRollColumn(s, t);
        }
      }
    }
  };
}

// ═══════════════════════════════════════════════════════
// Add Melody button to track labels for synth tracks
// ═══════════════════════════════════════════════════════
function addMelodyButtons() {
  document.querySelectorAll('.track-label').forEach(label => {
    const trackAttr = label.closest('[data-track]');
    if (!trackAttr) return;
    const track = parseInt(trackAttr.dataset.track);
    if (isNaN(track)) return;
    // Remove existing button if any
    const existing = label.querySelector('.melody-open-btn');
    if (existing) existing.remove();
    if (isMelodicTrack(track)) {
      const btn = document.createElement('button');
      btn.className = 'melody-open-btn';
      btn.textContent = '🎹';
      btn.title = 'Open Melody Editor';
      btn.addEventListener('click', e => {
        e.stopPropagation();
        showMelodyEditor(track);
      });
      label.appendChild(btn);
    }
  });
}

// ── Add 🎹 button to PAD containers for melodic tracks ──
function addMelodyPadButtons() {
  document.querySelectorAll('.pad').forEach(pad => {
    const padIdx = parseInt(pad.dataset.pad);
    if (isNaN(padIdx)) return;
    // Remove existing
    const existing = pad.querySelector('.melody-pad-btn');
    if (existing) existing.remove();
    if (isMelodicTrack(padIdx)) {
      const btn = document.createElement('button');
      btn.className = 'melody-pad-btn';
      btn.textContent = '🎹';
      btn.title = 'Melody options';
      btn.addEventListener('click', e => {
        e.stopPropagation();
        e.preventDefault();
        showPadMelodyMenu(btn, padIdx);
      });
      btn.addEventListener('mousedown', e => e.stopPropagation());
      btn.addEventListener('touchstart', e => e.stopPropagation());
      pad.appendChild(btn);
    }
  });
}

// ── Mini popup: choose between Step Editor or Live Note assignment ──
function showPadMelodyMenu(anchorBtn, padIdx) {
  // Close any existing
  const old = document.getElementById('pad-melody-menu');
  if (old) { old.remove(); return; }

  const menu = document.createElement('div');
  menu.id = 'pad-melody-menu';
  menu.className = 'pad-melody-menu';
  menu.innerHTML = `
    <button class="pad-melody-menu-item" data-action="step">
      <span class="pmm-icon">🎼</span>
      <div class="pmm-text"><b>Step Editor</b><small>Edit notes per step</small></div>
    </button>
    <button class="pad-melody-menu-item" data-action="live">
      <span class="pmm-icon">🎹</span>
      <div class="pmm-text"><b>Live Note</b><small>Assign note to pad</small></div>
    </button>
  `;
  document.body.appendChild(menu);

  // Position next to button
  const rect = anchorBtn.getBoundingClientRect();
  const menuRect = menu.getBoundingClientRect();
  let left = rect.right + 6;
  let top = rect.top;
  if (left + menuRect.width > window.innerWidth) left = rect.left - menuRect.width - 6;
  if (top + menuRect.height > window.innerHeight) top = window.innerHeight - menuRect.height - 8;
  if (top < 8) top = 8;
  menu.style.left = left + 'px';
  menu.style.top = top + 'px';

  function close() {
    menu.remove();
    document.removeEventListener('click', onDocClick, true);
  }
  function onDocClick(e) {
    if (!menu.contains(e.target)) close();
  }
  setTimeout(() => document.addEventListener('click', onDocClick, true), 10);

  menu.addEventListener('click', e => {
    const btn = e.target.closest('.pad-melody-menu-item');
    if (!btn) return;
    const action = btn.dataset.action;
    close();
    if (action === 'step') {
      showMelodyEditor(padIdx);
    } else if (action === 'live') {
      // Switch to MELODY tab and select this pad
      const tabBtn = document.querySelector('.tab-btn[data-tab="melody"]');
      if (tabBtn) tabBtn.click();
      setTimeout(() => {
        const sel = document.getElementById('melodyTrackSelect');
        if (sel) {
          sel.value = String(padIdx);
          sel.dispatchEvent(new Event('change'));
        }
        // Pre-fill assign dropdown to this pad
        const assignSel = document.getElementById('melodyAssignPadSelect');
        if (assignSel) assignSel.value = String(padIdx);
        if (window.showToast) window.showToast(`Pad ${padIdx + 1}: toca una tecla y pulsa "Assign to Pad"`, 'info', 2200);
      }, 50);
    }
  });
}

// Watch for synth engine changes via callback (set from app.js)
window.onSynthEnginesRefreshed = function() {
  addMelodyButtons();
  addMelodyPadButtons();
  updatePadMelodyAssignmentBadges();
  updateAllNoteIndicators();
};

// Chain with synth-editor.js onSynthEngineChanged (creates overlays/waveforms)
// We must NOT overwrite it — save original and call both
const _origOnSynthEngineChanged = window.onSynthEngineChanged || (typeof onSynthEngineChanged === 'function' ? onSynthEngineChanged : null);
window.onSynthEngineChanged = function(padIndex, engine) {
  // Call synth-editor.js first (creates/removes waveform overlay + params button)
  if (_origOnSynthEngineChanged) _origOnSynthEngineChanged(padIndex, engine);
  // Then update melody UI
  setTimeout(() => {
    addMelodyButtons();
    addMelodyPadButtons();
    updatePadMelodyAssignmentBadges();
    updateAllNoteIndicators();
    refreshMelodyTabTrackSelect();
  }, 80);
};

// ═══════════════════════════════════════════════════════
// MELODY TAB — Full dedicated tab with track selector + piano roll
// ═══════════════════════════════════════════════════════

let melodyTabTrack = -1;
let melodyTabInited = false;

const TRACK_NAMES = ['BD','SD','CH','OH','CY','CP','RS','CB',
                     'LT','MT','HT','MA','CL','HC','MC','LC'];
const ENG_NAMES = ['808','909','505','303','WT','SH101','FM2Op'];

function initMelodyTab() {
  if (melodyTabInited) return;
  melodyTabInited = true;

  const sel = document.getElementById('melodyTrackSelect');
  if (!sel) return;

  // Populate track selector
  refreshMelodyTabTrackSelect();

  sel.addEventListener('change', () => {
    const t = parseInt(sel.value);
    if (!isNaN(t) && t >= 0 && t < 16) {
      melodyTabTrack = t;
      refreshMelodyEngineSelect();
      renderMelodyTab();
    } else {
      melodyTabTrack = -1;
      refreshMelodyEngineSelect();
      renderMelodyTab();
    }
  });

  // Engine selector — assign melodic engine to selected track
  const engSel = document.getElementById('melodyEngineSelect');
  if (engSel) {
    engSel.addEventListener('change', () => {
      if (melodyTabTrack < 0) return;
      const newEng = parseInt(engSel.value);
      if (newEng >= 3 && newEng <= 6) {
        const fn = window.setSynthEngineExact || setSynthEngineExact;
        fn(melodyTabTrack, newEng);
        if (window.showToast) window.showToast(`Track ${melodyTabTrack+1} → ${ENG_NAMES[newEng]}`, 'info', 1500);
        renderMelodyTab();
      }
    });
  }

  // Clear button
  const clearBtn = document.getElementById('melodyTabClear');
  if (clearBtn) {
    clearBtn.addEventListener('click', () => {
      if (melodyTabTrack < 0) return;
      for (let s = 0; s < 64; s++) {
        stepNotesData[melodyTabTrack][s] = 0;
        stepFlagsData[melodyTabTrack][s] = 0;
        sendWS({cmd:'setStepNote', track: melodyTabTrack, step: s, note: 0, flags: 0, silent: true});
      }
      updateNoteIndicators(melodyTabTrack);
      renderMelodyTab();
      if (window.showToast) window.showToast('Notes cleared', 'info', 1000);
    });
  }

  // Play/Pause button
  const playBtn = document.getElementById('melodyTabPlay');
  if (playBtn) {
    playBtn.addEventListener('click', () => {
      playMelodyPreview(melodyTabTrack);
    });
  }

  // Piano roll click delegation
  const roll = document.getElementById('melodyTabPianoRoll');
  if (roll) {
    roll.addEventListener('click', handleTabPianoClick);
    roll.addEventListener('contextmenu', handleTabPianoRightClick);
  }

  const livePiano = document.getElementById('melodyVisualPiano');
  if (livePiano) {
    livePiano.addEventListener('pointerdown', e => {
      const key = e.target.closest('.melody-piano-key[data-note]');
      if (!key) return;
      e.preventDefault();
      key.setPointerCapture?.(e.pointerId);
      key.classList.add('pressed');
      playLivePianoNote(parseInt(key.dataset.note));
    });
    livePiano.addEventListener('pointerup', e => {
      const key = e.target.closest('.melody-piano-key[data-note]');
      if (key) key.classList.remove('pressed');
      if (melodyTabTrack >= 0 && window.padSynthEngine) {
        const eng = window.padSynthEngine[melodyTabTrack];
        if (typeof eng === 'number' && eng >= 3) sendWS({cmd:'synthNoteOff', track: melodyTabTrack, engine: eng});
      }
    });
    livePiano.addEventListener('pointerleave', () => {
      livePiano.querySelectorAll('.pressed').forEach(k => k.classList.remove('pressed'));
    });
  }

  const recordBtn = document.getElementById('melodyLiveRecord');
  if (recordBtn) {
    recordBtn.addEventListener('click', () => {
      melodyRecordEnabled = !melodyRecordEnabled;
      recordBtn.classList.toggle('recording', melodyRecordEnabled);
      updateLivePianoStatus();
    });
  }

  const resetBtn = document.getElementById('melodyLiveReset');
  if (resetBtn) {
    resetBtn.addEventListener('click', () => {
      melodyRecordStep = 0;
      updateLivePianoStatus();
    });
  }

  const assignBtn = document.getElementById('melodyAssignPad');
  if (assignBtn) assignBtn.addEventListener('click', assignSelectedNoteToPad);

  const clearAssignBtn = document.getElementById('melodyClearPadAssign');
  if (clearAssignBtn) clearAssignBtn.addEventListener('click', clearSelectedPadAssignment);

  const panicBtn = document.getElementById('melodyPanicBtn');
  if (panicBtn) panicBtn.addEventListener('click', () => {
    if (typeof window.panicAllNotes === 'function') window.panicAllNotes();
  });

  // Preset row delegation
  const presetRow = document.getElementById('melodyTabPresets');
  if (presetRow) {
    presetRow.addEventListener('click', e => {
      const btn = e.target.closest('.melody-preset-btn');
      if (!btn) return;
      const idx = parseInt(btn.dataset.idx);
      if (idx >= 0 && idx < melodyPresets.length) {
        // Use tab track (not popup track)
        const saved = melodyEditorTrack;
        melodyEditorTrack = melodyTabTrack;
        applyMelodyPreset(melodyPresets[idx]);
        melodyEditorTrack = saved;
        renderMelodyTab();
      }
    });
  }

  renderVisualPiano();
  renderPadAssignSelect();
  updatePadMelodyAssignmentBadges();
}

function refreshMelodyTabTrackSelect() {
  const sel = document.getElementById('melodyTrackSelect');
  if (!sel) return;
  const prev = sel.value;
  sel.innerHTML = '<option value="-1">— Select Track —</option>';
  for (let t = 0; t < 16; t++) {
    const eng = window.padSynthEngine ? window.padSynthEngine[t] : -1;
    const engLabel = (eng >= 0 && eng < ENG_NAMES.length) ? ENG_NAMES[eng] : 'Sample';
    const isMelodic = eng >= 3;
    const opt = document.createElement('option');
    opt.value = t;
    opt.textContent = `${t+1}. ${TRACK_NAMES[t]} [${engLabel}]${isMelodic ? ' ♪' : ''}`;
    sel.appendChild(opt);
  }
  // Restore selection
  if (prev !== '' && prev !== '-1') sel.value = prev;
  // Sync engine selector
  refreshMelodyEngineSelect();
}

function refreshMelodyEngineSelect() {
  const engSel = document.getElementById('melodyEngineSelect');
  if (!engSel) return;
  if (melodyTabTrack < 0) {
    engSel.value = '-1';
    engSel.disabled = true;
    return;
  }

  renderPadAssignSelect();
  updateLivePianoStatus();
  const eng = window.padSynthEngine ? window.padSynthEngine[melodyTabTrack] : -1;
  engSel.disabled = false;
  // If current engine is one of the melodic options, select it
  if (eng >= 3 && eng <= 6) {
    engSel.value = String(eng);
  } else {
    engSel.value = '-1';
  }
}

function renderPadAssignSelect() {
  const sel = document.getElementById('melodyAssignPadSelect');
  if (!sel) return;
  const previous = sel.value;
  sel.innerHTML = '';
  for (let t = 0; t < 16; t++) {
    const assignment = padMelodyAssignments[t];
    const opt = document.createElement('option');
    opt.value = String(t);
    opt.textContent = `${t + 1}. ${TRACK_NAMES[t]}${assignment ? ' ' + midiNoteName(assignment.note) : ''}`;
    sel.appendChild(opt);
  }
  if (previous !== '') sel.value = previous;
  if ((sel.value === '' || sel.value === '-1') && melodyTabTrack >= 0) sel.value = String(melodyTabTrack);
}

function renderVisualPiano() {
  const piano = document.getElementById('melodyVisualPiano');
  if (!piano) return;

  const whiteNotes = [];
  for (let n = LIVE_PIANO_LOW; n <= LIVE_PIANO_HIGH; n++) {
    if (!isBlackNote(n)) whiteNotes.push(n);
  }

  let html = '<div class="melody-piano-white-row">';
  whiteNotes.forEach(note => {
    html += `<button class="melody-piano-key melody-piano-white${note === selectedLiveNote ? ' selected' : ''}" data-note="${note}">`
      + `<span>${midiNoteName(note)}</span></button>`;
  });
  html += '</div><div class="melody-piano-black-layer">';

  for (let n = LIVE_PIANO_LOW; n <= LIVE_PIANO_HIGH; n++) {
    if (!isBlackNote(n)) continue;
    const whiteBefore = whiteNotes.filter(w => w < n).length;
    const left = (whiteBefore / whiteNotes.length) * 100;
    html += `<button class="melody-piano-key melody-piano-black${n === selectedLiveNote ? ' selected' : ''}" data-note="${n}" style="left:${left}%">`
      + `<span>${NOTE_NAMES[n % 12]}</span></button>`;
  }
  html += '</div>';
  piano.innerHTML = html;
  updateLivePianoStatus();
}

function updateLivePianoStatus() {
  const status = document.getElementById('melodyLiveStatus');
  if (!status) return;
  const trackLabel = melodyTabTrack >= 0 ? `${melodyTabTrack + 1}. ${TRACK_NAMES[melodyTabTrack]}` : 'No track';
  const recLabel = melodyRecordEnabled ? `REC S${melodyRecordStep + 1}` : 'Live';
  const noteLabel = selectedLiveNote ? `🎵 ${midiNoteName(selectedLiveNote)}` : 'toca una tecla';
  status.textContent = `${recLabel} · ${trackLabel} · ${noteLabel}`;
  status.classList.toggle('has-note', !!selectedLiveNote);
}

function updateVisualPianoSelection() {
  document.querySelectorAll('#melodyVisualPiano .melody-piano-key').forEach(key => {
    key.classList.toggle('selected', parseInt(key.dataset.note) === selectedLiveNote);
  });
  updateLivePianoStatus();
}

function recordLiveNote(note) {
  if (!melodyRecordEnabled || melodyTabTrack < 0 || !isMelodicTrack(melodyTabTrack)) return;
  const stepCount = window.currentStepCount || 16;
  const step = melodyRecordStep % stepCount;
  const accentChk = document.getElementById('melodyTabAccent');
  const slideChk = document.getElementById('melodyTabSlide');
  const flags = ((accentChk && accentChk.checked) ? 1 : 0) |
                ((slideChk && slideChk.checked) ? 2 : 0);

  stepNotesData[melodyTabTrack][step] = note;
  stepFlagsData[melodyTabTrack][step] = flags;
  sendWS({cmd:'setStepNote', track: melodyTabTrack, step, note, flags});
  sendWS({cmd:'setStep', track: melodyTabTrack, step, active: true, noteLen: 1});

  const seqStep = document.querySelector(`.seq-step[data-track="${melodyTabTrack}"][data-step="${step}"]`);
  if (seqStep) seqStep.classList.add('active');

  updateTabPianoColumn(step);
  updateNoteIndicators(melodyTabTrack);
  melodyRecordStep = (step + 1) % stepCount;
  updateLivePianoStatus();
}

function playLivePianoNote(note) {
  selectedLiveNote = note;
  updateVisualPianoSelection();
  if (melodyTabTrack < 0) return;
  if (!isMelodicTrack(melodyTabTrack)) return;
  const accentChk = document.getElementById('melodyTabAccent');
  const slideChk = document.getElementById('melodyTabSlide');
  previewNote(melodyTabTrack, note, !!(accentChk && accentChk.checked), !!(slideChk && slideChk.checked));
  recordLiveNote(note);
}

function assignSelectedNoteToPad() {
  const sel = document.getElementById('melodyAssignPadSelect');
  const pad = sel ? parseInt(sel.value) : melodyTabTrack;
  if (isNaN(pad) || pad < 0 || pad >= 16) return;

  let engine = window.padSynthEngine ? window.padSynthEngine[pad] : -1;
  if (!isMelodicTrack(pad)) {
    const sourceEngine = melodyTabTrack >= 0 && window.padSynthEngine ? window.padSynthEngine[melodyTabTrack] : -1;
    engine = sourceEngine >= 3 ? sourceEngine : 4;
    const fn = window.setSynthEngineExact || window.setSynthEngine;
    if (typeof fn === 'function') fn(pad, engine);
  }

  const accentChk = document.getElementById('melodyTabAccent');
  const slideChk = document.getElementById('melodyTabSlide');
  padMelodyAssignments[pad] = {
    note: selectedLiveNote,
    accent: !!(accentChk && accentChk.checked),
    slide: !!(slideChk && slideChk.checked),
    velocity: 127
  };
  savePadMelodyAssignments();
  renderPadAssignSelect();
  updatePadMelodyAssignmentBadges();
  if (window.showToast) window.showToast(`Pad ${pad + 1} -> ${midiNoteName(selectedLiveNote)}`, 'success', 1400);
}

function clearSelectedPadAssignment() {
  const sel = document.getElementById('melodyAssignPadSelect');
  const pad = sel ? parseInt(sel.value) : melodyTabTrack;
  if (isNaN(pad) || pad < 0 || pad >= 16) return;
  padMelodyAssignments[pad] = null;
  savePadMelodyAssignments();
  renderPadAssignSelect();
  updatePadMelodyAssignmentBadges();
}

function updatePadMelodyAssignmentBadges() {
  document.querySelectorAll('.pad').forEach(padEl => {
    const pad = parseInt(padEl.dataset.pad);
    if (isNaN(pad) || pad < 0 || pad >= 16) return;
    let badge = padEl.querySelector('.pad-melody-assignment');
    const assignment = padMelodyAssignments[pad];
    if (!assignment) {
      if (badge) badge.remove();
      return;
    }
    if (!badge) {
      badge = document.createElement('span');
      badge.className = 'pad-melody-assignment';
      padEl.appendChild(badge);
    }
    badge.textContent = midiNoteName(assignment.note);
  });
}

function renderMelodyTab() {
  const roll = document.getElementById('melodyTabPianoRoll');
  const badge = document.getElementById('melodyTrackEngine');
  const presetRow = document.getElementById('melodyTabPresets');
  if (!roll) return;

  if (melodyTabTrack < 0) {
    roll.innerHTML = '<div style="color:#666;text-align:center;padding:40px;">Select a track above to edit its melody</div>';
    if (badge) { badge.textContent = '—'; badge.style.color = '#888'; }
    return;
  }

  const t = melodyTabTrack;
  const eng = window.padSynthEngine ? window.padSynthEngine[t] : -1;
  const engLabel = (eng >= 0 && eng < ENG_NAMES.length) ? ENG_NAMES[eng] : 'Sample';
  const isMelodic = eng >= 3;
  if (badge) {
    badge.textContent = isMelodic ? ('🎹 ' + engLabel) : engLabel;
    badge.style.color = isMelodic ? '#ff4444' : '#888';
  }

  if (!isMelodic) {
    roll.innerHTML = '<div style="color:#ff8800;text-align:center;padding:40px;font-size:1.1em;">'
      + '⚠️ Track ' + (t+1) + ' (' + TRACK_NAMES[t] + ') uses <b>' + engLabel + '</b> — not a melodic engine.<br><br>'
      + 'Use the <b>Engine</b> selector above to assign a melodic synth (303, WT, SH-101, FM 2-Op).</div>';
    if (presetRow) presetRow.innerHTML = '';
    return;
  }

  // Render presets
  if (presetRow) {
    if (melodyPresets.length === 0) {
      presetRow.innerHTML = '<span class="melody-preset-empty">No presets</span>';
    } else {
      presetRow.innerHTML = melodyPresets.map((p, i) =>
        `<button class="melody-preset-btn" data-idx="${i}">${p.name}</button>`
      ).join('');
    }
  }

  // Render piano roll
  const stepCount = window.currentStepCount || 16;
  let html = '<div class="pr-grid">';

  // Header row: step numbers
  html += '<div class="pr-row pr-header-row"><div class="pr-label"></div>';
  for (let s = 0; s < stepCount; s++) {
    const beat = (s % 4 === 0) ? ' pr-beat' : '';
    html += `<div class="pr-cell pr-step-hdr${beat}">${s + 1}</div>`;
  }
  html += '</div>';

  // Note rows (from high to low)
  for (let n = PIANO_HIGH; n >= PIANO_LOW; n--) {
    const isBlack = [1,3,6,8,10].includes(n % 12);
    const isC = (n % 12 === 0);
    const rowClass = isBlack ? 'pr-row pr-black' : 'pr-row pr-white';
    const label = isC ? midiNoteName(n) : (isBlack ? '' : NOTE_NAMES[n % 12]);

    html += `<div class="${rowClass}${isC ? ' pr-octave' : ''}">`;
    html += `<div class="pr-label${isBlack ? ' pr-label-black' : ''}">${label}</div>`;

    for (let s = 0; s < stepCount; s++) {
      const active = (stepNotesData[t][s] === n);
      const flags = stepFlagsData[t][s];
      const beat = (s % 4 === 0) ? ' pr-beat' : '';
      const accent = (active && (flags & 1)) ? ' pr-accent' : '';
      const slide = (active && (flags & 2)) ? ' pr-slide' : '';
      html += `<div class="pr-cell${beat}${active ? ' pr-active' : ''}${accent}${slide}" `
            + `data-note="${n}" data-step="${s}"></div>`;
    }
    html += '</div>';
  }
  html += '</div>';
  roll.innerHTML = html;
}

function handleTabPianoClick(e) {
  const cell = e.target.closest('.pr-cell[data-note]');
  if (!cell || melodyTabTrack < 0) return;
  const note = parseInt(cell.dataset.note);
  const step = parseInt(cell.dataset.step);
  if (isNaN(note) || isNaN(step)) return;
  const t = melodyTabTrack;
  const currentNote = stepNotesData[t][step];

  if (currentNote === note) {
    // Toggle off
    stepNotesData[t][step] = 0;
    stepFlagsData[t][step] = 0;
    sendWS({cmd:'setStepNote', track: t, step, note: 0, flags: 0});
  } else {
    // Set note
    const accentChk = document.getElementById('melodyTabAccent');
    const slideChk = document.getElementById('melodyTabSlide');
    const flags = ((accentChk && accentChk.checked) ? 1 : 0) |
                  ((slideChk && slideChk.checked) ? 2 : 0);
    stepNotesData[t][step] = note;
    stepFlagsData[t][step] = flags;
    sendWS({cmd:'setStepNote', track: t, step, note, flags});
    // Preview the note sound
    previewNote(t, note, !!(flags & 1), !!(flags & 2));
  }
  // Update column visuals
  updateTabPianoColumn(step);
  updateNoteIndicators(t);
}

function handleTabPianoRightClick(e) {
  e.preventDefault();
  const cell = e.target.closest('.pr-cell[data-note]');
  if (!cell || melodyTabTrack < 0) return;
  const step = parseInt(cell.dataset.step);
  if (isNaN(step)) return;
  stepNotesData[melodyTabTrack][step] = 0;
  stepFlagsData[melodyTabTrack][step] = 0;
  sendWS({cmd:'setStepNote', track: melodyTabTrack, step, note: 0, flags: 0});
  updateTabPianoColumn(step);
  updateNoteIndicators(melodyTabTrack);
}

function updateTabPianoColumn(step) {
  const roll = document.getElementById('melodyTabPianoRoll');
  if (!roll || melodyTabTrack < 0) return;
  const t = melodyTabTrack;
  roll.querySelectorAll(`.pr-cell[data-step="${step}"]`).forEach(cell => {
    const n = parseInt(cell.dataset.note);
    if (isNaN(n)) return;
    const active = (stepNotesData[t][step] === n);
    const flags = stepFlagsData[t][step];
    cell.classList.toggle('pr-active', active);
    cell.classList.toggle('pr-accent', active && !!(flags & 1));
    cell.classList.toggle('pr-slide', active && !!(flags & 2));
  });
}

// ═══════════════════════════════════════════════════════
// INIT
// ═══════════════════════════════════════════════════════
function initMelodyEditor() {
  loadMelodyPresets();
  initMelodyTab();
  // Initial run — engines may already be set
  setTimeout(() => {
    addMelodyButtons();
    addMelodyPadButtons();
    updatePadMelodyAssignmentBadges();
    updateAllNoteIndicators();
    refreshMelodyTabTrackSelect();
  }, 500);
  // Safety: re-check after WS likely delivered engine state
  setTimeout(() => {
    addMelodyButtons();
    addMelodyPadButtons();
    updatePadMelodyAssignmentBadges();
    updateAllNoteIndicators();
    refreshMelodyTabTrackSelect();
  }, 2000);
}

// ═══════════════════════════════════════════════════════
// PC keyboard → Visual Piano (only when MELODY tab is active)
// Tracker-style two-row layout (Renoise/FL):
//   Lower octave: Z S X D C V G B H N J M  → C C# D D# E F F# G G# A A# B
//   Upper octave: Q 2 W 3 E R 5 T 6 Y 7 U  → C C# D D# E F F# G G# A A# B
//   Top:          I → C (next octave)
//   ←/→ : octave shift, ↓/↑ : semitone shift, Space passthrough
// Capture phase: intercepts BEFORE keyboard-controls.js so existing letter
// shortcuts are suppressed while the Melody tab is open. Hold Shift to
// fall back to the original action (Q-Y patterns, A/S vol, M color, …).
// ═══════════════════════════════════════════════════════
const PIANO_KEY_MAP = {
  'z':0,  's':1,  'x':2,  'd':3,  'c':4,  'v':5,
  'g':6,  'b':7,  'h':8,  'n':9,  'j':10, 'm':11,
  'q':12, '2':13, 'w':14, '3':15, 'e':16, 'r':17,
  '5':18, 't':19, '6':20, 'y':21, '7':22, 'u':23,
  'i':24
};
let pianoBaseOctave = 4; // base = octave * 12 → 48 (C3) by default
const _pianoHeldKeys = new Set();

function isMelodyTabActive() {
  const btn = document.querySelector('.tab-btn[data-tab="melody"]');
  if (btn && btn.classList.contains('active')) return true;
  // Also accept popup editor visible
  return !!melodyEditorVisible;
}

function sendSynthNoteOff() {
  if (melodyTabTrack < 0 || !window.padSynthEngine) return;
  const eng = window.padSynthEngine[melodyTabTrack];
  if (typeof eng === 'number' && eng >= 3) {
    sendWS({cmd:'synthNoteOff', track: melodyTabTrack, engine: eng});
  }
}

document.addEventListener('keydown', e => {
  if (!isMelodyTabActive()) return;
  const t = e.target;
  if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
  if (e.ctrlKey || e.altKey || e.metaKey) return;

  // Octave shift (no shift)
  if (!e.shiftKey && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
    if (e.repeat) { e.preventDefault(); e.stopImmediatePropagation(); return; }
    pianoBaseOctave = Math.max(0, Math.min(8, pianoBaseOctave + (e.key === 'ArrowRight' ? 1 : -1)));
    if (window.showToast) window.showToast(`🎹 Octave base: C${pianoBaseOctave - 1}`, 'info', 900);
    e.preventDefault(); e.stopImmediatePropagation();
    return;
  }

  // Shift+key → let the global keyboard-controls handler run (patterns, vol, etc.)
  if (e.shiftKey) return;

  const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
  if (!(k in PIANO_KEY_MAP)) return;

  e.preventDefault();
  e.stopImmediatePropagation();
  if (e.repeat || _pianoHeldKeys.has(k)) return;
  _pianoHeldKeys.add(k);

  const note = PIANO_KEY_MAP[k] + pianoBaseOctave * 12;
  if (note < 0 || note > 127) return;
  playLivePianoNote(note);
}, true);

document.addEventListener('keyup', e => {
  const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
  if (!_pianoHeldKeys.has(k)) return;
  _pianoHeldKeys.delete(k);
  if (_pianoHeldKeys.size === 0) sendSynthNoteOff();
}, true);

// Stop any held synth note when the tab loses focus or browser blurs
window.addEventListener('blur', () => {
  if (_pianoHeldKeys.size > 0) {
    _pianoHeldKeys.clear();
    sendSynthNoteOff();
  }
});

// Export
window.initMelodyEditor = initMelodyEditor;
window.showMelodyEditor = showMelodyEditor;
window.hideMelodyEditor = hideMelodyEditor;
window.stepNotesData = stepNotesData;
window.stepFlagsData = stepFlagsData;
window.updateAllNoteIndicators = updateAllNoteIndicators;
window.getPadMelodyAssignment = function(pad) {
  return pad >= 0 && pad < 16 ? padMelodyAssignments[pad] : null;
};
window.updatePadMelodyAssignmentBadges = updatePadMelodyAssignmentBadges;

})();
