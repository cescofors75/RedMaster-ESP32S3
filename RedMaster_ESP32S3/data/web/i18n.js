/* RED808 i18n — traducción compartida para todas las páginas.
 *
 * No depende de internet ni de un servicio externo: el catálogo viaja dentro
 * del firmware y funciona también en el AP aislado del ESP32. Los textos
 * creados dinámicamente se traducen mediante MutationObserver y los módulos
 * pueden usar window.t('clave') directamente.
 */
(function () {
  'use strict';

  var LOCALES = {
    ca: 'Català', es: 'Castellano', eu: 'Euskara', gl: 'Galego', en: 'English',
    fr: 'Français', de: 'Deutsch', it: 'Italiano', pt: 'Português'
  };
  var ORDER = ['ca', 'es', 'eu', 'gl', 'en', 'fr', 'de', 'it', 'pt'];
  var CATALOG = {
    'nav.sequencer': { sources: ['Sequencer', 'SEQUENCER', 'Secuenciador'], ca: 'Seqüenciador', es: 'Secuenciador', eu: 'Sekuentziadorea', gl: 'Secuenciador', en: 'Sequencer', fr: 'Séquenceur', de: 'Sequencer', it: 'Sequencer', pt: 'Sequenciador' },
    'nav.demo': { sources: ['Demo rápida', 'Demo'], ca: 'Demo ràpida', es: 'Demo rápida', eu: 'Demo azkarra', gl: 'Demo rápida', en: 'Quick demo', fr: 'Démo rapide', de: 'Schnelldemo', it: 'Demo rapida', pt: 'Demo rápida' },
    'nav.path': { sources: ['PATH'], ca: 'PATH', es: 'PATH', eu: 'PATH', gl: 'PATH', en: 'PATH', fr: 'PATH', de: 'PATH', it: 'PATH', pt: 'PATH' },
    'nav.multiview': { sources: ['Multiview'], ca: 'Multivista', es: 'Multivista', eu: 'Ikuspegi anitza', gl: 'Multivista', en: 'Multiview', fr: 'Multivue', de: 'Multiview', it: 'Multiview', pt: 'Multivista' },
    'nav.gesture': { sources: ['Gesture'], ca: 'Gestos', es: 'Gestos', eu: 'Keinuak', gl: 'Xestos', en: 'Gesture', fr: 'Gestes', de: 'Gesten', it: 'Gesti', pt: 'Gestos' },
    'nav.mobile': { sources: ['Mobile'], ca: 'Mòbil', es: 'Móvil', eu: 'Mugikorra', gl: 'Móbil', en: 'Mobile', fr: 'Mobile', de: 'Mobil', it: 'Mobile', pt: 'Móvel' },
    'nav.admin': { sources: ['Admin', 'ADMIN'], ca: 'Admin', es: 'Admin', eu: 'Admin', gl: 'Admin', en: 'Admin', fr: 'Admin', de: 'Admin', it: 'Admin', pt: 'Admin' },
    'nav.language': { sources: ['Idioma', 'Language'], ca: 'Idioma', es: 'Idioma', eu: 'Hizkuntza', gl: 'Idioma', en: 'Language', fr: 'Langue', de: 'Sprache', it: 'Lingua', pt: 'Idioma' },
    'status.connecting': { sources: ['CONECTANDO...', 'Conectando...', 'Conectando…', 'CONECTANDO…'], ca: 'CONNECTANT...', es: 'CONECTANDO...', eu: 'KONEKTATZEN...', gl: 'CONECTANDO...', en: 'CONNECTING...', fr: 'CONNEXION...', de: 'VERBINDE...', it: 'CONNESSIONE...', pt: 'A LIGAR...' },
    'status.connected': { sources: ['Conectado', 'Dispositivo Conectado'], ca: 'Connectat', es: 'Conectado', eu: 'Konektatuta', gl: 'Conectado', en: 'Connected', fr: 'Connecté', de: 'Verbunden', it: 'Connesso', pt: 'Ligado' },
    'status.disconnected': { sources: ['Desconectado'], ca: 'Desconnectat', es: 'Desconectado', eu: 'Deskonektatuta', gl: 'Desconectado', en: 'Disconnected', fr: 'Déconnecté', de: 'Getrennt', it: 'Disconnesso', pt: 'Desligado' },
    'status.waiting': { sources: ['Esperando conexión...', 'Esperando conexión…'], ca: 'Esperant connexió...', es: 'Esperando conexión...', eu: 'Konexioaren zain...', gl: 'Agardando conexión...', en: 'Waiting for connection...', fr: 'En attente de connexion...', de: 'Warte auf Verbindung...', it: 'In attesa di connessione...', pt: 'A aguardar ligação...' },
    'status.offline': { sources: ['Offline'], ca: 'Desconnectat', es: 'Sin conexión', eu: 'Lineaz kanpo', gl: 'Sen conexión', en: 'Offline', fr: 'Hors ligne', de: 'Offline', it: 'Offline', pt: 'Offline' },
    'core.performance': { sources: ['PERFORMANCE'], ca: 'ACTUACIÓ', es: 'PERFORMANCE', eu: 'EMANALDIA', gl: 'ACTUACIÓN', en: 'PERFORMANCE', fr: 'PERFORMANCE', de: 'PERFORMANCE', it: 'PERFORMANCE', pt: 'PERFORMANCE' },
    'core.pads': { sources: ['PADS', 'Pads'], ca: 'PADS', es: 'PADS', eu: 'PAD-AK', gl: 'PADS', en: 'PADS', fr: 'PADS', de: 'PADS', it: 'PAD', pt: 'PADS' },
    'core.livePads': { sources: ['LIVE PADS'], ca: 'PADS EN DIRECTE', es: 'PADS EN VIVO', eu: 'LIVE PAD-AK', gl: 'PADS EN DIRECTO', en: 'LIVE PADS', fr: 'PADS LIVE', de: 'LIVE-PADS', it: 'PAD LIVE', pt: 'PADS AO VIVO' },
    'core.description': { sources: ['Toca, carga y transforma tus sonidos principales.'], ca: 'Toca, carrega i transforma els teus sons principals.', es: 'Toca, carga y transforma tus sonidos principales.', eu: 'Jo, kargatu eta eraldatu zure soinu nagusiak.', gl: 'Toca, carga e transforma os teus sons principais.', en: 'Play, load and transform your main sounds.', fr: 'Jouez, chargez et transformez vos sons principaux.', de: 'Spiele, lade und bearbeite deine Hauptsounds.', it: 'Suona, carica e trasforma i tuoi suoni principali.', pt: 'Toca, carrega e transforma os teus sons principais.' },
    'transport.play': { sources: ['PLAY', '▶ PLAY'], ca: 'REPRODUEIX', es: 'REPRODUCIR', eu: 'ERREPRODUZITU', gl: 'REPRODUCIR', en: 'PLAY', fr: 'LECTURE', de: 'WIEDERGABE', it: 'RIPRODUCI', pt: 'REPRODUZIR' },
    'transport.stop': { sources: ['STOP', '■ STOP', '⏹ Stop seq'], ca: 'ATURA', es: 'PARAR', eu: 'GELDITU', gl: 'PARAR', en: 'STOP', fr: 'ARRÊT', de: 'STOPP', it: 'STOP', pt: 'PARAR' },
    'transport.pattern': { sources: ['PATRÓN ACTUAL', 'PAT', 'PAT +', 'PAT -'], ca: 'PATRÓ ACTUAL', es: 'PATRÓN ACTUAL', eu: 'UNEKO EREDUA', gl: 'PATRÓN ACTUAL', en: 'CURRENT PATTERN', fr: 'PATTERN ACTUEL', de: 'AKTUELLES PATTERN', it: 'PATTERN ATTUALE', pt: 'PADRÃO ATUAL' },
    'transport.bpm': { sources: ['BPM'], ca: 'BPM', es: 'BPM', eu: 'BPM', gl: 'BPM', en: 'BPM', fr: 'BPM', de: 'BPM', it: 'BPM', pt: 'BPM' },
    'transport.volSeq': { sources: ['VOL SEQ'], ca: 'VOL SEQ', es: 'VOL SEQ', eu: 'BOL SEQ', gl: 'VOL SEQ', en: 'SEQ VOL', fr: 'VOL SÉQ', de: 'SEQ-LAUTST.', it: 'VOL SEQ', pt: 'VOL SEQ' },
    'transport.volPads': { sources: ['VOL PADS'], ca: 'VOL PADS', es: 'VOL PADS', eu: 'PAD BOL', gl: 'VOL PADS', en: 'PAD VOL', fr: 'VOL PADS', de: 'PAD-LAUTST.', it: 'VOL PAD', pt: 'VOL PADS' },
    'tabs.sequencer': { sources: ['SEQUENCER'], ca: 'SEQÜENCIADOR', es: 'SECUENCIADOR', eu: 'SEKUENTZIADOREA', gl: 'SECUENCIADOR', en: 'SEQUENCER', fr: 'SÉQUENCEUR', de: 'SEQUENCER', it: 'SEQUENCER', pt: 'SEQUENCIADOR' },
    'tabs.mixer': { sources: ['MIXER'], ca: 'MESCLADOR', es: 'MEZCLADOR', eu: 'NAHASTAILUA', gl: 'MIXER', en: 'MIXER', fr: 'MIXEUR', de: 'MIXER', it: 'MIXER', pt: 'MISTURADOR' },
    'tabs.melody': { sources: ['MELODÍA', 'MELODY STUDIO'], ca: 'MELODIA', es: 'MELODÍA', eu: 'MELODIA', gl: 'MELODÍA', en: 'MELODY', fr: 'MÉLODIE', de: 'MELODIE', it: 'MELODIA', pt: 'MELODIA' },
    'tabs.more': { sources: ['MÁS'], ca: 'MÉS', es: 'MÁS', eu: 'GEHIAGO', gl: 'MÁIS', en: 'MORE', fr: 'PLUS', de: 'MEHR', it: 'ALTRO', pt: 'MAIS' },
    'tabs.path': { sources: ['PATH', 'PATH · PATCHBAY'], ca: 'PATH', es: 'PATH', eu: 'PATH', gl: 'PATH', en: 'PATH', fr: 'PATH', de: 'PATH', it: 'PATH', pt: 'PATH' },
    'tabs.jam': { sources: ['Jam'], ca: 'Jam', es: 'Jam', eu: 'Jam', gl: 'Jam', en: 'Jam', fr: 'Jam', de: 'Jam', it: 'Jam', pt: 'Jam' },
    'tabs.filters': { sources: ['Filtros', 'FILTERS'], ca: 'Filtres', es: 'Filtros', eu: 'Iragazkiak', gl: 'Filtros', en: 'Filters', fr: 'Filtres', de: 'Filter', it: 'Filtri', pt: 'Filtros' },
    'pads.synced': { sources: ['SYNCED', '🔗 SYNCED'], ca: 'SINCRONITZAT', es: 'SINCRONIZADO', eu: 'SINKRONIZATUTA', gl: 'SINCRONIZADO', en: 'SYNCED', fr: 'SYNCHRO', de: 'SYNCHRONISIERT', it: 'SINCRONIZZATO', pt: 'SINCRONIZADO' },
    'pads.syncLeds': { sources: ['SYNC LEDS', '💡 SYNC LEDS'], ca: 'SINCRONITZA LEDS', es: 'SINCRONIZAR LEDS', eu: 'LED-AK SINKRONIZATU', gl: 'SINCRONIZAR LEDS', en: 'SYNC LEDS', fr: 'SYNCHRO LEDS', de: 'LEDS SYNCHRONISIEREN', it: 'SINCRONIZZA LED', pt: 'SINCRONIZAR LEDS' },
    'pads.grid': { sources: ['GRID'], ca: 'GRAELLA', es: 'CUADRÍCULA', eu: 'SARETA', gl: 'GRELLA', en: 'GRID', fr: 'GRILLE', de: 'RASTER', it: 'GRIGLIA', pt: 'GRELHA' },
    'pads.solo': { sources: ['SOLO'], ca: 'SOLO', es: 'SOLO', eu: 'SOLO', gl: 'SOLO', en: 'SOLO', fr: 'SOLO', de: 'SOLO', it: 'SOLO', pt: 'SOLO' },
    'pads.extra': { sources: ['XTRA PADS'], ca: 'PADS EXTRA', es: 'PADS EXTRA', eu: 'PADS EXTRA', gl: 'PADS EXTRA', en: 'XTRA PADS', fr: 'PADS XTRA', de: 'XTRA-PADS', it: 'PAD EXTRA', pt: 'PADS EXTRA' },
    'actions.clear': { sources: ['CLEAR', 'Limpiar', '🗑️ Clear'], ca: 'NETEJA', es: 'LIMPIAR', eu: 'GARBITU', gl: 'LIMPAR', en: 'CLEAR', fr: 'EFFACER', de: 'LÖSCHEN', it: 'PULISCI', pt: 'LIMPAR' },
    'actions.save': { sources: ['Guardar', '💾 Guardar'], ca: 'Desa', es: 'Guardar', eu: 'Gorde', gl: 'Gardar', en: 'Save', fr: 'Enregistrer', de: 'Speichern', it: 'Salva', pt: 'Guardar' },
    'actions.load': { sources: ['Cargar escena'], ca: 'Carrega escena', es: 'Cargar escena', eu: 'Eszena kargatu', gl: 'Cargar escena', en: 'Load scene', fr: 'Charger la scène', de: 'Szene laden', it: 'Carica scena', pt: 'Carregar cena' },
    'actions.upload': { sources: ['SUBIR WAV', '📁 Subir WAV/MP3 desde mi dispositivo'], ca: 'PUJA WAV', es: 'SUBIR WAV', eu: 'WAV IGO', gl: 'SUBIR WAV', en: 'UPLOAD WAV', fr: 'TÉLÉVERSER WAV', de: 'WAV HOCHLADEN', it: 'CARICA WAV', pt: 'CARREGAR WAV' },
    'actions.cancel': { sources: ['Cancelar', '✖ Cancelar'], ca: 'Cancel·la', es: 'Cancelar', eu: 'Utzi', gl: 'Cancelar', en: 'Cancel', fr: 'Annuler', de: 'Abbrechen', it: 'Annulla', pt: 'Cancelar' },
    'actions.refresh': { sources: ['↺ Refresh', '↻ Refresh', '🔄 RECARGAR CONTEOS'], ca: '↻ Actualitza', es: '↻ Actualizar', eu: '↻ Freskatu', gl: '↻ Actualizar', en: '↻ Refresh', fr: '↻ Actualiser', de: '↻ Aktualisieren', it: '↻ Aggiorna', pt: '↻ Atualizar' },
    'settings.focus': { sources: ['MODO FOCO'], ca: 'MODE FOCUS', es: 'MODO FOCO', eu: 'FOKU MODUA', gl: 'MODO FOCO', en: 'FOCUS MODE', fr: 'MODE FOCUS', de: 'FOKUSMODUS', it: 'MODALITÀ FOCUS', pt: 'MODO FOCO' },
    'settings.theme': { sources: ['🎨 Tema visual', 'COLOR MODE'], ca: '🎨 Tema visual', es: '🎨 Tema visual', eu: '🎨 Ikus-gaia', gl: '🎨 Tema visual', en: '🎨 Visual theme', fr: '🎨 Thème visuel', de: '🎨 Darstellungsthema', it: '🎨 Tema visivo', pt: '🎨 Tema visual' },
    'mobile.pads': { sources: ['🥁 Pads'], ca: '🥁 Pads', es: '🥁 Pads', eu: '🥁 Pad-ak', gl: '🥁 Pads', en: '🥁 Pads', fr: '🥁 Pads', de: '🥁 Pads', it: '🥁 Pad', pt: '🥁 Pads' },
    'mobile.piano': { sources: ['🎹 ¡Toca las teclas!'], ca: '🎹 Toca les tecles!', es: '🎹 ¡Toca las teclas!', eu: '🎹 Jo teklak!', gl: '🎹 Toca as teclas!', en: '🎹 Play the keys!', fr: '🎹 Jouez les touches !', de: '🎹 Spiele die Tasten!', it: '🎹 Suona i tasti!', pt: '🎹 Toca nas teclas!' },
    'mobile.seq': { sources: ['Seq'], ca: 'Seq', es: 'Seq', eu: 'Seq', gl: 'Seq', en: 'Seq', fr: 'Seq', de: 'Seq', it: 'Seq', pt: 'Seq' },
    'mobile.fx': { sources: ['✨ Elige un efecto'], ca: '✨ Tria un efecte', es: '✨ Elige un efecto', eu: '✨ Aukeratu efektu bat', gl: '✨ Escolle un efecto', en: '✨ Choose an effect', fr: '✨ Choisissez un effet', de: '✨ Effekt wählen', it: '✨ Scegli un effetto', pt: '✨ Escolhe um efeito' },
    'mobile.tone': { sources: ['🎚️ Tono'], ca: '🎚️ To', es: '🎚️ Tono', eu: '🎚️ Tonua', gl: '🎚️ Ton', en: '🎚️ Tone', fr: '🎚️ Ton', de: '🎚️ Klang', it: '🎚️ Tono', pt: '🎚️ Tom' },
    'mobile.sounds': { sources: ['📁 Sonidos'], ca: '📁 Sons', es: '📁 Sonidos', eu: '📁 Soinuak', gl: '📁 Sons', en: '📁 Sounds', fr: '📁 Sons', de: '📁 Sounds', it: '📁 Suoni', pt: '📁 Sons' },
    'admin.system': { sources: ['SYSTEM', '💻 SYSTEM'], ca: 'SISTEMA', es: 'SISTEMA', eu: 'SISTEMA', gl: 'SISTEMA', en: 'SYSTEM', fr: 'SYSTÈME', de: 'SYSTEM', it: 'SISTEMA', pt: 'SISTEMA' },
    'admin.firmware': { sources: ['Firmware'], ca: 'Firmware', es: 'Firmware', eu: 'Firmware', gl: 'Firmware', en: 'Firmware', fr: 'Firmware', de: 'Firmware', it: 'Firmware', pt: 'Firmware' },
    'admin.memory': { sources: ['MEMORY', 'Memory Used'], ca: 'MEMÒRIA', es: 'MEMORIA', eu: 'MEMORIA', gl: 'MEMORIA', en: 'MEMORY', fr: 'MÉMOIRE', de: 'SPEICHER', it: 'MEMORIA', pt: 'MEMÓRIA' },
    'admin.reboot': { sources: ['⏻ Reboot'], ca: '⏻ Reinicia', es: '⏻ Reiniciar', eu: '⏻ Berrabiarazi', gl: '⏻ Reiniciar', en: '⏻ Reboot', fr: '⏻ Redémarrer', de: '⏻ Neustart', it: '⏻ Riavvia', pt: '⏻ Reiniciar' },
    'common.all': { sources: ['Todos', 'ALL'], ca: 'Tots', es: 'Todos', eu: 'Denak', gl: 'Todos', en: 'All', fr: 'Tous', de: 'Alle', it: 'Tutti', pt: 'Todos' },
    'common.none': { sources: ['NONE'], ca: 'CAP', es: 'NINGUNO', eu: 'BAT ERE EZ', gl: 'NINGÚN', en: 'NONE', fr: 'AUCUN', de: 'KEINER', it: 'NESSUNO', pt: 'NENHUM' },
    'common.ready': { sources: ['READY'], ca: 'PREPARAT', es: 'LISTO', eu: 'PREST', gl: 'LISTO', en: 'READY', fr: 'PRÊT', de: 'BEREIT', it: 'PRONTO', pt: 'PRONTO' },
    'common.off': { sources: ['OFF'], ca: 'OFF', es: 'OFF', eu: 'OFF', gl: 'OFF', en: 'OFF', fr: 'OFF', de: 'AUS', it: 'OFF', pt: 'OFF' },
    'common.on': { sources: ['ON', '● MACROS ON'], ca: 'ON', es: 'ON', eu: 'ON', gl: 'ON', en: 'ON', fr: 'ON', de: 'AN', it: 'ON', pt: 'ON' }
  };

  var sourceToKey = Object.create(null);
  Object.keys(CATALOG).forEach(function (key) {
    var item = CATALOG[key];
    (item.sources || []).concat(ORDER.map(function (lang) { return item[lang]; })).forEach(function (source) {
      if (source) sourceToKey[normalize(source)] = key;
    });
  });

  function normalize(value) {
    return String(value == null ? '' : value).replace(/\s+/g, ' ').trim();
  }
  function detectLocale() {
    var query = new URLSearchParams(window.location.search).get('lang');
    if (ORDER.indexOf(query) !== -1) return query;
    try {
      var stored = localStorage.getItem('red808_locale');
      if (ORDER.indexOf(stored) !== -1) return stored;
    } catch (_) {}
    var language = String(navigator.language || 'es').toLowerCase().slice(0, 2);
    return ORDER.indexOf(language) !== -1 ? language : 'es';
  }
  var locale = detectLocale();
  function t(key, vars) {
    var item = CATALOG[key];
    var value = item ? (item[locale] || item.es || item.en) : key;
    if (vars) Object.keys(vars).forEach(function (name) { value = value.replace(new RegExp('\\{' + name + '\\}', 'g'), vars[name]); });
    return value;
  }
  function translateTextNode(node) {
    var raw = node.nodeValue;
    var trimmed = normalize(raw);
    var key = sourceToKey[trimmed];
    if (!key) return;
    var translated = t(key);
    var start = raw.search(/\S|$/);
    var end = raw.search(/\s*$/);
    node.nodeValue = raw.slice(0, start) + translated + raw.slice(end);
  }
  function apply(root) {
    if (!root) return;
    var walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    var node;
    while ((node = walker.nextNode())) {
      var parent = node.parentElement;
      if (parent && !/^(SCRIPT|STYLE|OPTION)$/i.test(parent.tagName)) translateTextNode(node);
    }
    root.querySelectorAll && root.querySelectorAll('[title],[placeholder],[aria-label]').forEach(function (element) {
      ['title', 'placeholder', 'aria-label'].forEach(function (attribute) {
        var value = element.getAttribute(attribute);
        var key = sourceToKey[normalize(value)];
        if (key) element.setAttribute(attribute, t(key));
      });
    });
    var selector = document.getElementById('red808-locale');
    if (selector) selector.value = locale;
    document.documentElement.lang = locale;
  }
  function setLocale(next) {
    if (ORDER.indexOf(next) === -1 || next === locale) return;
    locale = next;
    try { localStorage.setItem('red808_locale', locale); } catch (_) {}
    apply(document.body);
    window.dispatchEvent(new CustomEvent('red808:localechange', { detail: { locale: locale } }));
  }
  function init() {
    apply(document.body);
    if (document.body && window.MutationObserver) {
      new MutationObserver(function (records) {
        records.forEach(function (record) { record.addedNodes.forEach(function (node) { if (node.nodeType === 1) apply(node); }); });
      }).observe(document.body, { childList: true, subtree: true });
    }
  }
  window.RED808I18N = { locales: LOCALES, order: ORDER.slice(), catalog: CATALOG, t: t, apply: apply, setLocale: setLocale, getLocale: function () { return locale; } };
  window.t = t;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init, { once: true });
  else init();
})();
