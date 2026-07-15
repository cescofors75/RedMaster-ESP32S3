/* RED808: aplica el tema visual guardado por la app principal (localStorage
   'r808_web_theme') a páginas que no cargan workspace-ui.js — admin,
   multiview. Sin esto se quedan siempre en el tema por defecto aunque
   ui-2026.css/theme-vars.css ya estén cargados, porque nadie fija
   data-theme en <html>/<body>. */
(function () {
  'use strict';
  var THEME_KEY = 'r808_web_theme';
  var THEME_IDS = ['red808', 'ocean', 'neon', 'sunset', 'rainbow', 'greyscale'];

  function normalizeTheme(id) {
    var aliases = { red: 'red808', acid: 'ocean', retro: 'rainbow', gray: 'greyscale', violet: 'rainbow' };
    var normalized = aliases[id] || id;
    return THEME_IDS.indexOf(normalized) !== -1 ? normalized : 'red808';
  }

  function apply(id) {
    var theme = normalizeTheme(id);
    document.documentElement.dataset.theme = theme;
    document.body.dataset.theme = theme;
  }

  var saved;
  try { saved = localStorage.getItem(THEME_KEY); } catch (_) { saved = null; }
  apply(saved || 'red808');

  // Cambios de tema hechos en otra pestaña/página (misma sesión) se reflejan
  // aquí sin recargar, vía el evento storage estándar.
  window.addEventListener('storage', function (event) {
    if (event.key === THEME_KEY) apply(event.newValue || 'red808');
  });
})();
