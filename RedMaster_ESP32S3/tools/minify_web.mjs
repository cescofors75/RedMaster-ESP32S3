import crypto from 'node:crypto';
import fs from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { transform } from 'esbuild';
import { minify as minifyHtml } from 'html-minifier-terser';

const webDir = path.resolve(process.argv[2] || 'data_gz/web');
const pagePlans = {
  'index.html': {
    css: 'style.css',
    deferredCss: 'daisy-controls.css',
    js: 'app.js',
  },
  'patchbay.html': { css: 'patchbay.css', js: 'patchbay.js' },
  'multiview.html': { css: 'multiview.css', js: 'multiview.js' },
  'gesture.html': { css: 'gesture-styles.css', js: 'gesture.js' },
  'gesture-pro.html': { css: 'gesture-pro.css', js: 'gesture-pro.js' },
  'mobile.html': { css: 'mobile.css', js: 'mobile.js' },
  'admin.html': { css: 'admin.css', js: 'admin.js' },
};

const localName = (url) => path.basename(url.split(/[?#]/, 1)[0]);
const hash = (data, length = 12) => crypto.createHash('sha256').update(data).digest('hex').slice(0, length);

async function collectFiles(dir) {
  const entries = await fs.readdir(dir, { withFileTypes: true });
  const nested = await Promise.all(entries.map(async (entry) => {
    const fullPath = path.join(dir, entry.name);
    return entry.isDirectory() ? collectFiles(fullPath) : [fullPath];
  }));
  return nested.flat();
}

const initialFiles = await collectFiles(webDir);
const sourceFingerprint = crypto.createHash('sha256');
for (const file of initialFiles.sort()) {
  sourceFingerprint.update(path.relative(webDir, file));
  sourceFingerprint.update(await fs.readFile(file));
}
const deferredVersion = sourceFingerprint.digest('hex').slice(0, 12);
const appPath = path.join(webDir, 'app.js');
let appSource = await fs.readFile(appPath, 'utf8');
appSource = appSource.replace(
  /const DEFERRED_ASSET_VERSION\s*=\s*['"][^'"]+['"]\s*;/,
  `const DEFERRED_ASSET_VERSION = '${deferredVersion}';`,
);
await fs.writeFile(appPath, appSource, 'utf8');

const consumed = new Set();
const outputs = new Set(['nav.js']);

for (const [htmlName, plan] of Object.entries(pagePlans)) {
  const htmlPath = path.join(webDir, htmlName);
  let html = await fs.readFile(htmlPath, 'utf8');
  const cssParts = [];
  const deferredCssParts = [];
  const jsParts = [];

  html = await replaceAsync(html, /<link\b[^>]*>/gi, async (tag) => {
    if (!/\brel\s*=\s*(["'])stylesheet\1/i.test(tag)) return tag;
    const href = tag.match(/\bhref\s*=\s*(["'])(.*?)\1/i)?.[2];
    if (!href || /^(?:data:|https?:)/i.test(href)) return tag;
    const name = localName(href);
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
      if (name === 'nav.js') return '<script src="/nav.js"></script>';
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
    await fs.writeFile(path.join(webDir, plan.css), cssParts.join('\n'), 'utf8');
    outputs.add(plan.css);
    html = html.replace('</head>', `  <link rel="stylesheet" href="/${plan.css}">\n</head>`);
  }
  if (deferredCssParts.length && plan.deferredCss) {
    await fs.writeFile(path.join(webDir, plan.deferredCss), deferredCssParts.join('\n'), 'utf8');
    outputs.add(plan.deferredCss);
    html = html.replace('</head>', `  <link rel="stylesheet" href="/${plan.deferredCss}" media="print" onload="this.media='all'">\n</head>`);
  }
  if (jsParts.length) {
    await fs.writeFile(path.join(webDir, plan.js), jsParts.join('\n;\n'), 'utf8');
    outputs.add(plan.js);
    html = html.replace('</body>', `  <script src="/${plan.js}" defer></script>\n</body>`);
  }
  await fs.writeFile(htmlPath, html, 'utf8');
}

for (const name of consumed) {
  if (!outputs.has(name)) await fs.rm(path.join(webDir, name), { force: true });
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

const versionedAssets = new Map();
for (const file of await collectFiles(webDir)) {
  if (/\.(?:js|css|ico)$/i.test(file)) {
    versionedAssets.set(path.basename(file), hash(await fs.readFile(file)));
  }
}

for (const htmlName of Object.keys(pagePlans)) {
  const htmlPath = path.join(webDir, htmlName);
  let html = await fs.readFile(htmlPath, 'utf8');
  html = html.replace(/\b(src|href)=(["'])([^"']+?)\2/gi, (whole, attr, quote, url) => {
    if (/^(?:data:|https?:)/i.test(url)) return whole;
    const name = localName(url);
    const version = versionedAssets.get(name);
    if (!version) return whole;
    return `${attr}=${quote}/${name}?v=${version}${quote}`;
  });
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

const finalFiles = (await collectFiles(webDir))
  .filter((file) => !/[\\/](?:sw\.js|build\.id)$/i.test(file))
  .sort();
const buildDigest = crypto.createHash('sha256');
for (const file of finalFiles) {
  buildDigest.update(path.relative(webDir, file));
  buildDigest.update(await fs.readFile(file));
}
await fs.writeFile(path.join(webDir, 'build.id'), `${buildDigest.digest('hex').slice(0, 16)}\n`, 'utf8');

const saved = bytesBefore - bytesAfter;
const percent = bytesBefore ? (saved * 100 / bytesBefore).toFixed(1) : '0.0';
console.log(`[minify_web] ${codeFiles.length} JS/CSS: ${bytesBefore} -> ${bytesAfter} bytes (-${percent}%)`);

async function replaceAsync(input, regex, replacer) {
  const matches = [...input.matchAll(regex)];
  if (!matches.length) return input;
  const replacements = await Promise.all(matches.map((match) => replacer(...match)));
  let out = '';
  let cursor = 0;
  matches.forEach((match, index) => {
    out += input.slice(cursor, match.index) + replacements[index];
    cursor = match.index + match[0].length;
  });
  return out + input.slice(cursor);
}
