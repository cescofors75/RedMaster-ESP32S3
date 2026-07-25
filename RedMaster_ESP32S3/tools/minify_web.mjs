import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { transform } from 'esbuild';
import { minify as minifyHtml } from 'html-minifier-terser';

const webDir = path.resolve(process.argv[2] || 'data_gz/web');
const pagePlans = {
  'index.html': {
    css: 'style.css',
    js: 'app.js',
    // La portada queda pequeña y carga cada CSS y luego app.js de uno en uno.
    sequentialCss: true,
  },
  'patchbay.html': { css: 'patchbay.css', js: 'patchbay.js' },
  'multiview.html': { css: 'multiview.css', js: 'multiview.js' },
  'gesture.html': { css: 'gesture-styles.css', js: 'gesture.js' },
  'gesture-pro.html': { css: 'gesture-pro.css', js: 'gesture-pro.js' },
  // Demo-first shell: one compressed response is considerably more reliable
  // than three parallel LittleFS/AsyncTCP transfers on the ESP32 AP.
  'mobile.html': { css: 'mobile.css', js: 'mobile.js', inline: true },
  'admin.html': { css: 'admin.css', js: 'admin.js' },
};

const localName = (url) => path.basename(url.split(/[?#]/, 1)[0]);

async function collectFiles(dir) {
  const entries = await fs.readdir(dir, { withFileTypes: true });
  const nested = await Promise.all(entries.map(async (entry) => {
    const fullPath = path.join(dir, entry.name);
    return entry.isDirectory() ? collectFiles(fullPath) : [fullPath];
  }));
  return nested.flat();
}

const consumed = new Set();
const outputs = new Set();
// Archivos referenciados por páginas con sequentialCss (p.ej. theme-vars.css
// en index.html) deben seguir sirviéndose sueltos: aunque otra página los
// consuma dentro de su bundle, no hay que borrarlos en la limpieza final.
const sequentialAssets = new Set();

for (const [htmlName, plan] of Object.entries(pagePlans)) {
  const htmlPath = path.join(webDir, htmlName);
  let html = await fs.readFile(htmlPath, 'utf8');
  // i18n debe ejecutarse antes de nav.js para que el selector y el menú común
  // nazcan ya en el idioma guardado, también en las páginas secundarias.
  if (!/\bi18n\.js(?:[?#"'])/i.test(html)) {
    html = html.replace(/<head>/i, '<head>\n<script src="i18n.js"></script>');
  }
  const cssParts = [];
  const deferredCssParts = [];
  const cssAssets = [];
  const jsParts = [];

  html = await replaceAsync(html, /<link\b[^>]*>/gi, async (tag) => {
    if (!/\brel\s*=\s*(["'])stylesheet\1/i.test(tag)) return tag;
    const href = tag.match(/\bhref\s*=\s*(["'])(.*?)\1/i)?.[2];
    if (!href || /^(?:data:|https?:)/i.test(href)) return tag;
    const name = localName(href);
    if (plan.sequentialCss) {
      cssAssets.push(`/${name}`);
      sequentialAssets.add(name);
      return '';
    }
    const file = path.join(webDir, name);
    try {
      const source = await fs.readFile(file, 'utf8');
      (/\bmedia\s*=\s*(["'])print\1/i.test(tag) && plan.deferredCss
        ? deferredCssParts : cssParts).push(`/* ${name} */\n${source}`);
      consumed.add(name);
      return '';
    } catch {
      return tag;
    }
  });

  html = await replaceAsync(html, /<style\b[^>]*>([\s\S]*?)<\/style>/gi, async (tag, body) => {
    cssParts.push(body);
    return '';
  });

  html = await replaceAsync(html, /<script\b[^>]*\bsrc\s*=\s*(["'])(.*?)\1[^>]*>\s*<\/script>/gi,
    async (tag, _quote, src) => {
      if (/^(?:data:|https?:)/i.test(src)) return tag;
      const name = localName(src);
      const file = path.join(webDir, name);
      try {
        jsParts.push(`/* ${name} */\n${await fs.readFile(file, 'utf8')}`);
        consumed.add(name);
        return '';
      } catch {
        return tag;
      }
    });

  html = await replaceAsync(html, /<script\b(?![^>]*\bsrc\s*=)[^>]*>([\s\S]*?)<\/script>/gi,
    async (_tag, body) => {
      if (body.trim()) jsParts.push(body);
      return '';
    });

  if (cssParts.length) {
    if (plan.inline) {
      const coreAttr = htmlName === 'index.html' ? ' data-red808-core' : '';
      html = html.replace('</head>', `  <style${coreAttr}>${cssParts.join('\n')}</style>\n</head>`);
    } else {
      await fs.writeFile(path.join(webDir, plan.css), cssParts.join('\n'), 'utf8');
      outputs.add(plan.css);
      const coreCssTag = htmlName === 'index.html'
        ? `  <link rel="stylesheet" href="/${plan.css}" data-red808-core>\n`
        : `  <link rel="stylesheet" href="/${plan.css}">\n`;
      html = html.replace('</head>', `${coreCssTag}</head>`);
    }
  }
  if (deferredCssParts.length && plan.deferredCss) {
    await fs.writeFile(path.join(webDir, plan.deferredCss), deferredCssParts.join('\n'), 'utf8');
    outputs.add(plan.deferredCss);
    html = html.replace('</head>', `  <link rel="stylesheet" href="/${plan.deferredCss}" media="print" onload="this.media='all'">\n</head>`);
  }
  if (jsParts.length) {
    if (plan.inline) {
      html = html.replace('</body>', `  <script>${jsParts.join('\n;\n')}</script>\n</body>`);
    } else {
      await fs.writeFile(path.join(webDir, plan.js), jsParts.join('\n;\n'), 'utf8');
      outputs.add(plan.js);
    }
    if (!plan.inline && !plan.sequentialCss) {
      html = html.replace('</body>', `  <script src="/${plan.js}" defer></script>\n</body>`);
    }
  }
  if (plan.sequentialCss) {
    const loader = `
  <style data-red808-loader>
    html{background:#09090b;color:#fff}body{opacity:0}
    html::before{content:'RED808 · CARGANDO';position:fixed;inset:0;z-index:2147483647;display:grid;place-items:center;background:#09090b;color:#ff3038;font:700 18px monospace;letter-spacing:.12em}
    html.red808-ready body{opacity:1}html.red808-ready::before{display:none}
    html.red808-failed::before{content:'RED808 · ERROR DE CARGA · RECARGA LA PAGINA';color:#ff453a}
  </style>
  <script>
  (()=>{
    const css=${JSON.stringify(cssAssets)}, app='/${plan.js}';
    const retry=(make,attempt=0)=>new Promise((resolve,reject)=>{
      const node=make();
      node.onload=()=>resolve(node);
      node.onerror=()=>{node.remove();if(attempt<3)setTimeout(()=>retry(make,attempt+1).then(resolve,reject),250*(attempt+1));else reject(new Error('asset'));};
      document.head.appendChild(node);
    });
    const loadCss=href=>retry(()=>{const node=document.createElement('link');node.rel='stylesheet';node.href=href;return node;});
    const loadJs=src=>retry(()=>{const node=document.createElement('script');node.src=src;return node;});
    const start=async()=>{try{for(const href of css)await loadCss(href);await loadJs(app);document.documentElement.classList.add('red808-ready');}catch(error){console.error('[RED808] carga inicial fallida',error);document.documentElement.classList.add('red808-failed');}};
    if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',start,{once:true});else start();
  })();
  </script>`;
    html = html.replace('</body>', `${loader}\n</body>`);
  }
  await fs.writeFile(htmlPath, html, 'utf8');
}

for (const name of consumed) {
  if (!outputs.has(name) && !sequentialAssets.has(name)) {
    await fs.rm(path.join(webDir, name), { force: true });
  }
}

const codeFiles = (await collectFiles(webDir)).filter((file) => /\.(?:js|css)$/i.test(file));
let bytesBefore = 0;
let bytesAfter = 0;
for (const file of codeFiles) {
  const source = await fs.readFile(file, 'utf8');
  const loader = path.extname(file).toLowerCase() === '.css' ? 'css' : 'js';
  const result = await transform(source, {
    loader,
    minify: true,
    legalComments: 'none',
    charset: 'utf8',
    target: loader === 'css' ? ['chrome80', 'safari13'] : ['es2018'],
  });
  await fs.writeFile(file, result.code, 'utf8');
  bytesBefore += Buffer.byteLength(source);
  bytesAfter += Buffer.byteLength(result.code);
}

for (const htmlName of Object.keys(pagePlans)) {
  const htmlPath = path.join(webDir, htmlName);
  let html = await fs.readFile(htmlPath, 'utf8');
  html = await minifyHtml(html, {
    collapseWhitespace: true,
    conservativeCollapse: true,
    removeComments: true,
    removeRedundantAttributes: true,
    minifyCSS: true,
    minifyJS: true,
  });
  await fs.writeFile(htmlPath, `${html}\n`, 'utf8');
}

const saved = bytesBefore - bytesAfter;
const percent = bytesBefore ? (saved * 100 / bytesBefore).toFixed(1) : '0.0';
console.log(`[minify_web] ${codeFiles.length} JS/CSS: ${bytesBefore} -> ${bytesAfter} bytes (-${percent}%)`);

async function replaceAsync(input, regex, replacer) {
  const matches = [...input.matchAll(regex)];
  if (!matches.length) return input;
  // Los bundles CSS/JS deben respetar exactamente el orden del HTML. Promise.all
  // hacía depender la cascada del orden de finalización de las lecturas.
  const replacements = [];
  for (const match of matches) replacements.push(await replacer(...match));
  let out = '';
  let cursor = 0;
  matches.forEach((match, index) => {
    out += input.slice(cursor, match.index) + replacements[index];
    cursor = match.index + match[0].length;
  });
  return out + input.slice(cursor);
}
