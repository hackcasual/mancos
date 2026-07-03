// Mancos service worker: app mode. The shell (HTML/JS/wasm) is precached
// under a per-build cache so JS and wasm never mix versions; bundles and
// pack manifests use stale-while-revalidate so a previously loaded pack
// works fully offline and refreshes in the background when online.
// __BUILD_VERSION__ is stamped by scripts/build-web.sh.
const VERSION = '__BUILD_VERSION__';
const SHELL_CACHE = `mancos-shell-${VERSION}`;
const RUNTIME_CACHE = 'mancos-runtime';
const SHELL = [
  './', './index.html', './app.js', './worker.js',
  './mancos_web.js', './mancos_web.wasm',
  './bundler.html', './bundler.js', './bundler-worker.js',
  './mancos_bundler_web.js', './mancos_bundler_web.wasm',
  './manifest.webmanifest', './icons/icon-32.png', './icons/icon-192.png',
  './icons/icon-512.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
      caches.open(SHELL_CACHE)
          .then((cache) => cache.addAll(SHELL))
          .then(() => self.skipWaiting()));
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    for (const key of await caches.keys()) {
      if (key.startsWith('mancos-shell-') && key !== SHELL_CACHE) {
        await caches.delete(key);
      }
    }
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;
  const url = new URL(event.request.url);
  const isPackData = url.pathname.endsWith('.yafcbundle') ||
                     url.pathname.endsWith('/manifest.json');
  if (isPackData) {
    // Stale-while-revalidate (covers cross-origin mancos-data too).
    event.respondWith((async () => {
      const cache = await caches.open(RUNTIME_CACHE);
      const cached = await cache.match(event.request);
      const refresh = fetch(event.request).then((response) => {
        if (response.ok) cache.put(event.request, response.clone());
        return response;
      }).catch(() => cached);
      return cached ?? refresh;
    })());
    return;
  }
  // Shell: cache-first within this build's cache; everything else falls
  // through to the network.
  event.respondWith((async () => {
    const cached = await caches.match(event.request,
                                      { cacheName: SHELL_CACHE, ignoreSearch: true });
    return cached ?? fetch(event.request);
  })());
});
