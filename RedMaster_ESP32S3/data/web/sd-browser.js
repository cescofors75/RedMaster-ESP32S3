/* Daisy SD browser domain module. Loaded before app.js; functions resolve
 * app globals (sendWebSocket/applyDaisySampleMetadata) when invoked. */
let _sdSelectedFile = null;
let _sdSelectedPad = -1;

function sdRefreshStatus() {
  sendWebSocket({ cmd: 'sdGetStatus' });
  sendWebSocket({ cmd: 'sdListKits' });
}

function sdListFolders() {
  sendWebSocket({ cmd: 'sdListFolders' });
  const bc = document.getElementById('sdBreadcrumb');
  if (bc) {
    bc.replaceChildren();
    const root = document.createElement('button');
    root.type = 'button'; root.className = 'sd-crumb sd-crumb-root'; root.textContent = '/ root';
    root.addEventListener('click', sdListFolders);
    bc.appendChild(root);
  }
  _sdSelectedFile = null;
  sdUpdateAssignInfo();
}

function sdListFiles(folder) {
  folder = String(folder ?? '');
  sendWebSocket({ cmd: 'sdListFiles', folder });
  const bc = document.getElementById('sdBreadcrumb');
  if (bc) {
    bc.replaceChildren();
    const root = document.createElement('button');
    root.type = 'button'; root.className = 'sd-crumb sd-crumb-root'; root.textContent = '/ root';
    root.addEventListener('click', sdListFolders);
    const sep = document.createElement('span'); sep.className = 'sd-crumb-sep'; sep.textContent = '›';
    const current = document.createElement('span'); current.className = 'sd-crumb'; current.textContent = folder;
    bc.append(root, sep, current);
  }
  _sdSelectedFile = null;
  sdUpdateAssignInfo();
}

function sdLoadKit(kit) { sendWebSocket({ cmd: 'sdLoadKit', kit }); sdLog(`Loading kit: ${kit}...`); }
function sdUnloadKit() { sendWebSocket({ cmd: 'sdUnloadKit' }); }

function sdAssignFile(pad) {
  if (!_sdSelectedFile) return;
  sendWebSocket({ cmd: 'sdLoadSample', pad, folder: _sdSelectedFile.folder, file: _sdSelectedFile.file });
  sdLog(`Loading "${_sdSelectedFile.file}" → pad ${pad}...`);
}

function sdRenderStatus(data) {
  const ind = document.getElementById('sdIndicator');
  const txt = document.getElementById('sdStatusText');
  const stats = document.getElementById('sdStats');
  const kitSec = document.getElementById('sdCurrentKit');
  if (ind) ind.style.color = data.present ? '#0f0' : '#f00';
  if (txt) txt.textContent = data.present ? 'SD Card OK' : 'No SD Card';
  if (stats && data.present) stats.textContent = `${data.freeMB}/${data.totalMB} MB free`;
  if (kitSec) {
    const hasKit = Boolean(data.kit && data.kit.length);
    kitSec.style.display = hasKit ? 'flex' : 'none';
    if (hasKit) {
      const name = document.getElementById('sdKitName');
      const pads = document.getElementById('sdKitPads');
      if (name) name.textContent = data.kit;
      if (pads) pads.textContent = `(${data.loaded} pads)`;
    }
  }
}

function sdEmptyRow(className, text) {
  const row = document.createElement('div'); row.className = className; row.textContent = text; return row;
}

function sdRenderKitList(kits, error) {
  const el = document.getElementById('sdKitList'); if (!el) return;
  el.replaceChildren();
  if (error) { el.appendChild(sdEmptyRow('sd-error', String(error))); return; }
  if (!kits.length) { el.appendChild(sdEmptyRow('sd-empty', 'No kits found')); return; }
  kits.forEach((value) => {
    const kit = String(value);
    const row = document.createElement('button'); row.type = 'button'; row.className = 'sd-kit-item'; row.title = 'Click to load';
    const icon = document.createElement('span'); icon.className = 'sd-kit-icon'; icon.textContent = '📁';
    row.append(icon, document.createTextNode(` ${kit}`));
    row.addEventListener('click', () => sdLoadKit(kit));
    el.appendChild(row);
  });
}

