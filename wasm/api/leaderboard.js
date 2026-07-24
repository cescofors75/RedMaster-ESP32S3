// Ranking global de RayRunner — Vercel Serverless Function (Node.js, sin
// dependencias: solo `fetch`, ya global en el runtime Node 18+ de Vercel).
//
// Almacen: Upstash Redis, via su API REST — un sorted set (`raydrone:scores`)
// es la estructura de datos hecha justo para esto: ZADD para guardar una
// puntuacion, ZREVRANGE para traer el top ordenado en un unico comando.
// No hace falta SQL, ni un ORM, ni un servidor que mantener.
//
// Variables de entorno esperadas (las inyecta Vercel al conectar la
// integracion de Upstash desde el dashboard, pestana Storage). Se aceptan
// los dos nombres que ha usado Vercel para esto, por si acaso:
//   KV_REST_API_URL / KV_REST_API_TOKEN              (integracion "Vercel KV")
//   UPSTASH_REDIS_REST_URL / UPSTASH_REDIS_REST_TOKEN (Upstash directo)

const crypto = require('crypto');

const REST_URL = process.env.KV_REST_API_URL || process.env.UPSTASH_REDIS_REST_URL;
const REST_TOKEN = process.env.KV_REST_API_TOKEN || process.env.UPSTASH_REDIS_REST_TOKEN;
const SIGN_SECRET = process.env.RAYDRONE_SCORE_SECRET || REST_TOKEN;
const KEY = 'raydrone:scores';
const TOKEN_KEY = `${KEY}:token`;
const RATE_KEY = `${KEY}:rate`;
const MAX_KEEP = 200; // cuantas entradas se conservan en el almacen
const TOP_N = 10;     // cuantas se devuelven al pedir el ranking
const TOKEN_TTL_MS = 15 * 60 * 1000;
const TOKEN_TTL_SEC = Math.ceil(TOKEN_TTL_MS / 1000);
const POST_LIMIT_PER_MIN = 8;
const LEVELS = new Set([1, 2, 3, 4, 5, 6]);

// Records "leyenda": marcas base que siempre aparecen en el ranking (se
// fusionan con las reales de Upstash, ordenadas y sin duplicar). Mismo set
// que el cliente para que local y global coincidan.
const SEED = [
    { n: 'BRUZOS', s: 13792, l: 1, d: 1 },
    { n: 'THOR', s: 10230, l: 1, d: 2 },
];
function mergeSeed(list) {
    const merged = [...list, ...SEED].sort((a, b) => b.s - a.s);
    const seen = new Set(), out = [];
    for (const e of merged) { const k = `${e.n}|${e.s}`; if (!seen.has(k)) { seen.add(k); out.push(e); } }
    return out;
}

function b64url(s) {
    return Buffer.from(s).toString('base64url');
}

function signPayload(payload) {
    const body = b64url(JSON.stringify(payload));
    const sig = crypto.createHmac('sha256', SIGN_SECRET).update(body).digest('base64url');
    return `${body}.${sig}`;
}

function verifyToken(token) {
    if (typeof token !== 'string') return null;
    const parts = token.split('.');
    if (parts.length !== 2) return null;
    const expected = crypto.createHmac('sha256', SIGN_SECRET).update(parts[0]).digest('base64url');
    const a = Buffer.from(parts[1]);
    const b = Buffer.from(expected);
    if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return null;
    try {
        const payload = JSON.parse(Buffer.from(parts[0], 'base64url').toString('utf8'));
        if (!payload || typeof payload.n !== 'string' || typeof payload.i !== 'number') return null;
        const age = Date.now() - payload.i;
        if (age < 0 || age > TOKEN_TTL_MS) return null;
        return payload;
    } catch (_) {
        return null;
    }
}

function reqOrigin(req) {
    if (req.headers.origin) return req.headers.origin;
    const ref = req.headers.referer || req.headers.referrer || '';
    try {
        return ref ? new URL(ref).origin : '';
    } catch (_) {
        return '';
    }
}

function siteOrigin(req) {
    const proto = (req.headers['x-forwarded-proto'] || 'https').split(',')[0].trim();
    const host = (req.headers['x-forwarded-host'] || req.headers.host || '').split(',')[0].trim();
    return host ? `${proto}://${host}` : '';
}

function allowedOrigins(req) {
    const env = (process.env.RAYDRONE_ALLOWED_ORIGINS || '')
        .split(',')
        .map((s) => s.trim())
        .filter(Boolean);
    const same = siteOrigin(req);
    return same ? [same, ...env] : env;
}

function originAllowed(req) {
    const origin = reqOrigin(req);
    return !!origin && allowedOrigins(req).includes(origin);
}

function setCors(req, res) {
    const origin = reqOrigin(req);
    if (origin && allowedOrigins(req).includes(origin)) {
        res.setHeader('Access-Control-Allow-Origin', origin);
    }
    res.setHeader('Vary', 'Origin');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
}

function clientIp(req) {
    const fwd = req.headers['x-forwarded-for'];
    return (Array.isArray(fwd) ? fwd[0] : (fwd || '')).split(',')[0].trim()
        || req.socket?.remoteAddress
        || 'unknown';
}

async function checkPostRateLimit(req) {
    const bucket = Math.floor(Date.now() / 60000);
    const hash = crypto.createHash('sha256').update(clientIp(req)).digest('hex').slice(0, 16);
    const key = `${RATE_KEY}:${hash}:${bucket}`;
    const [count] = await redisPipeline([
        ['INCR', key],
        ['EXPIRE', key, 70],
    ]);
    return Number(count || 0) <= POST_LIMIT_PER_MIN;
}

