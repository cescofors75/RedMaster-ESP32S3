/* RED808 2026 workspace shell: progressive disclosure and accessible navigation. */
(function () {
  'use strict';

  const WORKSPACES = {
    performance: {
      kicker: 'PERFORMANCE',
      title: 'PADS',
      description: 'Toca, carga y transforma tus sonidos principales.'
    },
    sequencer: {
      kicker: 'COMPOSICIÓN',
      title: 'Secuenciar',
      description: 'Construye patrones, variaciones y arreglos con control preciso.'
    },
    volumes: {
      kicker: 'BALANCE',
      title: 'Mezclar',
      description: 'Ajusta niveles, mute y solo sin abandonar el flujo musical.'
    },
    melody: {
      kicker: 'INSTRUMENTO',
      title: 'Melodía',
      description: 'Programa voces, notas y articulaciones de los motores melódicos.'
    },
    'xtra-pads': {
      kicker: 'HERRAMIENTA',
      title: 'XTRA Pads',
      description: 'Organiza disparos independientes y material fuera del kit principal.'
    },
    stems: {
      kicker: 'HERRAMIENTA',
      title: 'Stems',
      description: 'Gestiona pistas largas, acompañamientos y estado real en Daisy.'
    }
  };

  const THEME_KEY = 'r808_web_theme';
  const THEME_IDS = ['red808', 'ocean', 'neon', 'sunset', 'rainbow', 'greyscale'];
  const THEME_NAMES = {
    red808: 'RED808', ocean: 'OCEAN', neon: 'NEON', sunset: 'SUNSET',
    rainbow: 'RAINBOW', greyscale: 'GREYSCALE'
  };
  const THEME_COLORS = {
    red808: '#e23d22', ocean: '#4a9eff', neon: '#39ff14', sunset: '#ff6b35',
    rainbow: '#ff00aa', greyscale: '#cccccc'
  };

  function normalizeTheme(id) {
    const aliases = { red: 'red808', acid: 'ocean', retro: 'rainbow', gray: 'greyscale', violet: 'rainbow' };
    const normalized = aliases[id] || id;
    return THEME_IDS.includes(normalized) ? normalized : 'red808';
  }

  function applyTheme(themeId, notifyHardware) {
    const id = normalizeTheme(themeId);
    const previous = normalizeTheme(document.documentElement.dataset.theme || 'red808');
    if (previous !== 'greyscale' && id === 'greyscale') localStorage.setItem('r808LastColorTheme', previous);
    document.documentElement.dataset.theme = id;
    document.body.dataset.theme = id;
    // Legacy mono-mode paints fixed red gradients. Greyscale is a real theme,
    // so it must use the shared neutral palette instead of those old rules.
    document.body.classList.remove('mono-mode');
    localStorage.setItem(THEME_KEY, id);

    const name = document.getElementById('workspaceThemeName');
    if (name) name.textContent = THEME_NAMES[id];
    document.querySelectorAll('#workspaceThemeMenu [data-theme]').forEach((button) => {
      const selected = button.dataset.theme === id;
      button.classList.toggle('active', selected);
      button.setAttribute('aria-checked', selected ? 'true' : 'false');
    });
    const colorToggle = document.getElementById('colorToggle');
    if (colorToggle) colorToggle.textContent = id === 'greyscale' ? 'MONO · GREYSCALE' : `TEMA · ${THEME_NAMES[id]}`;
    const metaTheme = document.querySelector('meta[name="theme-color"]');
    if (metaTheme) metaTheme.setAttribute('content', THEME_COLORS[id]);
    if (notifyHardware && typeof window.syncLedMonoMode === 'function') window.syncLedMonoMode();
    window.dispatchEvent(new CustomEvent('red808:themechange', { detail: { theme: id } }));
    return id;
  }

  function toggleMonoTheme() {
    const current = normalizeTheme(document.documentElement.dataset.theme || 'red808');
    const next = current === 'greyscale'
      ? normalizeTheme(localStorage.getItem('r808LastColorTheme') || 'red808')
      : 'greyscale';
    applyTheme(next, true);
  }

  function cycleTheme(direction) {
    const current = normalizeTheme(document.documentElement.dataset.theme || 'red808');
    const index = THEME_IDS.indexOf(current);
    const next = THEME_IDS[(index + (direction < 0 ? -1 : 1) + THEME_IDS.length) % THEME_IDS.length];
    applyTheme(next, true);
    return next;
  }

  window.RED808Themes = {
    apply: applyTheme,
    toggleMono: toggleMonoTheme,
    cycle: cycleTheme,
    name: (id) => THEME_NAMES[normalizeTheme(id)],
    ids: THEME_IDS.slice()
  };

  function setWorkspace(tabId) {
    const workspace = WORKSPACES[tabId] || WORKSPACES.performance;
    const kicker = document.getElementById('workspaceKicker');
    const title = document.getElementById('workspaceTitle');
    const description = document.getElementById('workspaceDescription');
    if (kicker) kicker.textContent = workspace.kicker;
    if (title) title.textContent = workspace.title;
    if (description) description.textContent = workspace.description;

    document.querySelectorAll('.tab-btn[data-tab]').forEach((button) => {
      const selected = button.dataset.tab === tabId;
      button.setAttribute('aria-selected', selected ? 'true' : 'false');
      button.setAttribute('tabindex', button.closest('.workspace-primary-tabs')
        ? (selected ? '0' : '-1')
        : '0');
    });
    document.querySelectorAll('.tab-content').forEach((panel) => {
      panel.setAttribute('aria-hidden', panel.id === `tab-${tabId}` ? 'false' : 'true');
    });

    const moreMenu = document.getElementById('workspaceMoreMenu');
    if (moreMenu) {
      moreMenu.classList.toggle('contains-active', tabId === 'xtra-pads' || tabId === 'stems');
    }
  }

  function setAdvanced(enabled) {
    document.body.classList.toggle('advanced-ui', enabled);
    const button = document.getElementById('advancedModeBtn');
    if (button) {
      button.setAttribute('aria-pressed', enabled ? 'true' : 'false');
      button.textContent = enabled ? 'OCULTAR AVANZADOS' : 'CONTROLES AVANZADOS';
    }
    localStorage.setItem('red808AdvancedUi', enabled ? '1' : '0');
  }

  function setFocus(enabled) {
    document.body.classList.toggle('workspace-focus-mode', enabled);
    const button = document.getElementById('focusModeBtn');
    if (button) {
      button.setAttribute('aria-pressed', enabled ? 'true' : 'false');
      button.textContent = enabled ? 'SALIR DE FOCO' : 'MODO FOCO';
    }
  }

  document.addEventListener('DOMContentLoaded', function () {
    const tabButtons = Array.from(document.querySelectorAll('.tab-btn[data-tab]'));
    const primaryTabs = Array.from(document.querySelectorAll('.workspace-primary-tabs .tab-btn[data-tab]'));
    const panels = Array.from(document.querySelectorAll('.tab-content'));

    tabButtons.forEach((button) => {
      button.setAttribute('role', 'tab');
      button.setAttribute('aria-controls', `tab-${button.dataset.tab}`);
      button.addEventListener('click', () => {
        const moreMenu = document.getElementById('workspaceMoreMenu');
        if (moreMenu) moreMenu.open = false;
      });
    });
    document.addEventListener('click', (event) => {
      const picker = document.getElementById('workspaceThemePicker');
      if (picker && picker.open && !picker.contains(event.target)) picker.open = false;
    });
    panels.forEach((panel) => {
      panel.setAttribute('role', 'tabpanel');
      panel.setAttribute('tabindex', '0');
    });

    document.querySelectorAll('#workspaceThemeMenu [data-theme]').forEach((button) => {
      button.addEventListener('click', () => {
        applyTheme(button.dataset.theme, true);
        const picker = document.getElementById('workspaceThemePicker');
        if (picker) picker.open = false;
      });
    });

    primaryTabs.forEach((button, index) => {
      button.addEventListener('keydown', (event) => {
        if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return;
        event.preventDefault();
        let next = index;
        if (event.key === 'ArrowRight') next = (index + 1) % primaryTabs.length;
        if (event.key === 'ArrowLeft') next = (index - 1 + primaryTabs.length) % primaryTabs.length;
        if (event.key === 'Home') next = 0;
        if (event.key === 'End') next = primaryTabs.length - 1;
        primaryTabs[next].focus();
        primaryTabs[next].click();
      });
    });

    const advancedButton = document.getElementById('advancedModeBtn');
    if (advancedButton) {
      advancedButton.addEventListener('click', () => setAdvanced(!document.body.classList.contains('advanced-ui')));
    }
    const focusButton = document.getElementById('focusModeBtn');
    if (focusButton) {
      focusButton.addEventListener('click', () => setFocus(!document.body.classList.contains('workspace-focus-mode')));
    }
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') {
        const picker = document.getElementById('workspaceThemePicker');
        if (picker && picker.open) picker.open = false;
      }
      if (event.key === 'Escape' && document.body.classList.contains('workspace-focus-mode')) setFocus(false);
    });
    window.addEventListener('red808:tabchange', (event) => setWorkspace(event.detail.tabId));

    applyTheme(localStorage.getItem(THEME_KEY) || document.documentElement.dataset.theme || 'red808', false);
    localStorage.removeItem('red808AdvancedUi');
    document.body.classList.remove('advanced-ui');
    const active = document.querySelector('.tab-btn[data-tab].active');
    setWorkspace(active ? active.dataset.tab : 'performance');
  });
})();