function sdRenderFolders(folders) {
  const el = document.getElementById('sdFileList'); if (!el) return;
  el.replaceChildren();
  if (!folders.length) { el.appendChild(sdEmptyRow('sd-empty', 'Empty')); return; }
  folders.forEach((value) => {
    const folder = String(value);
    const row = document.createElement('button'); row.type = 'button'; row.className = 'sd-folder-item';
    const icon = document.createElement('span'); icon.className = 'sd-icon'; icon.textContent = '📂';
    row.append(icon, document.createTextNode(` ${folder}`));
    row.addEventListener('click', () => sdListFiles(folder));
    el.appendChild(row);
  });
}

function sdRenderFiles(folderValue, files) {
  const el = document.getElementById('sdFileList'); if (!el) return;
  const folder = String(folderValue ?? '');
  el.replaceChildren();
  if (!files.length) { el.appendChild(sdEmptyRow('sd-empty', 'No files')); return; }
  files.forEach((info) => {
    const name = String(info.name ?? '');
    const sizeKB = ((Number(info.size) || 0) / 1024).toFixed(1);
    const row = document.createElement('button'); row.type = 'button'; row.className = 'sd-file-item'; row.title = `${sizeKB} KB`;
    const icon = document.createElement('span'); icon.className = 'sd-icon'; icon.textContent = '🔊';
    const small = document.createElement('small'); small.textContent = `${sizeKB}K`;
    row.append(icon, document.createTextNode(` ${name} `), small);
    row.addEventListener('click', () => sdSelectFile(folder, name, row));
    el.appendChild(row);
  });
}

function sdSelectFile(folder, file, element) {
  document.querySelectorAll('.sd-file-item.selected').forEach((item) => item.classList.remove('selected'));
  if (element) element.classList.add('selected');
  _sdSelectedFile = { folder, file };
  sdUpdateAssignInfo();
}

function sdUpdateAssignInfo() {
  const el = document.getElementById('sdAssignInfo'); if (!el) return;
  el.textContent = _sdSelectedFile ? `"${_sdSelectedFile.file}" → click a pad to assign` : 'Select a file, then click a pad';
  el.classList.toggle('ready', Boolean(_sdSelectedFile));
}

function sdHandleEvent(data) {
  const names = { 1: 'Kit loaded', 2: 'Load error', 3: 'Kit unloaded', 4: 'Sample loaded', 5: 'Boot loaded', 6: 'XTRA loaded' };
  sdLog(`${names[data.event] || `Event ${data.event}`}${data.name ? ` "${data.name}"` : ''} (${data.padCount} pads)`);
  if (data.event === 4 && data.name) {
    const pads = [], masks = [data.maskLo, data.maskHi, data.maskXtra].map((v) => (Number(v) || 0) & 0xff);
    for (let bit = 0; bit < 8; bit++) masks.forEach((mask, bank) => { if (mask & (1 << bit)) pads.push(bank * 8 + bit); });
    pads.forEach((pad) => applyDaisySampleMetadata(pad, data.name));
  }
  if ([1, 3, 4].includes(data.event)) sdRefreshStatus();
}

function sdLog(message) {
  const el = document.getElementById('sdLog'); if (!el) return;
  const line = document.createElement('div'); line.className = 'sd-log-line';
  line.textContent = `[${new Date().toLocaleTimeString()}] ${message}`;
  el.prepend(line);
  while (el.children.length > 20) el.lastChild.remove();
}

function initializeSdBrowser() {
  const grid = document.getElementById('sdPadGrid');
  const names = ['BD','SD','CH','OH','CY','CP','RS','CB','LT','MT','HT','MA','CL','HC','MC','LC'];
  if (grid && !grid.children.length) names.forEach((name, index) => {
    const button = document.createElement('button'); button.type = 'button'; button.className = 'sd-pad-btn'; button.textContent = name;
    button.addEventListener('click', () => {
      _sdSelectedPad = index;
      document.querySelectorAll('.sd-pad-btn.active').forEach((item) => item.classList.remove('active'));
      button.classList.add('active'); sdAssignFile(index);
    });
    grid.appendChild(button);
  });
  document.querySelectorAll('.tab-btn[data-tab]').forEach((button) => button.addEventListener('click', () => {
    if (button.dataset.tab === 'daisy-sd') sdRefreshStatus();
  }));
}
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initializeSdBrowser, { once: true });
} else {
  initializeSdBrowser();
}
