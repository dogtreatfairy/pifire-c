// Cache the app shell so the PWA opens instantly; API and WebSocket traffic always goes to the network.
/* Substituted by CMake at build time, so every release gets its own cache. */
const VERSION = 'pifire-@PF_VERSION@';
const SHELL = ['/', '/index.html', '/app.js', '/style.css', '/uPlot.iife.min.js', '/uPlot.min.css', '/manifest.webmanifest', '/icon.svg', '/icon-180.png', '/icon-192.png',
  '/pages/home.js', '/pages/history.js', '/pages/cook.js', '/pages/settings.js', '/pages/more.js', '/pages/network.js', '/pages/pellets.js', '/pages/learning.js', '/pages/probes.js', '/pages/rules.js', '/icons.js'];

/* addAll is all-or-nothing: one file that 404s and the whole install rejects, the new worker never
   activates, and the phone keeps being served the previous shell for ever. Cache them one at a
   time so a missing file costs that file and nothing else. */
self.addEventListener('install', (e) => {
  e.waitUntil((async () => {
    const c = await caches.open(VERSION);
    await Promise.all(SHELL.map((u) => c.add(u).catch(() => {})));
    await self.skipWaiting();
  })());
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

/* A response is not an answer just because it arrived. Tailscale's proxy answers a request made
   while the tunnel is still coming up with a gateway error, and this used to hand that straight to
   the page: the app showed a broken shell instead of the perfectly good one in the cache, on the
   one origin where it could not simply be reloaded. A server error falls back to the cache like any
   other failure. A 404 does not -- that is the server answering, and pretending otherwise would
   hide real mistakes behind stale files. */
const usable = (r) => r && (r.ok || (r.status >= 400 && r.status < 500));

self.addEventListener('fetch', (e) => {
  if (e.request.method !== 'GET') return;
  const url = new URL(e.request.url);
  /* only our own origin: somebody else's server is not ours to cache or to second-guess */
  if (url.origin !== self.location.origin) return;
  if (url.pathname.startsWith('/api/') || url.pathname === '/ws') return;
  e.respondWith((async () => {
    const cached = await caches.match(e.request);
    const fromNet = fetch(e.request).then((r) => {
      if (r && r.ok) caches.open(VERSION).then((c) => c.put(e.request, r.clone())).catch(() => {});
      return usable(r) ? r : null;
    }).catch(() => null);

    if (cached) {
      const raced = await Promise.race([fromNet, new Promise((res) => setTimeout(() => res(null), NET_PATIENCE_MS))]);
      return raced || cached;
    }
    const net = await fromNet;
    if (net) return net;
    /* Nothing cached and nothing usable from the network. For a page, the shell we already have
       beats the browser's error screen, which on a Home Screen app is a dead white rectangle with
       no way back. */
    if (e.request.mode === 'navigate') {
      const shell = (await caches.match('/index.html')) || (await caches.match('/'));
      if (shell) return shell;
    }
    return Response.error();
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