function plausibleScore(score, level, issuedAt) {
    const elapsedSec = Math.max(0, (Date.now() - issuedAt) / 1000);
    // Generoso para no castigar una partida buena, pero evita POSTs instantaneos
    // con 999999 puntos desde scripts externos.
    const cap = 2500 + level * 6500 + elapsedSec * 900;
    return score <= Math.min(999999, cap);
}

function queryParam(req, name) {
    if (req.query && req.query[name] !== undefined) return req.query[name];
    try {
        const base = siteOrigin(req) || 'https://raydrone.local';
        return new URL(req.url || '', base).searchParams.get(name);
    } catch (_) {
        return null;
    }
}

// Ejecuta una tanda de comandos Redis via el endpoint /pipeline de Upstash
// (varios comandos en un unico viaje de red) y devuelve sus resultados en orden.
async function redisPipeline(commands) {
    const res = await fetch(`${REST_URL}/pipeline`, {
        method: 'POST',
        headers: { Authorization: `Bearer ${REST_TOKEN}`, 'Content-Type': 'application/json' },
        body: JSON.stringify(commands),
    });
    if (!res.ok) throw new Error(`Upstash respondio ${res.status}`);
    const data = await res.json();
    return data.map((r) => r.result);
}

// El nick lo escribe el usuario: nunca confiar en su forma. Se quitan los
// caracteres de control (el cliente solo hace trim+slice; esto es un
// cinturon extra por si la peticion no viniera de ese formulario) y se
// recorta a 12 - la misma cota que ya aplica el campo del cliente.
function sanitizeNick(n) {
    if (typeof n !== 'string') return 'anon';
    let clean = '';
    for (const ch of n) {
        const c = ch.codePointAt(0);
        if (c >= 0x20 && c !== 0x7f) clean += ch;
    }
    clean = clean.trim().slice(0, 12);
    return clean || 'anon';
}

module.exports = async (req, res) => {
    setCors(req, res);
    if (req.method === 'OPTIONS') {
        res.status(originAllowed(req) ? 204 : 403).end();
        return;
    }

    if (!REST_URL || !REST_TOKEN) {
        // Sin la integracion de Upstash conectada todavia en este proyecto de
        // Vercel: el cliente hace fallback a su ranking local (ver game.html).
        res.status(503).json({ error: 'Ranking global no configurado (falta Upstash).' });
        return;
    }

    try {
        if (req.method === 'GET') {
            if (queryParam(req, 'start') === '1') {
                if (!originAllowed(req)) { res.status(403).json({ error: 'origen no permitido' }); return; }
                const nonce = crypto.randomBytes(16).toString('base64url');
                const issuedAt = Date.now();
                const token = signPayload({ n: nonce, i: issuedAt });
                await redisPipeline([['SET', `${TOKEN_KEY}:${nonce}`, '1', 'EX', TOKEN_TTL_SEC, 'NX']]);
                res.status(200).json({ token, issuedAt, ttlMs: TOKEN_TTL_MS });
                return;
            }
            // Se piden más de las que se devuelven porque al fusionar los records
            // "leyenda" y deduplicar podría recortarse el top real.
            const [entries] = await redisPipeline([['ZREVRANGE', KEY, 0, TOP_N + SEED.length]]);
            const real = (entries || []).map((m) => { try { return JSON.parse(m); } catch (_) { return null; } }).filter(Boolean);
            const top = mergeSeed(real).slice(0, TOP_N);
            res.status(200).json({ top });
            return;
        }

        if (req.method === 'POST') {
            if (!originAllowed(req)) { res.status(403).json({ error: 'origen no permitido' }); return; }
            if (!(await checkPostRateLimit(req))) { res.status(429).json({ error: 'demasiados envios' }); return; }
            const body = typeof req.body === 'string' ? JSON.parse(req.body || '{}') : (req.body || {});
            const score = Math.max(0, Math.min(999999, Math.floor(Number(body.score))));
            if (!Number.isFinite(score)) { res.status(400).json({ error: 'score invalido' }); return; }
            const levelNum = Math.floor(Number(body.level));
            const level = LEVELS.has(levelNum) ? levelNum : 1;
            const token = verifyToken(body.token);
            if (!token) { res.status(403).json({ error: 'token de partida invalido' }); return; }
            if (!plausibleScore(score, level, token.i)) { res.status(400).json({ error: 'score fuera de rango para la partida' }); return; }
            const tokenKey = `${TOKEN_KEY}:${token.n}`;
            const [seen] = await redisPipeline([['GET', tokenKey]]);
            if (!seen) { res.status(409).json({ error: 'token de partida usado o caducado' }); return; }
            const nick = sanitizeNick(body.nick);
            const entry = { n: nick, s: score, l: level, d: Date.now() };
            const member = JSON.stringify(entry);
            const [, , , rank, total] = await redisPipeline([
                ['DEL', tokenKey],
                ['ZADD', KEY, score, member],
                ['ZREMRANGEBYRANK', KEY, 0, -(MAX_KEEP + 1)], // conserva solo las MAX_KEEP mejores
                ['ZREVRANK', KEY, member],
                ['ZCARD', KEY],
            ]);
            res.status(200).json({ pos: (rank ?? 0) + 1, total: total ?? 1 });
            return;
        }

        res.status(405).json({ error: 'metodo no soportado' });
    } catch (err) {
        res.status(502).json({ error: 'Upstash no disponible', detail: String(err) });
    }
};
