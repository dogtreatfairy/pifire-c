// Cache the app shell so the PWA opens instantly; API and WebSocket traffic always goes to the network.
/* Substituted by CMake at build time, so every release gets its own cache. */
const VERSION = 'pifire-@PF_VERSION@';
/* Without these the app is not an app: it is a page of unstyled links, which is exactly what a
   half-filled cache produced. They are cached with a retry and their failure fails the install. */
const CORE = ['/', '/index.html', '/app.js', '/style.css', '/icons.js'];
/* Everything else is worth having and survivable without: a missing chart library costs the
   history page, not the whole shell. */
const EXTRA = ['/uPlot.iife.min.js', '/uPlot.min.css', '/manifest.webmanifest', '/icon.svg', '/icon-180.png', '/icon-192.png',
  '/pages/home.js', '/pages/history.js', '/pages/cook.js', '/pages/settings.js', '/pages/more.js', '/pages/network.js',
  '/pages/pellets.js', '/pages/learning.js', '/pages/probes.js', '/pages/rules.js'];

/* addAll is all-or-nothing, so one file that 404s used to reject the whole install and the phone
   kept the previous shell for ever. Caching each file separately and swallowing every failure
   traded that for something worse: an install that "succeeded" with a hole in it. If the hole was
   style.css, and the next load caught a slow tunnel, the app came up as unstyled HTML -- and since
   activate had already deleted the previous cache, there was no older copy left to fall back on.
   So the core is required and retried, and anything else may fail quietly. */
self.addEventListener('install', (e) => {
  e.waitUntil((async () => {
    const c = await caches.open(VERSION);
    for (const u of CORE) {
      try { await c.add(u); } catch { await c.add(u); }   /* a second throw rejects the install */
    }
    await Promise.all(EXTRA.map((u) => c.add(u).catch(() => {})));
    await self.skipWaiting();
  })());
});

/* Do not throw away a working shell for one that is not finished. The old cache is only dropped
   once this version actually holds everything the app needs to render. */
self.addEventListener('activate', (e) => {
  e.waitUntil((async () => {
    const c = await caches.open(VERSION);
    const complete = (await Promise.all(CORE.map((u) => c.match(u)))).every(Boolean);
    if (complete) {
      const keys = await caches.keys();
      await Promise.all(keys.filter((k) => k !== VERSION).map((k) => caches.delete(k)));
    }
    await self.clients.claim();
  })());
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
    let net = await fromNet;
    if (!net) {
      /* Nothing cached and the first attempt came to nothing. One more try before giving up:
         failing a stylesheet leaves the app looking like raw HTML, which is worse than waiting. */
      try {
        const again = await fetch(e.request, { cache: 'reload' });
        if (usable(again)) {
          if (again.ok) caches.open(VERSION).then((c) => c.put(e.request, again.clone())).catch(() => {});
          net = again;
        }
      } catch { /* still nothing */ }
    }
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

/* A push from the grill. This is the only path that reaches a phone with the app closed: the push
   service wakes this worker, and the worker draws the notification. The payload is already
   decrypted by the browser by the time it arrives here. */
self.addEventListener('push', (e) => {
  let d = { title: 'PiFire', body: '', code: '' };
  try { if (e.data) d = { ...d, ...e.data.json() }; } catch { try { d.body = e.data ? e.data.text() : ''; } catch { /* nothing usable */ } }
  e.waitUntil(self.registration.showNotification(d.title || 'PiFire', {
    body: d.body || '',
    /* one notification per kind, so a condition that keeps reporting replaces itself rather than
       stacking up a column of identical rows */
    tag: d.code || 'pifire',
    renotify: true,
    icon: '/icon-192.png',
    badge: '/icon-192.png',
    data: { code: d.code || '' },
  }));
});

/* Apple can retire a subscription and expect a new one without the app being opened. */
self.addEventListener('pushsubscriptionchange', (e) => {
  e.waitUntil((async () => {
    try {
      const r = await fetch('/api/v1/push');
      const { key } = await r.json();
      if (!key) return;
      const sub = await self.registration.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: key });
      await fetch('/api/v1/push/subscribe', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(sub),
      });
    } catch { /* it will be re-made the next time the app is opened */ }
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
