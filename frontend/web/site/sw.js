// sw.js — keeps friend-local on the phone once it has been opened, so it
// opens with no connection, like an app. The copy here is served at
// once and a newer one fetched behind it, which shows on the next open.
// Only the site's own files: requests to the provider are never touched.
// Built into the site by ./build.sh site, which fills in the version.
const CACHE = 'friend-local-@@VERSION@@';
const FILES = ['./', 'manifest.webmanifest', 'icon-180.png', 'icon-192.png', 'icon-512.png'];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys()
    .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== self.location.origin) return;
  const key = req.mode === 'navigate' ? './' : req;
  e.respondWith(caches.open(CACHE).then(async (cache) => {
    const kept = await cache.match(key, { ignoreSearch: true });
    const fresh = fetch(req).then((res) => {
      if (res.ok) cache.put(key, res.clone());
      return res;
    });
    if (kept) { e.waitUntil(fresh.catch(() => {})); return kept; }
    return fresh;
  }));
});
