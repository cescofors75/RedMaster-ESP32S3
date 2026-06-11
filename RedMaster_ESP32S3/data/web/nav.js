/* RED808 nav.js — navegación común + indicador de conexión unificado.
 *
 * Se carga en el <head> de TODAS las páginas (sin defer) para poder
 * interceptar el constructor de WebSocket antes de que el script de la
 * página cree su conexión. No abre ningún WebSocket propio (cada cliente
 * WS cuesta heap en el ESP32): observa el que la página ya crea hacia /ws.
 *
 * UI: botón flotante "808" (esquina inferior derecha) con un punto de
 * estado (verde=conectado, ámbar=conectando, rojo=desconectado) que
 * despliega el menú de páginas. También inyecta estilos globales de
 * accesibilidad (:focus-visible) y scrollbar oscura coherente.
 */
(function () {
  'use strict';

  /* ── 1. Monitor de conexión: interceptar WebSocket hacia /ws ────────── */
  var navState = 'idle';            // idle | connecting | open | closed
  var statusDot = null;

  function setNavState(s) {
    navState = s;
    if (!statusDot) return;
    statusDot.className = 'r808nav-dot r808nav-dot--' + s;
    statusDot.title = { open: 'Conectado', connecting: 'Conectando…',
                        closed: 'Desconectado', idle: 'Sin WebSocket' }[s] || s;
  }

  var NativeWS = window.WebSocket;
  function PatchedWS(url, protocols) {
    var sock = protocols !== undefined ? new NativeWS(url, protocols)
                                       : new NativeWS(url);
    try {
      if (String(url).indexOf('/ws') !== -1) {
        setNavState('connecting');
        sock.addEventListener('open',  function () { setNavState('open'); });
        sock.addEventListener('close', function () { setNavState('closed'); });
        sock.addEventListener('error', function () { setNavState('closed'); });
      }
    } catch (e) { /* el monitor nunca debe romper la conexión de la página */ }
    return sock;
  }
  PatchedWS.prototype = NativeWS.prototype;
  PatchedWS.CONNECTING = NativeWS.CONNECTING;
  PatchedWS.OPEN       = NativeWS.OPEN;
  PatchedWS.CLOSING    = NativeWS.CLOSING;
  PatchedWS.CLOSED     = NativeWS.CLOSED;
  window.WebSocket = PatchedWS;

  /* ── 2. Páginas del dispositivo ──────────────────────────────────────── */
  var PAGES = [
    { href: '/',            label: 'Sequencer', match: ['/', '/index.html'] },
    { href: '/patchbay',    label: 'Patchbay',  match: ['/patchbay', '/patchbay.html'] },
    { href: '/multiview',   label: 'Multiview', match: ['/multiview', '/multiview.html'] },
    { href: '/gesture-pro', label: 'Gesture',   match: ['/gesture-pro', '/gesture-pro.html', '/gesture', '/gesture.html'] },
    { href: '/mobile',      label: 'Mobile',    match: ['/mobile', '/mobile.html'] },
    { href: '/adm',         label: 'Admin',     match: ['/adm', '/admin.html'] }
  ];

  /* ── 3. UI ───────────────────────────────────────────────────────────── */
  function buildUi() {
    var css = [
      '.r808nav{position:fixed;right:12px;bottom:12px;z-index:99990;',
      'font-family:system-ui,-apple-system,sans-serif;-webkit-user-select:none;user-select:none}',
      '.r808nav-fab{display:flex;align-items:center;gap:6px;padding:8px 12px;',
      'background:var(--r808-bg-elevated,#1a1a20);color:var(--r808-text,#dde4f0);',
      'border:1px solid var(--r808-border,rgba(255,255,255,.12));border-radius:999px;',
      'font-size:12px;font-weight:700;letter-spacing:.08em;cursor:pointer;',
      'box-shadow:0 4px 14px rgba(0,0,0,.45);transition:transform .15s ease,border-color .15s ease}',
      '.r808nav-fab:hover{transform:translateY(-1px);border-color:var(--r808-red-glow,#ff4d4d)}',
      '.r808nav-dot{width:9px;height:9px;border-radius:50%;background:#555;flex:0 0 auto;',
      'transition:background .2s ease,box-shadow .2s ease}',
      '.r808nav-dot--open{background:var(--r808-accent-green,#30d158);box-shadow:0 0 6px var(--r808-accent-green,#30d158)}',
      '.r808nav-dot--connecting{background:var(--r808-accent-orange,#ff9500);box-shadow:0 0 6px var(--r808-accent-orange,#ff9500)}',
      '.r808nav-dot--closed{background:var(--r808-red-primary,#ff0000);box-shadow:0 0 6px var(--r808-red-primary,#ff0000);',
      'animation:r808navPulse 1.2s ease-in-out infinite}',
      '@keyframes r808navPulse{50%{opacity:.35}}',
      '.r808nav-menu{position:absolute;right:0;bottom:calc(100% + 8px);min-width:150px;',
      'display:none;flex-direction:column;padding:6px;',
      'background:var(--r808-bg-surface,#10141d);border:1px solid var(--r808-border,rgba(255,255,255,.12));',
      'border-radius:12px;box-shadow:0 8px 24px rgba(0,0,0,.55)}',
      '.r808nav.open .r808nav-menu{display:flex}',
      '.r808nav-link{padding:8px 12px;border-radius:8px;color:var(--r808-text,#dde4f0);',
      'text-decoration:none;font-size:13px;line-height:1;white-space:nowrap}',
      '.r808nav-link:hover{background:var(--r808-bg-elevated,#1a1a20);color:var(--r808-red-glow,#ff4d4d)}',
      '.r808nav-link.current{color:var(--r808-red-glow,#ff4d4d);font-weight:700;pointer-events:none}',
      /* Accesibilidad global: foco visible para navegación por teclado */
      ':focus-visible{outline:2px solid var(--r808-accent-cyan,#00e5ff);outline-offset:2px}',
      /* Scrollbar oscura coherente en todas las páginas */
      '*{scrollbar-width:thin;scrollbar-color:var(--r808-bg-elevated,#1a1a20) transparent}',
      '::-webkit-scrollbar{width:10px;height:10px}',
      '::-webkit-scrollbar-track{background:transparent}',
      '::-webkit-scrollbar-thumb{background:var(--r808-bg-elevated,#1a1a20);border-radius:5px;',
      'border:2px solid transparent;background-clip:content-box}',
      '::-webkit-scrollbar-thumb:hover{background:var(--r808-text-dim,#6e7b91);',
      'border:2px solid transparent;background-clip:content-box}'
    ].join('');

    var style = document.createElement('style');
    style.textContent = css;
    document.head.appendChild(style);

    var root = document.createElement('nav');
    root.className = 'r808nav';
    root.setAttribute('aria-label', 'RED808 páginas');

    var menu = document.createElement('div');
    menu.className = 'r808nav-menu';
    var path = window.location.pathname;
    PAGES.forEach(function (p) {
      var a = document.createElement('a');
      a.className = 'r808nav-link' + (p.match.indexOf(path) !== -1 ? ' current' : '');
      a.href = p.href;
      a.textContent = p.label;
      menu.appendChild(a);
    });

    var fab = document.createElement('button');
    fab.className = 'r808nav-fab';
    fab.type = 'button';
    fab.setAttribute('aria-haspopup', 'true');
    fab.setAttribute('aria-expanded', 'false');
    statusDot = document.createElement('span');
    statusDot.className = 'r808nav-dot';
    fab.appendChild(statusDot);
    fab.appendChild(document.createTextNode('808'));
    fab.addEventListener('click', function () {
      var open = root.classList.toggle('open');
      fab.setAttribute('aria-expanded', open ? 'true' : 'false');
    });
    document.addEventListener('click', function (e) {
      if (!root.contains(e.target)) {
        root.classList.remove('open');
        fab.setAttribute('aria-expanded', 'false');
      }
    });

    root.appendChild(menu);
    root.appendChild(fab);
    document.body.appendChild(root);
    setNavState(navState);  // pintar el estado acumulado antes del DOM ready
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', buildUi);
  } else {
    buildUi();
  }

  /* ── 4. Wake lock en páginas de performance táctil ───────────────────
   * En /mobile y /gesture* la pantalla se apagaba en plena sesión. Se
   * solicita al primer toque (los navegadores exigen gesto de usuario)
   * y se re-adquiere al volver de background. Sin soporte → no-op.     */
  var path = window.location.pathname;
  var isTouchPerf = path.indexOf('/mobile') === 0 || path.indexOf('/gesture') === 0;
  if (isTouchPerf && 'wakeLock' in navigator) {
    var wakeLock = null;
    var requestWakeLock = function () {
      navigator.wakeLock.request('screen').then(function (wl) {
        wakeLock = wl;
      }).catch(function () { /* denegado/no disponible: ignorar */ });
    };
    document.addEventListener('pointerdown', function onFirstTouch() {
      document.removeEventListener('pointerdown', onFirstTouch);
      requestWakeLock();
    });
    document.addEventListener('visibilitychange', function () {
      if (!document.hidden && wakeLock !== null) requestWakeLock();
    });
  }
})();
