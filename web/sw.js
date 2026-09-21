// Cache the app shell so the PWA opens instantly; API and WebSocket traffic always goes to the network.
const VERSION = 'pifire-v8';
const SHELL = ['/', '/index.html', '/app.js', '/style.css', '/uPlot.iife.min.js', '/uPlot.min.css', '/manifest.webmanifest', '/icon.svg', '/icon-180.png', '/icon-192.png',
  '/pages/home.js', '/pages/history.js', '/pages/cook.js', '/pages/settings.js', '/pages/more.js', '/pages/network.js', '/pages/pellets.js', '/pages/learning.js', '/pages/probes.js', '/icons.js'];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(VERSION).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((keys) => Promise.all(keys.filter((k) => k !== VERSION).map((k) => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== 'GET' || url.pathname.startsWith('/api/') || url.pathname === '/ws') return;
  // network first, fall back to cache (keeps the shell fresh after an update)
  e.respondWith(fetch(e.request).then((r) => { const copy = r.clone(); caches.open(VERSION).then((c) => c.put(e.request, copy)); return r; })
    .catch(() => caches.match(e.request).then((r) => r || caches.match('/index.html'))));
});
