// Cache the app shell so the PWA opens instantly; API and WebSocket traffic always goes to the network.
const VERSION = 'pifire-v13';
const SHELL = ['/', '/index.html', '/app.js', '/style.css', '/uPlot.iife.min.js', '/uPlot.min.css', '/manifest.webmanifest', '/icon.svg', '/icon-180.png', '/icon-192.png',
  '/pages/home.js', '/pages/history.js', '/pages/cook.js', '/pages/settings.js', '/pages/more.js', '/pages/network.js', '/pages/pellets.js', '/pages/learning.js', '/pages/probes.js', '/pages/rules.js', '/icons.js'];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(VERSION).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((keys) => Promise.all(keys.filter((k) => k !== VERSION).map((k) => caches.delete(k)))).then(() => self.clients.claim()));
});
/* Network first, but only for as long as the network is worth waiting for.
 *
 * Waiting for the network to fail before reaching for the cache is the difference between an app
 * that opens instantly and one that appears to hang. A phone that has just woken leaves its
 * Tailscale tunnel re-handshaking, and a request into it does not fail, it simply never answers
 * until the operating system times out tens of seconds later. The whole point of caching the shell
 * is that we already have a perfectly good copy of it. So the network gets a couple of seconds to
 * be quicker than the cache, and after that the cache wins and the network response, when it
 * eventually lands, just refreshes the cache for next time. */
const NET_PATIENCE_MS = 2500;

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== 'GET' || url.pathname.startsWith('/api/') || url.pathname === '/ws') return;
  e.respondWith((async () => {
    const cached = await caches.match(e.request);
    const fromNet = fetch(e.request).then((r) => {
      if (r && r.ok) { const copy = r.clone(); caches.open(VERSION).then((c) => c.put(e.request, copy)); }
      return r;
    });
    fromNet.catch(() => {});                     /* a late failure must not surface as unhandled */
    if (!cached) return fromNet.catch(() => caches.match('/index.html'));
    const raced = await Promise.race([fromNet.catch(() => null), new Promise((res) => setTimeout(() => res(null), NET_PATIENCE_MS))]);
    return raced || cached;
  })());
});

/* Tapping a notification should bring the app forward rather than open a second copy. */
self.addEventListener('notificationclick', (e) => {
  e.notification.close();
  e.waitUntil((async () => {
    const all = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    for (const c of all) if ('focus' in c) return c.focus();
    if (self.clients.openWindow) return self.clients.openWindow('/');
  })());
});
