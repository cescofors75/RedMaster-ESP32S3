import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');
const firmwareSource = () => [
  read('src/WebInterface.cpp'),
  read('src/web/WebUploadPipeline.inc'),
].join('\n');

function scanWavChunks(buffer) {
  if (buffer.length < 12 || buffer.toString('ascii', 0, 4) !== 'RIFF' ||
      buffer.toString('ascii', 8, 12) !== 'WAVE') return false;
  let pos = 12;
  let fmt = false;
  while (pos <= buffer.length && buffer.length - pos >= 8) {
    const id = buffer.toString('ascii', pos, pos + 4);
    const size = buffer.readUInt32LE(pos + 4);
    const padded = size + (size & 1);
    if (!Number.isSafeInteger(padded) || padded > buffer.length - pos - 8) return false;
    if (id === 'fmt ') {
      if (size < 16) return false;
      fmt = true;
    } else if (id === 'data') {
      return fmt && size > 0;
    }
    pos += 8 + padded;
  }
  return false;
}

function validWav(samples = 32) {
  const dataSize = samples * 2;
  const out = Buffer.alloc(44 + dataSize);
  out.write('RIFF', 0); out.writeUInt32LE(out.length - 8, 4); out.write('WAVE', 8);
  out.write('fmt ', 12); out.writeUInt32LE(16, 16); out.writeUInt16LE(1, 20);
  out.writeUInt16LE(1, 22); out.writeUInt32LE(44100, 24); out.writeUInt32LE(88200, 28);
  out.writeUInt16LE(2, 32); out.writeUInt16LE(16, 34);
  out.write('data', 36); out.writeUInt32LE(dataSize, 40);
  return out;
}

function acceptsWsFrame(opcode, length) {
  return opcode === 'text' ? length <= 24_576 : length <= 256;
}

class UploadOwnerGate {
  owner = null;
  begin(request) {
    if (this.owner && this.owner !== request) return false;
    this.owner = request;
    return true;
  }
  finish(request) {
    if (this.owner !== request) return false;
    this.owner = null;
    return true;
  }
}

test('WAV scanner accepts a valid PCM file', () => {
  assert.equal(scanWavChunks(validWav()), true);
});

test('WAV fuzz never loops or accepts overflowing chunks', () => {
  for (let i = 0; i < 20_000; i++) {
    const size = 12 + Math.floor(Math.random() * 512);
    const input = Buffer.alloc(size);
    input.write('RIFF', 0); input.write('WAVE', 8);
    for (let p = 12; p + 8 <= size; p += 8) input.writeUInt32LE((Math.random() * 0x100000000) >>> 0, p + 4);
    assert.doesNotThrow(() => scanWavChunks(input));
  }
  const overflow = validWav();
  overflow.writeUInt32LE(0xfffffff0, 16);
  assert.equal(scanWavChunks(overflow), false);
});

test('all literal frontend commands have a firmware handler', () => {
  const webDir = path.join(root, 'data', 'web');
  const sent = new Set();
  for (const file of fs.readdirSync(webDir).filter((name) => /\.(js|html)$/.test(name))) {
    const source = fs.readFileSync(path.join(webDir, file), 'utf8');
    for (const match of source.matchAll(/cmd\s*:\s*['"]([^'"]+)['"]/g)) sent.add(match[1]);
  }
  const firmware = firmwareSource();
  const handled = new Set(['setBulk']);
  for (const match of firmware.matchAll(/cmd\s*==\s*"([^"]+)"/g)) handled.add(match[1]);
  assert.deepEqual([...sent].filter((cmd) => !handled.has(cmd)).sort(), []);
});

test('large Song Chain uses PSRAM JSON and fits public limit', () => {
  const chain = Array.from({ length: 32 }, (_, pattern) => ({ pattern, repeats: 4 }));
  assert.ok(Buffer.byteLength(JSON.stringify({ cmd: 'songChainUpload', chain })) > 512);
  const firmware = firmwareSource();
  assert.match(firmware, /PsramJsonDocument doc\(4096\)/);
  assert.match(firmware, /SONG_CHAIN_MAX/);
});

