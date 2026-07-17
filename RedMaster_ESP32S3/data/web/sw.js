/* Generated for data_gz by tools/prepare_data_gz.py. Keep the placeholders. */
'use strict';

const CACHE_NAME = 'red808-web-__RED808_CACHE_VERSION__';
const PRECACHE = __RED808_PRECACHE_MANIFEST__;
const STATIC_PATHS = new Set(__RED808_STATIC_MANIFEST__);
const NAVIGATION_CACHE_KEYS = {
  '/': '/index.html',
  '/index.html': '/index.html',
  '/patchbay': '/patchbay',
  '/patchbay.html': '/patchbay',
  '/multiview': '/multiview.html',
  '/multiview.html': '/multiview.html',
  '/gesture': '/gesture.html',
  '/gesture.html': '/gesture.html',
  '/gesture-pro': '/gesture-pro.html',
  '/gesture-pro.html': '/gesture-pro.html',
  '/mobile': '/mobile.html',
  '/mobile.html': '/mobile.html',
  '/adm': '/adm',
  '/admin.html': '/adm'
};

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => Promise.all(PRECACHE.map((asset) =>
        fetch(new Request(asset, { cache: 'reload' }))
          .then((response) => response.ok ? cache.put(asset, response) : undefined)
          .catch(() => undefined)
      )))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then((names) => Promise.all(names
        .filter((name) => name.startsWith('red808-web-') && name !== CACHE_NAME)
        .map((name) => caches.delete(name))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET' || request.headers.has('range')) return;

  const url = new URL(request.url);
  if (url.origin !== self.location.origin) return;

  if (request.mode === 'navigate') {
    const cacheKey = NAVIGATION_CACHE_KEYS[url.pathname];
    if (!cacheKey) return;
    event.respondWith(
      fetch(request)
        .then((response) => {
          if (!response.ok) throw new Error(`navigation ${response.status}`);
          const copy = response.clone();
          event.waitUntil(caches.open(CACHE_NAME).then((cache) => cache.put(cacheKey, copy)));
          return response;
        })
        .catch(() => caches.match(cacheKey)
          .then((cached) => cached || caches.match('/index.html')))
    );
    return;
  }

  const cacheKey = url.pathname;
  if (!STATIC_PATHS.has(cacheKey)) return;
  event.respondWith(
    caches.match(request).then((exact) => exact || caches.match(cacheKey, { ignoreSearch: true })).then((cached) => {
      const refresh = fetch(request)
        .then((response) => {
          if (!response.ok) return response;
          const copy = response.clone();
          event.waitUntil(caches.open(CACHE_NAME).then((cache) => cache.put(request, copy)));
          return response;
        })
        .catch(() => undefined);
      if (cached) {
        event.waitUntil(refresh);
        return cached;
      }
      return refresh.then((response) => response || Response.error());
    })
  );
});