test('large WebSocket frame fuzz enforces text and binary limits', () => {
  assert.equal(acceptsWsFrame('text', 24_576), true);
  assert.equal(acceptsWsFrame('text', 24_577), false);
  assert.equal(acceptsWsFrame('binary', 256), true);
  assert.equal(acceptsWsFrame('binary', 257), false);
  for (let i = 0; i < 20_000; i++) {
    const opcode = Math.random() < 0.8 ? 'text' : 'binary';
    const length = Math.floor(Math.random() * 100_000);
    const limit = opcode === 'text' ? 24_576 : 256;
    assert.equal(acceptsWsFrame(opcode, length), length <= limit);
  }
  const firmware = firmwareSource();
  assert.match(firmware, /kWsMaxTextBytes = 24576/);
  assert.match(firmware, /kWsMaxBinaryBytes = 256/);
  assert.match(firmware, /frame_too_large/);
});

test('upload callbacks reject concurrent owners and contain no wait loop', () => {
  const firmware = firmwareSource();
  const start = firmware.indexOf('void WebInterface::handleDaisyUpload');
  const end = firmware.indexOf('// ── pumpCleanTrackStream', start);
  const handler = firmware.slice(start, end);
  assert.match(handler, /upload_busy/);
  assert.doesNotMatch(handler, /\bdelay\s*\(|vTaskDelay\s*\(|while\s*\(/);
  assert.match(handler, /owner != request/);
});

test('two-upload interleaving fuzz never transfers ownership silently', () => {
  for (let run = 0; run < 10_000; run++) {
    const gate = new UploadOwnerGate();
    const first = Math.random() < 0.5 ? 'A' : 'B';
    const second = first === 'A' ? 'B' : 'A';
    assert.equal(gate.begin(first), true);
    assert.equal(gate.begin(second), false);
    assert.equal(gate.finish(second), false);
    assert.equal(gate.owner, first);
    assert.equal(gate.finish(first), true);
    assert.equal(gate.begin(second), true);
  }
});

test('commands and binary frames always produce ACK or structured error', () => {
  const firmware = firmwareSource();
  assert.match(firmware, /webSendCommandStatus\(client, "ack", cmd\.c_str\(\)\)/);
  assert.match(firmware, /"unknown_command"/);
  assert.match(firmware, /"invalid_json"/);
  assert.match(firmware, /webSendCommandStatus\(client, "ack", "triggerBinary"\)/);
  assert.match(firmware, /"bad_frame"/);
});

test('web sources have no mojibake, duplicate static ids or dummy favicon', () => {
  const webDir = path.join(root, 'data', 'web');
  for (const file of fs.readdirSync(webDir).filter((name) => /\.(js|html|css)$/.test(name))) {
    const source = fs.readFileSync(path.join(webDir, file), 'utf8');
    assert.doesNotMatch(source, /Ô|├|┬|�/, file);
    if (file.endsWith('.html')) {
      const ids = [...source.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);
      assert.equal(new Set(ids).size, ids.length, `${file}: duplicate id`);
    }
  }
  assert.notEqual(read('data/web/favicon.ico').trim(), 'dummy');
});

test('touch navigation is raised above performance controls', () => {
  const nav = read('data/web/nav.js');
  assert.match(nav, /r808nav--touch/);
  assert.match(nav, /bottom:72px/);
});

test('Patchbay reflects Daisy track slots and only activates complete paths', () => {
  const html = read('data/web/patchbay.html');
  const source = read('data/web/patchbay.js');
  assert.doesNotMatch(html, /pbAddEffect\('phaser'\)/);
  assert.doesNotMatch(html, /pbAddEffect\('echo'\)/);
  assert.match(source, /delay:'echo'/);
  assert.match(source, /lowpass:'filter'/);
  assert.match(source, /hasPath\(next\.id, 'master'\)/);
  assert.match(source, /reconcileAllTrackRoutes/);

  const sharedStart = source.indexOf('function updateSharedFxState');
  const sharedEnd = source.indexOf('function saveFirmwareFiltersToShared', sharedStart);
  const sharedBlock = source.slice(sharedStart, sharedEnd);
  assert.match(sharedBlock, /getTrackRouteWinners\(track\)/);
  assert.match(sharedBlock, /winners\.forEach/);
  assert.doesNotMatch(sharedBlock, /cables\.forEach/);

  const clearStart = source.indexOf('function clearFxFromTrack');
  const clearEnd = source.indexOf('/* ──────────── SYNC:', clearStart);
  const clearBlock = source.slice(clearStart, clearEnd);
  assert.doesNotMatch(clearBlock, /clearTrackFX/);
  assert.match(clearBlock, /setTrackBitCrush/);
  assert.match(clearBlock, /setTrackDistortion/);
});

test('Patchbay slot winner fuzz is deterministic: downstream node wins', () => {
  const slotFor = (type) => ({
    lowpass: 'filter', highpass: 'filter', bandpass: 'filter',
    delay: 'echo', echo: 'echo', distortion: 'distortion',
    bitcrusher: 'bitcrusher', flanger: 'flanger', compressor: 'compressor',
  })[type];
  const types = ['lowpass', 'highpass', 'bandpass', 'delay', 'echo',
    'distortion', 'bitcrusher', 'flanger', 'compressor'];
  for (let run = 0; run < 10_000; run++) {
    const chain = Array.from({ length: 1 + Math.floor(Math.random() * 20) },
      (_, depth) => ({ id: `n${depth}`, depth, type: types[Math.floor(Math.random() * types.length)] }));
    const winners = new Map();
    chain.sort((a, b) => a.depth - b.depth).forEach(node => winners.set(slotFor(node.type), node));
    winners.forEach((winner, slot) => {
      const sameSlot = chain.filter(node => slotFor(node.type) === slot);
      assert.equal(winner.id, sameSlot[sameSlot.length - 1].id);
    });
  }
});

test('professional bank exposes 16 named resident patterns with performance metadata', () => {
  const bank = read('src/PatternBank.cpp');
  const header = read('src/PatternBank.h');
  const expected = [
    'Pulse Bloom', 'Night Transit', 'Glass Garage', 'Warm Pressure',
    'Carbon Breaks', 'Neon Motorik', 'Elastic Electro', 'Low Gravity',
    'Acid Trace', 'Sub Signal', 'Pocket Chrome', 'Organ Dust',
    'Night Bus', 'Afterglow', 'Peak Relay', 'Machine Ritual',
  ];
  assert.match(header, /BUILTIN_PATTERN_COUNT\s*=\s*16/);
  expected.forEach((name) => assert.match(bank, new RegExp(name.replace(/[\/]/g, '\\/'))));
  assert.match(bank, /recommendedBpm/);
  assert.match(bank, /setStepProbability/);
  assert.match(bank, /setStepRatchet/);
  assert.match(bank, /setStepCutoffLock/);
});

test('professional bank is sample-first and preserves expressive steps on Daisy', () => {
  const bank = read('src/PatternBank.cpp');
  const sequencer = read('src/Sequencer.cpp');
  const master = read('src/main.cpp');
  const protocol = read('src/protocol.h');
  const daisy = read('../RedMaster_DaisySeed64MB/DaisySeed/main.cpp');

  assert.match(bank, /ENGINE_PROFILE\[BUILTIN_PATTERN_COUNT\]\[MAX_TRACKS\]/);
  const profile = bank.slice(bank.indexOf('constexpr int8_t ENGINE_PROFILE'), bank.indexOf('constexpr uint8_t PRESET_PROFILE'));
  const rows = profile.split('\n').filter((line) => /^\s*\{(?:SMP|E\w+)/.test(line));
  assert.equal(rows.length, 16);
  rows.forEach((row) => assert.ok((row.match(/\bSMP\b/g) || []).length >= 12));
  for (const engine of ['E808', 'E909', 'E505', 'E303', 'EWT', 'ESH', 'EFM', 'ENOISE'])
    assert.match(bank, new RegExp(`\\b${engine}\\b`));
  assert.match(sequencer, /out\[s\]\.ratchet\s*=\s*pd->ratchets/);
  assert.match(sequencer, /out\[s\]\.noteVoices/);
  assert.match(master, /\(ratchet - 1\) << 4/);
  assert.match(master, /dsqSetStepNotes/);
  assert.match(master, /sequencer\.setStepCallback\(nullptr\)/);
  assert.match(protocol, /CMD_DSQ_SET_STEP_NOTES\s+0xDE/);
  assert.match(daisy, /dst\.ratchet\s*=\s*\(\(sp\[i\]\.noteLenDiv >> 4\)/);
  assert.match(daisy, /DsqProcessPendingTriggers/);
  assert.match(daisy, /DsqProcessHeldNotes/);
});

test('Daisy groove keeps swing pairs stable and Showcase has one sample-accurate transport', () => {
  const daisy = read('../RedMaster_DaisySeed64MB/DaisySeed/main.cpp');
  const makefile = read('../RedMaster_DaisySeed64MB/DaisySeed/Makefile');
  assert.match(daisy, /currentStep & 1\) == 0\) thr = dseq\.samplesPerStep \+ offset/);
  assert.match(daisy, /else thr = dseq\.samplesPerStep > offset \? dseq\.samplesPerStep - offset/);
  assert.match(daisy, /RunStartupShowcaseDemo/);
  assert.match(daisy, /BuildStartupShowcaseProgram/);
  assert.match(daisy, /ShowcaseBlocksMasterCommand/);
  assert.match(daisy, /ShowcaseHit\(0, SHOW_BD/);
  assert.match(daisy, /songPlaying && patternWrapped/);
  assert.match(daisy, /if\(kStartupShowcaseDemo\)\{/);
  assert.match(daisy, /if\(ShowcaseBlocksMasterCommand\(hdr->cmd\)\) return/);
  assert.match(daisy, /cleanTrackEnabled\[track\] = false/);
  assert.doesNotMatch(daisy, /static uint16_t songStep/);
  assert.match(makefile, /RED808_STARTUP_SHOWCASE_DEMO \?= 0/);
});

test('Master pattern indexes are resolved through the Daisy resident cache', () => {
  const main = read('src/main.cpp');
  const web = firmwareSource();
  const deferredStart = main.indexOf('void dsqUploadPatternDeferred');
  const uploadStart = main.indexOf('static void dsqUploadPatternToSlot');
  assert.ok(deferredStart >= 0 && uploadStart > deferredStart);
  assert.doesNotMatch(main.slice(deferredStart, uploadStart), /%=\s*DSQ_PATTERNS|%\s*DSQ_PATTERNS/);
  assert.match(main, /ensurePatternResident/);
  assert.match(main, /_daisySlotMasterPattern/);
  assert.match(web, /dsqGetResidentSlot/);
  assert.doesNotMatch(read('data/web/mobile.js'), /applyDemoPattern/);
});

test('pattern-bank JSON import preserves advanced step data and metadata', () => {
  const firmware = firmwareSource();
  for (const field of ['noteLenDivs', 'probabilities', 'ratchets', 'notes', 'noteVoices',
    'flags', 'volumeLocks', 'cutoffLocks', 'reverbLocks', 'recommendedBpm',
    'humanizeTimingMs', 'humanizeVelocity']) {
    assert.match(firmware, new RegExp(`\\["${field}"\\]`));
  }
  assert.match(firmware, /PsramJsonDocument doc\(131072\)/);
});

test('workspace UI keeps core workflows and exposes PATH while secondary tools stay progressive', () => {
  const html = read('data/web/index.html');
  const shell = read('data/web/workspace-ui.js');
  const css = read('data/web/workspace-2026.css');
  const navStart = html.indexOf('<nav class="tab-navigation workspace-navigation"');
  const navEnd = html.indexOf('</nav>', navStart);
  const nav = html.slice(navStart, navEnd);
  const primaryStart = nav.indexOf('<div class="workspace-primary-tabs"');
  const primaryEnd = nav.indexOf('</div>', primaryStart);
  const primary = nav.slice(primaryStart, primaryEnd);

  assert.equal((primary.match(/class="tab-btn/g) || []).length, 5);
  assert.match(primary,
    /class="tab-btn workspace-path-link"[^>]*(?:href="\/patchbay"|window\.location\.href='\/patchbay')/);
  assert.doesNotMatch(nav, /href="\/multiview/);
  assert.match(nav, /data-tab="xtra-pads"/);
  assert.match(nav, /data-tab="stems"/);
  assert.doesNotMatch(html, /id="headerStemUploadBtn"/);
  assert.doesNotMatch(html, /id="advancedModeBtn"/);
  assert.match(shell, /ArrowLeft/);
  assert.match(shell, /event\.key === 'Escape'/);
  assert.match(css, /\.workspace-focus-mode/);
  assert.match(css, /\.pad\.triggered[\s\S]*opacity:\s*1\s*!important/);
  assert.match(css, /prefers-reduced-motion/);
  assert.match(css, /body\.embed-mode \.workspace-overview\s*\{\s*display:\s*none\s*!important/);
  assert.match(html, /href="studio-workspaces\.css"/);
  assert.match(html, /id="pathBtn"[\s\S]*href="\/patchbay"/);
  assert.match(html, /id="sequencerRuler"/);
  assert.match(read('data/web/app.js'), /function renderSequencerRuler\(stepCount\)/);
  assert.match(read('data/web/app.js'), /className = 'track-identity'/);
  assert.match(read('data/web/app.js'), /className = 'track-actions'/);
  assert.match(read('data/web/studio-workspaces.css'), /\.sequencer-ruler/);
  const firmware = firmwareSource();
  assert.match(firmware, /server->on\("\/workspace-2026\.css"/);
  assert.match(firmware, /server->on\("\/studio-workspaces\.css"/);
  assert.match(firmware, /server->on\("\/workspace-ui\.js"/);
});

test('desktop and mobile share the six BlueSlaveP4-compatible visual themes', () => {
  const html = read('data/web/index.html');
  const tokens = read('data/web/theme-vars.css');
  const shell = read('data/web/workspace-ui.js');
  const mobile = read('data/web/mobile.js');
  const nav = read('data/web/nav.js');
  const ids = ['red808', 'ocean', 'neon', 'sunset', 'rainbow', 'greyscale'];

  assert.equal((html.match(/role="radio" data-theme=/g) || []).length, 6);
  ids.forEach((id) => {
    assert.match(html, new RegExp(`data-theme="${id}"`));
    assert.match(tokens, new RegExp(`:root\\[data-theme="${id}"\\]`));
  });
  assert.match(shell, /const THEME_KEY = 'r808_web_theme'/);
  assert.match(mobile, /const THEME_KEY = 'r808_web_theme'/);
  assert.match(nav, /document\.documentElement\.dataset\.theme = savedTheme/);
  assert.match(shell, /classList\.remove\('mono-mode'\)/);
  assert.match(mobile, /classList\.remove\('mono-mode'\)/);
  assert.match(mobile, /cmd: 'setLedMonoMode', value: id === 'greyscale'/);
  assert.match(mobile, /acid: 'ocean'/);
  assert.match(mobile, /retro: 'rainbow'/);
  assert.match(mobile, /gray: 'greyscale'/);
  assert.match(shell, /function cycleTheme\(direction\)/);
  assert.match(read('data/web/keyboard-controls.js'), /RED808Themes\.cycle\(e\.shiftKey \? -1 : 1\)/);
  assert.match(read('data/web/app.js'), /document\.documentElement\.dataset\.theme === 'greyscale'/);
  const workspaces = read('data/web/studio-workspaces.css');
  assert.match(workspaces, /--mixer-track-rgb:\s*var\(--pad-color-0\)/);
  assert.match(workspaces, /rgba\(var\(--r808-accent-rgb/);
  assert.match(read('data/web/patchbay.css'), /rgba\(var\(--r808-bg-surface-rgb/);
});

test('live pads reserve control zones and expose synth engines through one popover', () => {
  const app = read('data/web/app.js');
  const synthEditor = read('data/web/synth-editor.js');
  const workspace = read('data/web/workspace-2026.css');

  assert.match(app, /class="pad-topbar"/);
  assert.match(app, /class="pad-hit-area"/);
  assert.match(app, /class="pad-action-bar"/);
  assert.match(app, /class="pad-engine-menu-toggle"/);
  assert.match(app, /class="pad-engine-options"/);
  assert.match(app, /data-engine="-1"[^>]*>SMP</);
  assert.match(app, /pad\.querySelector\('\.pad-topbar'\)\?\.appendChild\(synthStrip\)/);
  assert.match(app, /function closePadEngineMenus\(exceptStrip = null\)/);
  assert.match(app, /if \(event\.key === 'Escape'\) closePadEngineMenus\(\)/);
  assert.match(app, /s\.src = `\$\{src\}\?v=\$\{DEFERRED_ASSET_VERSION\}`/);
  assert.doesNotMatch(app, /pad-synth-column/);
  assert.match(synthEditor, /paramsBtn\.textContent = 'SYN'/);
  assert.match(synthEditor, /pad\.querySelector\('\.pad-action-bar'\)/);

  assert.match(workspace, /grid-template-rows: 32px minmax\(52px, 1fr\) 36px/);
  assert.match(workspace, /\.pad-action-bar[\s\S]*grid-template-columns: repeat\(4, minmax\(0, 1fr\)\)/);
  assert.match(workspace, /\.pad-synth-strip,[\s\S]*position: static !important/);
  assert.match(workspace, /\.pad\.engine-menu-open[\s\S]*contain: none !important[\s\S]*overflow: visible !important/);
  assert.match(workspace, /\.pad-engine-options[\s\S]*grid-template-columns: repeat\(4, minmax\(42px, 1fr\)\)/);
  assert.match(workspace, /\.pad-engine-options[\s\S]*z-index: 320/);
  assert.match(workspace, /\.pad-engine-options \.synth-btn,[\s\S]*height: 42px !important/);
  assert.match(workspace, /\.pad\.synth-mode-active \.pad-key-hint[\s\S]*display: none !important/);
  assert.match(workspace, /\.pad-action-bar \.synth-params-btn,[\s\S]*position: static !important/);
});
