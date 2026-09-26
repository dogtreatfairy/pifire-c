// PiFire UI core: API client, live status over WebSocket, router, shared widgets.
import { renderHome } from './pages/home.js';
import { renderHistory } from './pages/history.js';
import { renderCook } from './pages/cook.js';
import { renderProbes } from './pages/probes.js';
import { renderSettings } from './pages/settings.js';
import { icon as lucide, brandIcon, MODE_ICON, tileStyle } from './icons.js';
import { renderMore } from './pages/more.js';
import { renderNetwork } from './pages/network.js';

export const PF = {
  status: null,
  settings: null,
  units: 'F',
  listeners: new Set(),
  connected: false,
};

// ---------- API ----------
/* Every request gets a deadline.
 *
 * A request with no timeout is not merely slow, it is contagious. Over a link that has gone away
 * without saying so -- a phone that slept and left a Tailscale tunnel to re-handshake, a network
 * that changed underneath us -- the socket sits open until the operating system gives up, which is
 * tens of seconds. Meanwhile the status poll starts another one every few seconds, a browser only
 * allows about six connections to one host, and once they are all held by requests that will never
 * answer, nothing else can get out either. That is how a link that is actually back in a second or
 * two leaves the app unusable for minutes. A short deadline frees the connection instead. */
export async function api(path, opts = {}) {
  const ms = opts.timeout ?? 8000;
  const ac = new AbortController();
  const t = setTimeout(() => ac.abort(), ms);
  let r, j;
  try {
    r = await fetch('/api/v1' + path, {
      method: opts.method || (opts.body ? 'POST' : 'GET'),
      headers: opts.body ? { 'Content-Type': 'application/json' } : undefined,
      body: opts.body ? JSON.stringify(opts.body) : undefined,
      signal: ac.signal,
      cache: 'no-store',
    });
    /* The body is read inside the timeout, not after it. Headers arriving is not the same as the
       answer arriving: a tunnel that stalls mid-body leaves this await hanging for ever, and the
       timer that was meant to prevent exactly that had already been cleared. */
    j = await r.json().catch(() => ({}));
  } catch (e) {
    throw new Error(e.name === 'AbortError' ? 'The grill did not answer in time' : 'Could not reach the grill');
  } finally { clearTimeout(t); }
  if (!r.ok) throw new Error(j.message || `HTTP ${r.status}`);
  return j;
}
export const cmd = (c) => api('/cmd', { body: c }).catch((e) => toast(e.message, true));
export const patchSettings = async (group, obj) => {
  await api('/settings' + (group ? '/' + group.replace(/\./g, '/') : ''), { method: 'PATCH', body: obj });
  PF.settings = await api('/settings');
  applyTheme();
};

// ---------- units / formatting ----------
export const fmtTemp = (v, d = 0) => (v == null || Number.isNaN(v) ? '—' : Number(v).toFixed(d));
export const degUnit = () => (PF.units === 'C' ? '°C' : '°F');
export const fmtDur = (s) => {
  s = Math.max(0, Math.round(s));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return h ? `${h}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}` : `${m}:${String(sec).padStart(2, '0')}`;
};
export const fmtTime = (ts) => new Date(ts * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });

// ---------- live status ----------
// The socket is treated as suspect, never trusted: iOS suspends timers and quietly kills sockets when
// the app is in the background, so a returning app may hold a socket that is CONNECTING forever or
// OPEN-but-dead. A 2 s watchdog reconnects on any of those, every resume reconnects at once, and
// while the socket is down the status is polled over plain HTTP so the page never goes stale.
let ws, wsTimer, lastMsgAt = 0, connectingAt = 0, pollTimer = null;
function connect() {
  if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) return;
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  let sock;
  try { sock = new WebSocket(`${proto}://${location.host}/ws`); } catch { scheduleReconnect(); return; }
  ws = sock;
  connectingAt = Date.now();
  sock.onopen = () => { if (sock !== ws) return; PF.everConnected = true; PF.wsRetry = 0; lastMsgAt = Date.now(); setConnected(true); };
  sock.onclose = () => { if (sock !== ws) return; ws = null; setConnected(false); scheduleReconnect(); };
  sock.onerror = () => { try { sock.close(); } catch {} };
  sock.onmessage = (ev) => {
    if (sock !== ws) return;
    lastMsgAt = Date.now();
    const m = JSON.parse(ev.data);
    if (m.type === 'status') { PF.status = m; PF.units = m.units; emit(); }
    else if (m.type === 'error') toast(m.msg, true);
    else if (m.type === 'event') { PF.alertGen = (PF.alertGen || 0) + 1; alert(m); emit(); }
    else if (m.type === 'alarms') refreshAlarms();
  };
}
function scheduleReconnect() {
  clearTimeout(wsTimer);
  const wait = Math.min(5000, 500 * 2 ** Math.min(4, PF.wsRetry++ || 0));
  wsTimer = setTimeout(connect, wait);
}
function reconnectNow() {
  clearTimeout(wsTimer);
  PF.wsRetry = 0;
  const old = ws; ws = null;
  try { old?.close(); } catch {}
  connect();
}
setInterval(() => {
  if (document.hidden) return;
  const now = Date.now();
  if (!ws) { connect(); return; }
  if (ws.readyState === WebSocket.CONNECTING && now - connectingAt > 6000) reconnectNow();
  else if (ws.readyState === WebSocket.OPEN && now - lastMsgAt > 8000) reconnectNow();   /* the daemon pushes at least every 5 s */
  else if (ws.readyState === WebSocket.CLOSED) reconnectNow();
}, 2000);
/* Coming back from a sleeping phone, the socket has to be rebuilt and the tunnel underneath it may
   still be waking. Ask for the status over plain HTTP at the same time, so the screen is right as
   soon as anything gets through rather than only once the socket is up. */
function resumeNow() {
  setTimeout(reconnectNow, 150);
  pollStatus();
  checkVersion();
}

/* The daemon can be updated while this app is sitting in the background -- on a phone it may not be
 * launched from cold for days -- and the modules already running are then older than the grill they
 * are talking to. That is invisible and confusing: a setting that was fixed is still broken, and
 * reloading is not something anyone thinks to do to an app. So the running version is compared with
 * the daemon's whenever the app comes back to the front, not only when it starts.
 *
 * Reloading immediately would only fetch the shell the service worker already holds, so the new
 * worker is given a moment to install and take over first; if it does not, the reload still gets
 * fresh files, because the fetch handler prefers the network. */
let versionCheckAt = 0;
async function checkVersion() {
  if (document.hidden || Date.now() - versionCheckAt < 30000) return;
  versionCheckAt = Date.now();
  let sys;
  try { sys = await api('/system'); } catch { return; }            /* offline: keep what we have */
  if (!sys?.version) return;
  let seen = null;
  try { seen = localStorage.getItem('pf_version'); localStorage.setItem('pf_version', sys.version); } catch { /* storage unavailable */ }
  if (!seen || seen === sys.version) return;
  try {
    const regs = await navigator.serviceWorker?.getRegistrations?.() || [];
    await Promise.all(regs.map((r) => r.update().catch(() => {})));
    await Promise.race([navigator.serviceWorker?.ready, new Promise((r) => setTimeout(r, 3000))]);
  } catch { /* no service worker: the reload is enough */ }
  location.reload();
}
for (const ev of ['online', 'pageshow', 'focus']) window.addEventListener(ev, resumeNow);
document.addEventListener('visibilitychange', () => { if (!document.hidden) resumeNow(); });
/* One poll at a time. Without this the three-second timer keeps starting new ones on top of a
   request that has not answered yet, which is the pile-up the deadline above exists to prevent. */
let polling = false;
async function pollStatus() {
  if (document.hidden || polling) return;
  polling = true;
  try { const st = await api('/status', { timeout: 4000 }); PF.status = st; PF.units = st.units; emit(); }
  catch { /* still down */ }
  finally { polling = false; }
}
let lostTimer = null;
function setConnected(on) {
  PF.connected = on;
  paintNet();
  // the banner only appears after the link has been down for a while (a reconnect takes < 1 s and must not flash)
  clearTimeout(lostTimer);
  clearInterval(pollTimer); pollTimer = null;
  if (on) { PF.lost = false; emit(); }
  else { lostTimer = setTimeout(() => { PF.lost = true; emit(); }, 4000); pollTimer = setInterval(pollStatus, 3000); }
}
function emit() { for (const fn of PF.listeners) fn(PF.status); }
export function onStatus(fn) { PF.listeners.add(fn); return () => PF.listeners.delete(fn); }

// ---------- widgets ----------
const SVG_TAGS = new Set(['svg', 'path', 'circle', 'line', 'text', 'g']);
export function el(tag, attrs = {}, ...children) {
  const e = SVG_TAGS.has(tag) ? document.createElementNS('http://www.w3.org/2000/svg', tag) : document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') e.setAttribute('class', v);
    else if (k === 'html') e.innerHTML = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) if (c != null) e.append(c.nodeType ? c : document.createTextNode(String(c)));
  return e;
}
// ---------- notifications ----------
// Two layers: iOS-style banners that slide in under the header (auto-dismiss, errors stay until
// tapped), and a notification centre behind the bell in the header that keeps everything that
// matters (grill events, alarms, errors, failed actions) until the user clears it. The centre is
// persisted per device; events that happened while the app was closed are pulled from the daemon's
// event log on boot so nothing is missed.
const store = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} };
const load = (k, d) => { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } };
/* The list behind the bell is the daemon's alarm table, not a pile kept in this browser. That is
   what makes an alarm disappear by itself when the grill fixes it, and what makes clearing one on
   a phone clear it on the laptop. Banners stay local: a banner is a moment on one screen. */
PF.alarms = { alarms: [], unacked: 0, active: 0, worst: -1 };
let centreRender = null;
export async function refreshAlarms() {
  try {
    PF.alarms = await api('/alarms');
    updateBadge();
    centreRender?.();
    askAboutFixables();
  } catch { /* offline: keep showing what we last knew */ }
}
const KIND_ICON = { error: 'circle-x', warn: 'triangle-alert', ok: 'circle-check', info: 'info' };
/* The remedies an alarm may name, and what carrying one out means here. The daemon says what can
   be done about a condition; this is the only place that knows how to do it. */
const FIX_LABEL = { shutdown: 'Shut Down' };
const FIX_RUN = { shutdown: () => cmd({ cmd: 'mode', mode: 'Shutdown' }) };
const snoozeLabel = (s) => (s >= 3600 ? `Snooze ${Math.round(s / 3600)} h` : `Snooze ${Math.round(s / 60)} min`);
/* Carrying out the remedy answers the alarm, so it is acknowledged at the same time. Without that
   it would sit in the list as an unanswered warning after the very thing it asked for was done. */
async function runFix(n) {
  try { await FIX_RUN[n.fix]?.(); } catch (e) { toast(e.message, true); return; }
  try { await api('/alarms/ack', { body: { key: n.key } }); } catch {}
  await refreshAlarms();
}

/* An alarm that names a remedy is a question, so it is asked rather than filed.
 *
 * Only once per activation, and never for one already acknowledged or snoozed: the list behind the
 * bell is where a question you have already answered lives. If the condition ends by itself -- the
 * grill goes out -- the alarm goes with it and the question is moot. */
const asked = new Set();
function askAboutFixables() {
  for (const n of PF.alarms.alarms || []) {
    if (!n.active || !n.fix || n.acked || n.shelved_for || asked.has(n.key)) continue;
    asked.add(n.key);
    dialog((close) => el('div', {},
      el('h3', {}, n.title || 'The grill needs an answer'),
      n.body ? el('p', { class: 'muted' }, n.body) : null,
      el('div', { class: 'form-actions' },
        el('button', { class: 'btn ghost', type: 'button',
          onclick: async () => { close(); try { await api('/alarms/ack', { body: { key: n.key } }); } catch {} await refreshAlarms(); } }, 'Ignore'),
        el('button', { class: 'btn ghost', type: 'button',
          onclick: async () => { close(); try { await api('/alarms/shelve', { body: { key: n.key, seconds: n.snooze_s || 1800 } }); } catch {} await refreshAlarms(); } },
          snoozeLabel(n.snooze_s || 1800)),
        el('button', { class: 'btn primary', type: 'button',
          onclick: () => { close(); runFix(n); } }, FIX_LABEL[n.fix] || 'Fix'))));
    return;   /* one question at a time */
  }
  /* Forget the ones that have gone, so the same condition arising again is asked about again. */
  const live = new Set((PF.alarms.alarms || []).filter((a) => a.active).map((a) => a.key));
  for (const k of asked) if (!live.has(k)) asked.delete(k);
}
function kindOf(code = '', level = 1) {
  if (/^E\d/.test(code) || level >= 3) return 'error';
  if (/Limit_Alarm|Pellet_Level_Low|Autotune_Failed|W\d\d/.test(code) || level === 2) return 'warn';
  if (/Achieved|Timer_Expired|Probe_ETA|Recipe_|Autotune_Done|Tuning_Applied/.test(code)) return 'ok';
  return 'info';
}
function updateBadge() {
  const b = document.getElementById('bell-badge');
  if (!b) return;
  const n = PF.alarms.unacked || 0;
  b.hidden = n === 0; b.textContent = n > 99 ? '99+' : String(n);
  const bell = document.getElementById('bell');
  if (bell) bell.classList.toggle('has-error', (PF.alarms.worst ?? -1) >= 3);
}
const critKind = (c) => (c >= 3 ? 'error' : c === 2 ? 'warn' : c === 0 ? 'ok' : 'info');
/* Show something on this screen now. What is worth keeping is kept by the daemon, so this only
   ever draws a banner -- there is no second, per-device copy of the list to drift out of step. */
export function notify({ kind = 'info', title = '', body = '', code = '', ts = Date.now() / 1000 }, { banner = true } = {}) {
  const n = { id: `${Math.round(ts * 1000)}-${Math.random().toString(36).slice(2, 6)}`, kind, title, body, code, ts };
  if (banner) showBanner(n);
  return n;
}
function showBanner(n) {
  const stack = document.getElementById('toasts');
  if (!stack) return;
  const iconSvg = () => { const w = el('span', { class: `ic-wrap ${n.kind}` }); import('./icons.js').then((m) => w.append(m.icon(KIND_ICON[n.kind] || 'info'))); return w; };
  const t = el('div', { class: `ntoast ${n.kind}`, role: 'status' }, iconSvg(),
    el('div', { class: 'nt-body' }, n.title ? el('div', { class: 'nt-title' }, n.title) : null, n.body ? el('div', { class: 'nt-text' }, n.body) : null),
    el('button', { class: 'nt-close', 'aria-label': 'Dismiss', onclick: (e) => { e.stopPropagation(); dismiss(); } }, '×'));
  const dismiss = () => { t.classList.add('out'); setTimeout(() => t.remove(), 220); };
  t.onclick = dismiss;
  stack.append(t);
  while (stack.children.length > 3) stack.firstElementChild.remove();
  if (n.kind !== 'error') setTimeout(dismiss, n.kind === 'warn' ? 7000 : 4000);
}
// grill event pushed over the socket
function alert(m) {
  const kind = kindOf(m.code, m.level);
  notify({ kind, title: m.title, body: m.body, code: m.code, ts: m.ts || Date.now() / 1000 });
  try {
    if (document.visibilityState !== 'visible') showSystemNotification(m);
    if (navigator.vibrate && kind !== 'info') navigator.vibrate([120, 60, 120]);
  } catch {}
}
// Nothing to catch up on: the daemon holds the list, so opening the app shows the state of the
// grill rather than a replay of an event log. An old entry cannot reappear as news, and one that
// was cleared on another device is already gone here.
// iOS draws Safari's address bar and toolbar when the page is opened in a tab rather than launched
// from the Home Screen icon. Say so once, with the taps that fix it, instead of leaving it a mystery.
function installHint() {
  const iOS = /iPad|iPhone|iPod/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  const standalone = window.navigator.standalone === true || window.matchMedia('(display-mode: standalone)').matches;
  if (!iOS || standalone) return;
  try { if (localStorage.getItem('pf.installHint') === '1') return; localStorage.setItem('pf.installHint', '1'); } catch { /* private window */ }
  notify({ kind: 'info', title: 'Add PiFire to your Home Screen',
    body: 'Tap Share, then "Add to Home Screen". Launched from there it runs full screen, without Safari\'s bars.' });
}

/* iPhones do not have the Notification constructor. A Home Screen web app on iOS 16.4 or later can
   show a notification, but only through its service worker's registration, so that is the path to
   try first and the constructor is the desktop fallback. Note that this reaches you while the app
   is running or recently backgrounded; once iOS has fully closed it nothing arrives here, which is
   what Pushover is for. */
export async function showSystemNotification(m) {
  if (!('Notification' in window) || Notification.permission !== 'granted') return;
  const opts = { body: m.body, tag: m.code, icon: '/icon-192.png', badge: '/icon-192.png', data: { code: m.code } };
  try {
    const reg = await navigator.serviceWorker?.ready;
    if (reg?.showNotification) { await reg.showNotification(m.title, opts); return; }
  } catch {}
  try { new Notification(m.title, opts); } catch {}
}

/* Whether a system notification can reach this device at all, and why not when it cannot. */
export function alertSupport() {
  const standalone = window.matchMedia?.('(display-mode: standalone)').matches || window.navigator.standalone === true;
  const ios = /iP(hone|ad|od)/.test(navigator.userAgent) || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
  if (!('Notification' in window)) {
    return { ok: false, why: ios && !standalone
      ? 'iPhone: Share → Add to Home Screen, then open it from there.'
      : 'This browser cannot show notifications.' };
  }
  if (ios && !standalone) return { ok: false, why: 'Add to the Home Screen and open it from there. iOS allows notifications only then.' };
  if (!window.isSecureContext) return { ok: false, why: 'Needs https. Reach the grill over https or Tailscale.' };
  if (Notification.permission === 'denied') return { ok: false, why: 'Notifications are blocked for PiFire in your device settings.' };
  return { ok: Notification.permission === 'granted', why: Notification.permission === 'granted' ? '' : 'Not allowed yet.' };
}

export function requestAlertPermission() {
  if ('Notification' in window && Notification.permission === 'default')
    return Notification.requestPermission().then((p) => { if (p === 'granted') ensurePushSubscription(); return p; }).catch(() => 'default');
  if (typeof Notification !== 'undefined' && Notification.permission === 'granted') ensurePushSubscription();
  return Promise.resolve(typeof Notification !== 'undefined' ? Notification.permission : 'denied');
}

/* Hand the grill a push subscription, which is the only thing that reaches this phone once iOS has
   closed the app. Showing a notification from the running page stops the moment the app is
   suspended -- seconds, on iOS -- so without this the browser option is decorative. Subscribing is
   idempotent: the browser returns the subscription it already has, and the daemon keys on the
   endpoint, so calling this on every launch keeps a refreshed subscription current. */
export async function ensurePushSubscription() {
  try {
    if (!('serviceWorker' in navigator) || !('PushManager' in window)) return null;
    if (typeof Notification === 'undefined' || Notification.permission !== 'granted') return null;
    const info = await api('/push');
    if (!info.available || !info.key) return null;
    const reg = await navigator.serviceWorker.ready;
    let sub = await reg.pushManager.getSubscription();
    if (sub && sub.options?.applicationServerKey) {
      /* a subscription made against a different grill key cannot be decrypted by this one */
      const cur = btoa(String.fromCharCode(...new Uint8Array(sub.options.applicationServerKey)))
        .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
      if (cur !== info.key) { try { await sub.unsubscribe(); } catch {} sub = null; }
    }
    if (!sub) sub = await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: info.key });
    await api('/push/subscribe', { body: sub.toJSON ? sub.toJSON() : sub });
    return sub;
  } catch (e) {
    return null;
  }
}
// Short confirmations ("Saved") are banners only; errors also land in the centre.
export function toast(msg, err = false) {
  notify({ kind: err ? 'error' : 'ok', title: err ? 'Something went wrong' : '', body: msg }, { keep: err });
}
export function openNotifications() {
  return pushScreen((close) => {
    const wrap = el('div', { class: 'ncenter' });
    const render = () => {
      /* Clearing one means it goes. An alarm whose condition still stands used to stay in the
         list after being cleared, dimmed, on the industrial principle that a standing condition
         is still standing -- and on a phone that read as a row that cannot be got rid of. It is
         gone from here once it has been seen; if the condition ends and comes back, it is raised
         again as news. */
      const list0 = (PF.alarms.alarms || []).filter((a) => !(a.acked && a.active));
      /* Standing conditions first, worst first, then what is merely waiting to be read. */
      const items = list0.slice().sort((a, b) =>
        (b.active - a.active) || (b.crit - a.crit) || (b.ts - a.ts));
      wrap.innerHTML = '';
      wrap.append(el('div', { class: 'row between' }, el('h3', {}, 'Notifications'),
        el('button', { class: 'btn sm ghost', type: 'button', disabled: !items.length,
          onclick: async () => { try { await api('/alarms/ack', { body: { all: true } }); } catch {} await refreshAlarms(); } }, 'Clear all')));
      if (!items.length) wrap.append(el('div', { class: 'muted', style: 'padding:14px 0' }, 'Nothing to review.'));
      const list = el('div', { class: 'nlist' });
      for (const n of items) {
        const kind = critKind(n.crit);
        const w = el('span', { class: `ic-wrap ${kind}` }); import('./icons.js').then((m) => w.append(m.icon(KIND_ICON[kind] || 'info')));
        const when = new Date((n.cleared_ts || n.ts) * 1000).toLocaleString([], { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' });
        /* An alarm says what it is doing now; one that has ended says so instead of vanishing
           silently, because fixing something is not the same as having seen that it broke. */
        const meta = [
          n.active ? 'Happening now' : n.notice ? when : `Ended ${when}`,
          n.raises > 1 ? `${n.raises}\u00d7` : null,
          n.shelved_for ? `muted ${Math.round(n.shelved_for / 60)} min` : null,
        ].filter(Boolean).join(' \u00b7 ');
        list.append(el('div', { class: `nitem ${kind}${n.active ? ' live' : ''}` }, w,
          el('div', { class: 'nt-body' },
            n.title ? el('div', { class: 'nt-title' }, n.title) : null,
            n.body ? el('div', { class: 'nt-text' }, n.body) : null,
            el('div', { class: 'meta' }, meta),
            /* The verbs sit under what they are about rather than beside it: two buttons alongside
               a paragraph squeeze the paragraph into a column three words wide. Silencing it comes
               first and fixing it last, the order every other row in the app is read in. */
            n.active && (n.fix || !n.shelved_for) ? el('div', { class: 'nt-acts' },
              /* Mute is for the one that is right, keeps happening, and cannot be fixed now. */
              !n.shelved_for ? el('button', { class: 'btn xs ghost', type: 'button',
                title: `Silence for ${Math.round((n.snooze_s || 1800) / 60)} minutes`,
                onclick: async () => { try { await api('/alarms/shelve', { body: { key: n.key, seconds: n.snooze_s || 1800 } }); } catch {} await refreshAlarms(); } },
                n.snooze_s ? snoozeLabel(n.snooze_s) : 'Mute') : null,
              /* An alarm that names a remedy offers it here, rather than sending the reader off to
                 find the screen the remedy lives on. */
              n.fix ? el('button', { class: 'btn xs primary', type: 'button',
                onclick: () => runFix(n) }, FIX_LABEL[n.fix] || 'Fix') : null) : null),
          el('button', { class: 'nt-close', 'aria-label': 'Clear',
            onclick: async () => { try { await api('/alarms/ack', { body: { key: n.key } }); } catch {} await refreshAlarms(); } }, '\u00d7')));
      }
      wrap.append(list);
    };
    centreRender = render;
    render();
    refreshAlarms();
    return wrap;
  }, { title: 'Notifications', back: 'Back' }).then((v) => { centreRender = null; return v; });
}
/* A pushed screen, not a floating box.
 *
 * Anything bigger than a question -- editing a notification, setting a probe up, reviewing what the
 * grill has said -- is a screen you go to and come back from, the way a native app works. A large
 * modal is a website's idea of the same thing: it hangs over the page, it cannot be reached by the
 * back gesture, and while `showModal()` holds the page inert the tab bar does not answer, so there
 * is no navigating away from it either.
 *
 * So this pushes a history entry and covers the content area between the bars. The back gesture
 * unwinds it, tapping a tab closes it, and its own back arrow closes it -- all of them the same
 * event as far as the caller is concerned: the promise resolves.
 */
let sheetSeq = 0;
export function pushScreen(build, opts = {}) {
  const view = document.getElementById('view');
  const id = ++sheetSeq;
  return new Promise((resolve) => {
    let pending, done = false, restoreBack = null, screenEl = null, stash = null;
    const finish = () => {
      if (done) return;
      done = true;
      restoreBack?.();
      window.removeEventListener('popstate', finish);
      window.removeEventListener('hashchange', onNav);
      /* Only put the page back if it is still ours to put back. If the route changed underneath us
         the router has already drawn something else, and restoring would clobber it. */
      if (screenEl && screenEl.parentNode === view) {
        screenEl.remove();
        if (stash) view.append(stash);
        view.scrollTop = 0;
      }
      resolve(pending);
    };
    /* Tapping a tab has already pushed its own entry, so unwinding ours would undo their
       navigation. Just stand down. */
    const onNav = () => finish();
    const close = (v) => {
      pending = v;
      if (history.state?.pfScreen === id) history.back();   /* -> popstate -> finish */
      else finish();
    };
    /* One navigation bar, not two. The app header already has a back affordance and a place for it;
       a screen that draws its own ends up with "< Settings  < Probes" stacked down the page, which
       is a navigation stack rendered twice and belongs to no app. So the screen takes over the
       header's back arrow for as long as it is up, labelled with where it came from, and puts it
       back exactly as it found it. */
    const b = document.getElementById('tb-back');
    const lab = document.getElementById('tb-back-label');
    const prev = { hidden: b?.hidden, onclick: b?.onclick, label: lab?.textContent };
    restoreBack = () => {
      if (!b) return;
      if (prev.hidden) { b.hidden = true; b.onclick = null; }
      else { b.hidden = false; b.onclick = prev.onclick; if (lab) lab.textContent = prev.label; }
    };
    setBack(() => close(undefined), opts.back || 'Back');

    /* A page, not a panel over one.
     *
     * This used to be an overlay pinned to the viewport, which meant its edges were its own problem
     * to get right -- and it got them wrong: its pinned footer sat on top of the Home button, the
     * one control that is always there. A page put INSIDE the scrolling content cannot occlude the
     * bars, because the content area already stops where they begin. The page it replaces is set
     * aside whole and put back when this one closes, which is what going somewhere and coming back
     * actually means. */
    stash = document.createDocumentFragment();
    while (view.firstChild) stash.append(view.firstChild);
    screenEl = el('div', { class: 'screen' }, build(close));
    /* Anything the person types, picks or toggles makes the screen dirty, and the screen's own Save
       lights up. One listener at the top rather than a flag threaded through every field. */
    /* Not armed until the screen has finished drawing itself. Building a form can raise change
       events of its own -- a select settling on its first option, a field filled from the rule --
       and counting those would light Save on a screen nobody has touched, which is the one thing
       this is here to stop. */
    let armed = false;
    requestAnimationFrame(() => requestAnimationFrame(() => { armed = true; }));
    const wake = () => {
      if (!armed) return;
      const g = screenEl.querySelector('[data-save-guard]');
      if (g) { g.disabled = false; delete g.dataset.saveGuard; }
    };
    screenEl.addEventListener('input', wake);
    screenEl.addEventListener('change', wake);
    screenEl.addEventListener('pf-dirty', wake);
    view.append(screenEl);
    view.scrollTop = 0;
    history.pushState({ pfScreen: id }, '');
    window.addEventListener('popstate', finish);
    window.addEventListener('hashchange', onNav);
  });
}

export function dialog(build) {
  const d = document.getElementById('dlg');
  d.innerHTML = '';
  const close = (v) => {
    d.close();
    /* A closed dialog is gone -- and if it is not, say so loudly rather than leaving a panel
     * sitting on the page. The layout of a <dialog> belongs to its [open] state, and an author
     * `display` written without that qualification beats the browser's own rule and keeps the
     * element laid out after close() has run. That shipped three times, each time looking like a
     * dialog that would not close while close() worked perfectly. The stylesheet is fixed; this is
     * the tripwire, so the next one is a console error and not a stuck app. */
    if (getComputedStyle(d).display !== 'none') {
      console.error('[pifire] dialog stayed laid out after close(); check .dialog[open] in style.css');
      d.style.display = 'none';
    }
    d._resolve?.(v);
  };
  d.style.display = '';   /* clear the tripwire's override from a previous close */
  return new Promise((resolve) => {
    d._resolve = resolve;
    const content = build(close);
    d.append(content);
    /* There is always a way out.
     *
     * showModal() makes the rest of the page inert, so a dialog you cannot dismiss is not a stuck
     * dialog, it is a stuck app -- the tab bar stops answering and nothing moves. Escape needs a
     * keyboard and the backdrop is a sliver beside a full-height sheet on a phone, so neither is a
     * way out there. Every dialog therefore gets a close mark, in its header if it has one and
     * floating at the top corner if it does not, put here rather than in each caller so that one
     * added later cannot forget it. */
    if (!content.querySelector?.('[data-dlg-close]')) {
      const x = el('button', {
        class: 'dlg-x', type: 'button', 'aria-label': 'Close', 'data-dlg-close': '',
        onclick: () => close(undefined),
      }, lucide('x', 'ic'));
      const head = content.querySelector?.('.sheet-head');
      if (head) head.append(x); else d.append(x);
    }
    // the close event is queued asynchronously; ignore one that belongs to a previous dialog
    // when a new one has already been opened in its place
    d.onclose = () => { if (!d.open) d._resolve?.(undefined); };
    d.onclick = (e) => { if (e.target === d) close(undefined); };
    d.showModal();
  });
}
/* The actions of a pushed edit screen, as one row stuck under the header.
 *
 * At the foot of the page they were a scroll away from whatever you had just changed, and on a
 * long form you edited a field at the top and then went looking for Save. Under the header they
 * are where the thing you are editing is. One row, always the same: the destructive action alone
 * on the left as a mark with no word -- it is not a thing to reach for by reading -- and the
 * dismissive then the committing action on the right, which is the order everything else in this
 * app uses. See docs/design-language.md. */
export function actionBar(left = [], right = []) {
  /* The Home button rises out of the tab bar and passes over this row, so the row keeps a hole in
     the middle the size of that circle. A single stretchy spacer was not enough: it only holds the
     two groups apart, and as soon as a screen had a third verb the right-hand group grew back
     across the centre and the circle landed on Cancel. The hole is reserved, and the groups share
     what is left either side of it. */
  return el('div', { class: 'screen-actions' },
    ...left.filter(Boolean), el('span', { class: 'grow' }),
    el('span', { class: 'home-gap', 'aria-hidden': 'true' }),
    el('span', { class: 'grow' }), ...right.filter(Boolean));
}

export function screenActions({ onDelete, deleteTitle = 'Delete', onCancel, onSave, saveLabel = 'Save', extra, dirty = true } = {}) {
  const save = onSave ? actionBtn('save', saveLabel, { size: '', onclick: onSave }) : null;
  /* Save is lit only once there is something to save.
   *
   * An always-live Save on a screen you opened to read says the same thing whether you changed
   * anything or not, so it stops meaning anything, and it invites a pointless write on the way out.
   * The screen watches its own inputs (see pushScreen) and lights this the moment one of them
   * moves; anything the page changes in code -- adding a condition, ticking a chip -- says so by
   * dispatching `pf-dirty`. Passing dirty: true keeps a screen that is born changed, such as a new
   * rule, ready to save from the start. */
  if (save && !dirty) { save.disabled = true; save.dataset.saveGuard = ''; }
  return actionBar(
    [onDelete ? iconBtn('trash-2', deleteTitle, { class: 'danger', onclick: onDelete }) : null],
    [...(extra || []),
     onCancel ? actionBtn('cancel', 'Cancel', { size: '', onclick: onCancel }) : null,
     save]);
}

export function confirmDialog(title, text, okLabel = 'Confirm', danger = false) {
  return dialog((close) => el('div', {},
    el('h3', {}, title), el('p', { class: 'muted' }, text),
    el('div', { class: 'btnrow' },
      el('button', { class: 'btn ghost', type: 'button', onclick: () => close(false) }, 'Cancel'),
      el('button', { class: 'btn ' + (danger ? 'danger' : 'primary'), type: 'button', onclick: () => close(true) }, okLabel))));
}
export function numberDialog(title, value, { min = 0, max = 600, step = 5, presets = [], unit = degUnit() } = {}) {
  return dialog((close) => {
    const inp = el('input', { type: 'text', inputmode: 'decimal', value, 'aria-label': title, enterkeyhint: 'done' });
    const form = el('form', { method: 'dialog', onsubmit: (e) => { e.preventDefault(); const v = parseFloat(inp.value); if (!Number.isNaN(v)) close(Math.min(max, Math.max(min, v))); } },
      el('h3', {}, title),
      el('div', { class: 'num-input' },
        el('button', { class: 'btn', type: 'button', onclick: () => (inp.value = (parseFloat(inp.value) || 0) - step) }, '−'),
        inp, el('span', { class: 'muted' }, unit),
        el('button', { class: 'btn', type: 'button', onclick: () => (inp.value = (parseFloat(inp.value) || 0) + step) }, '+')),
      presets.length ? el('div', { class: `presets ${presets.length === 9 ? 'pad' : ''}` }, presets.map((p) => el('button', { class: `btn ${presets.length === 9 ? '' : 'sm'} ${Number(value) === p ? 'primary' : ''}`, type: 'button', onclick: () => { if (presets.length === 9) close(p); else inp.value = p; } }, `${p}${presets.length === 9 ? '°' : unit}`))) : null,
      el('div', { class: 'btnrow' },
        el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'),
        el('button', { class: 'btn primary', type: 'submit' }, 'Set')));
    setTimeout(() => { inp.focus(); inp.select(); }, 50);
    return form;
  });
}
export function toggleRow(label, checked, onchange, help) {
  const input = el('input', { type: 'checkbox', checked, onchange: (e) => onchange(e.target.checked) });
  return el('label', { class: 'toggle' },
    el('div', {}, el('div', {}, label), help ? el('div', { class: 'help' }, help) : null),
    el('span', { class: 'switch' }, input, el('span')));
}
/* A section you can open: the one shape for this in the whole app.
 *
 * Built from the same parts as a row you tap -- icon tile, title, value, chevron -- so a section
 * header and a list row line up, and opening one draws the card under it without touching the
 * header. Settings had this and every other page grew its own, which is how the probes page ended
 * up with bare <details> carrying the browser's triangle next to headings that did not match
 * anything else.
 *
 * `meta` is the line under the title: what the row is worth knowing at a glance while shut. */
export function fold(title, meta, body, icon, color, open = false, value = null) {
  return el('details', { class: 'fold ios-fold', open },
    el('summary', {},
      icon ? el('span', { class: 'tile', style: tileStyle(color) }, lucide(icon)) : null,
      el('span', { class: 'body' }, el('span', { class: 't' }, title), meta ? el('span', { class: 's' }, meta) : null),
      /* What it is SET TO, at the right of its own row -- the way a settings row reads
         "Language   English  >". A value belongs on the right, not folded into the subtitle. */
      value != null ? el('span', { class: 'v' }, value) : null,
      lucide('chevron-right', 'ic chev')),
    el('div', { class: 'fold-body' }, body));
}

/* A button that leads with its mark.
 *
 * A modern interface says what a control does with a shape and a colour before it says it with a
 * word: a trash can for delete, a pencil for edit, a plus for add. The colour carries the same
 * message -- red destroys, the accent commits, grey backs out -- so the three are never confused
 * at a glance, and the mark makes the word almost redundant on a narrow screen.
 *
 * `kind` is one of the verbs below; anything else is a plain button with whatever icon is named. */
const VERB = {
  add: { icon: 'plus', cls: '' },
  edit: { icon: 'pencil', cls: '' },
  delete: { icon: 'trash-2', cls: 'danger' },
  save: { icon: 'check', cls: 'primary' },
  cancel: { icon: 'x', cls: 'ghost' },
  filter: { icon: 'sliders-horizontal', cls: '' },
};
export function actionBtn(kind, label, attrs = {}, iconOverride) {
  const v = VERB[kind] || { icon: iconOverride || kind, cls: '' };
  const cls = ['btn', attrs.size || 'sm', v.cls, attrs.class].filter(Boolean).join(' ');
  const { size, class: _c, ...rest } = attrs;
  return el('button', { type: 'button', ...rest, class: cls }, lucide(iconOverride || v.icon, 'ic btn-ic'), label ? el('span', {}, label) : null);
}

/* A text input with its own mark inside it. */
export function iconField(icon, input) {
  return el('div', { class: 'field-ic' }, lucide(icon, 'ic'), input);
}

/* A saved thing, listed the way a saved card is: a mark for what it is, its name, one line of
 * detail, a badge if it is the one in use, and the action you take on it as a bare icon at the end.
 * The whole row opens it. This is the Payment Methods shape -- "Visa •••• 4242 / Expires 12/25"
 * with a Default badge and a bin at the right -- and it suits anything kept in a collection.
 *
 * { icon, color, title, meta, badge, onclick, actions: [node] }
 */
export function itemRow(o) {
  return el('div', { class: `irow ${o.badge ? 'current' : ''}` },
    el('button', { class: 'irow-main', type: 'button', onclick: o.onclick },
      o.icon ? el('span', { class: 'tile', style: tileStyle(o.color) }, lucide(o.icon)) : null,
      el('span', { class: 'body' },
        el('span', { class: 't' }, o.title, o.badge ? el('span', { class: 'badge-pill' }, o.badge) : null),
        o.meta ? el('span', { class: 's' }, o.meta) : null),
      /* a settings row's answer -- what it is set to -- on the right, before the chevron */
      o.value != null ? el('span', { class: 'v' }, o.value) : null,
      /* the chevron says "this opens"; a row whose own actions include a pencil has said it already */
      o.chevron === false ? null : lucide('chevron-right', 'ic chev')),
    o.actions?.length ? el('div', { class: 'irow-acts' }, o.actions) : null);
}

/* An action with no label: a bare mark with a thumb-sized target around it, for the end of a row
   where the row itself already says what the thing is. */
export function iconBtn(icon, title, attrs = {}) {
  const { class: cls, ...rest } = attrs;
  return el('button', { type: 'button', title, 'aria-label': title, ...rest, class: `btn icon ${cls || ''}` }, lucide(icon, 'ic btn-ic'));
}

/* Every collection travels as a file, the same way: Export on the left (it takes nothing away),
   Import on the right (it changes the list, so it is the committing action and it confirms).
   Under the Add row, on every list that has one -- recipes, notifications, the tuning library. */
export function transferRow({ what, filename, fetchDoc, importDoc, confirmText }) {
  const download = async () => {
    try {
      const doc = await fetchDoc();
      const name = `${filename}-${new Date().toISOString().slice(0, 10)}.json`;
      const url = URL.createObjectURL(new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' }));
      const a = el('a', { href: url, download: name });
      document.body.append(a); a.click(); a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 10000);
      toast(`Exported ${what}`);
    } catch (e) { toast(e.message, true); }
  };
  const upload = () => {
    const f = el('input', { type: 'file', accept: 'application/json,.json' });
    f.onchange = async () => {
      const file = f.files?.[0];
      if (!file) return;
      let doc;
      try { doc = JSON.parse(await file.text()); } catch { toast(`${file.name} is not a JSON file`, true); return; }
      if (!await confirmDialog(`Import ${what} from ${file.name}?`, confirmText, 'Import')) return;
      try { await importDoc(doc); } catch (e) { toast(e.message || `That file does not hold ${what}`, true); }
    };
    f.click();
  };
  return el('div', { class: 'btnrow transfer' },
    el('button', { class: 'btn ghost', type: 'button', onclick: download }, lucide('download', 'ic btn-ic'), el('span', {}, 'Export')),
    el('button', { class: 'btn ghost', type: 'button', onclick: upload }, lucide('upload', 'ic btn-ic'), el('span', {}, 'Import')));
}

/* The way to add another one: a full-width outlined button at the FOOT of the list it adds to,
   where the eye ends up after reading what is already there. */
export function addRow(label, onclick) {
  return el('button', { class: 'btn ghost block', type: 'button', onclick }, lucide('plus', 'ic btn-ic'), el('span', {}, label));
}

/* A table that becomes cards on a phone.
 *
 * Several of the same thing -- tuning anchors, probe profiles, bags of pellets -- read best as a
 * table with a header row, and a table is unusable at 402 px. So each row carries its own column
 * names: on a wide screen the header row shows and the per-cell labels are hidden, and on a phone
 * the header goes and every value sits under its own small caption, four to a line. One set of
 * markup, two layouts, and the same information either way.
 *
 * columns: [{ key, label }]   rows: [{ <key>: value, _actions?: [node], _onclick?: fn }]
 */
export function dataTable(columns, rows, opts = {}) {
  const t = el('div', { class: 'dtable', style: `--dt-cols:${columns.length}` });
  t.append(el('div', { class: 'dt-head' }, columns.map((c) => el('span', {}, c.label))));
  for (const r of rows) {
    const cells = columns.map((c) => el('span', { class: 'dt-cell' },
      el('i', { class: 'dt-k' }, c.label),
      el('b', { class: 'dt-v' }, r[c.key] == null || r[c.key] === '' ? '—' : r[c.key])));
    const body = r._onclick
      ? el('button', { class: 'dt-cells', type: 'button', onclick: r._onclick }, cells, lucide('chevron-right', 'ic chev'))
      : el('div', { class: 'dt-cells' }, cells);
    t.append(el('div', { class: 'dt-row' }, body, r._actions ? el('div', { class: 'dt-acts' }, r._actions) : null));
  }
  if (!rows.length) t.append(el('p', { class: 'help' }, opts.empty || 'Nothing here yet.'));
  return t;
}

/* A heading with an action beside it: "Probe Profiles" and an Add button. One recipe, so every
   manager on every page has its bar in the same place and the same shape. */
export function sectionBar(title, ...actions) {
  return el('div', { class: 'row between' }, el('h2', {}, title), ...actions.filter(Boolean));
}

/* `hug` is for a control that shares a row with something else; by default the options divide the
   width of whatever holds them. */
export function segmented(options, value, onchange, { hug = false } = {}) {
  const wrap = el('div', { class: `segmented${hug ? ' hug' : ''}` });
  for (const [v, label] of options) {
    wrap.append(el('button', { type: 'button', class: v === value ? 'active' : '', onclick: (e) => { wrap.querySelectorAll('button').forEach((b) => b.classList.remove('active')); e.currentTarget.classList.add('active'); onchange(v); } }, label));
  }
  return wrap;
}

// ---------- iOS-style grouped list: [icon tile][title / subtitle][chevron] ----------
export function listGroup(title, rows, footer) {
  const list = el('div', { class: 'ios-list' }, ...rows.filter(Boolean).map((r) => {
    /* A brand mark carries its own shape and colour, so it stands on a plain tile; everything else
       is a white glyph on a coloured one. */
    const tile = r.brand ? el('span', { class: 'tile brand' }, brandIcon(r.brand))
               : r.icon ? el('span', { class: 'tile', style: tileStyle(r.color) }, lucide(r.icon)) : null;
    const body = el('div', { class: 'body' }, el('div', { class: 't' }, r.title), r.sub ? el('div', { class: 's' }, r.sub) : null);
    const right = r.value != null ? el('span', { class: 'v' }, r.value) : null;
    const chevron = r.onclick || r.href ? lucide('chevron-right', 'ic chev') : null;
    const attrs = { class: `row ${r.danger ? 'danger' : ''}`, type: 'button' };
    if (r.onclick) attrs.onclick = r.onclick; else if (r.href) attrs.onclick = () => (location.hash = r.href);
    return el('button', attrs, tile, body, right, chevron);
  }));
  return el('section', { class: 'ios-group' }, title ? el('h2', {}, title) : null, list, footer ? el('div', { class: 'foot' }, footer) : null);
}

// ---------- theme ----------
export function applyTheme() {
  const t = PF.settings?.globals?.theme || 'dark';
  document.documentElement.dataset.theme = t === 'auto' ? '' : t;
  document.querySelector('meta[name=theme-color]').content = t === 'light' ? '#f3f3f5' : '#111214';
}

// ---------- router ----------
const pages = { home: renderHome, history: renderHistory, cook: renderCook, probes: renderProbes, settings: renderSettings, more: renderMore, setup: (v) => renderNetwork(v, { captive: true }) };
let teardown = null;
/* The back affordance belongs to the navigation bar, not to the page. Putting it in the scrolling
   content meant it slid away the moment you scrolled, which no native app does -- you should never
   have to scroll back up to leave a page. Pages call this while rendering; the router clears it on
   every navigation so it cannot outlive the page that asked for it. */
/* `href` may be a hash to go to, or a function to run -- a pushed screen closes itself rather than
   navigating, and it uses the same one affordance to do it. */
export function setBack(href, label = 'Back') {
  const b = document.getElementById('tb-back');
  const lab = document.getElementById('tb-back-label');
  if (!b) return;
  if (lab) lab.textContent = label;
  b.onclick = typeof href === 'function' ? href : () => { location.hash = href; };
  b.hidden = false;
}
function clearBack() {
  const b = document.getElementById('tb-back');
  if (b) { b.hidden = true; b.onclick = null; }
}

function route() {
  /* Navigating away puts anything open away with it. A box still hanging over the new page is the
     surest sign you are looking at a website. */
  const dlg = document.getElementById('dlg');
  if (dlg?.open) dlg.close();
  clearBack();
  // captive-portal browsers land on /setup by path rather than by hash
  const hash = location.hash.replace(/^#\/?/, '') || (location.pathname === '/setup' ? 'setup' : 'home');
  const [page, ...rest] = hash.split('/');
  const fn = pages[page] || renderHome;
  document.documentElement.dataset.page = pages[page] ? page : 'home';
  document.querySelectorAll('.tabbar a').forEach((a) => a.classList.toggle('active', a.dataset.tab === (pages[page] ? page : 'home')));
  if (teardown) { teardown(); teardown = null; }
  const view = document.getElementById('view');
  view.innerHTML = '';
  view.scrollTop = 0;
  const r = fn(view, rest);
  if (typeof r === 'function') teardown = r;
  else if (r && typeof r.then === 'function') { const token = (route.token = (route.token || 0) + 1); r.then((t) => { if (typeof t === 'function') { if (route.token === token) teardown = t; else t(); } }); }
}
window.addEventListener('hashchange', route);

// ---------- header control strip and alert banner ----------
/* One indicator for one question: can this app reach the grill, and by what road.
 *
 * There used to be two, a green dot beside the name and a Tailscale mark beside it, and they
 * answered the same question twice. Now there is a single mark. It is the Tailscale logo when this
 * browser is talking to the grill through the tailnet -- which is decided by the address in the
 * address bar, not by the grill merely having Tailscale installed, because the two are different
 * facts and only the first is about this connection. Otherwise it is a plain network glyph. Green
 * when the live link is up, red when it is not. */
let lastNet = {};
function overTailscale(net) {
  const ts = net && net.tailscale;
  if (!ts) return false;
  const host = location.hostname.toLowerCase();
  const name = (ts.name || '').toLowerCase();
  return (name && (host === name || host === name.split('.')[0])) || /\.ts\.net$/.test(host) || /^100\./.test(host);
}
function paintLink() {
  const l = document.getElementById('ind-link');
  if (!l) return;
  const ts = overTailscale(lastNet);
  l.className = `tb-ind ${PF.connected ? 'ok' : 'bad'}`;
  l.title = PF.connected
    ? (ts ? `Connected through Tailscale${lastNet.tailscale && lastNet.tailscale.name ? ' · ' + lastNet.tailscale.name : ''}` : 'Connected to the grill')
    : 'Not connected to the grill';
  l.replaceChildren(ts ? brandIcon('tailscale') : lucide('network'));
}
const bars = (n, cls) => el('span', { class: `sig s${n} ${cls}` }, [1, 2, 3, 4].map((i) => el('i', { class: i <= n ? 'on' : '' })));
const wifiBars = (pct) => (!pct ? 0 : pct >= 75 ? 4 : pct >= 55 ? 3 : pct >= 35 ? 2 : 1);

/* The grill's own uplink: how well IT is connected, which is a different question from whether this
 * app can reach it. Four bars, a hotspot marker, or a wired tag -- but only while the link is up.
 * The moment it drops, every one of those is a claim about a signal nobody is measuring any more:
 * the last reading might be a minute old or an hour, and drawing four bars beside a red connection
 * mark says the grill is fine when the truth is that we have no idea. So it falls back to a plain
 * aerial, dimmed, which says exactly that. */
function paintNet() {
  const wifi = document.getElementById('ind-wifi');
  if (!wifi) return;
  const net = lastNet;
  wifi.hidden = false;
  if (!PF.connected) {
    wifi.className = 'tb-ind muted';
    wifi.title = 'Signal unknown: not connected to the grill';
    wifi.replaceChildren(lucide('wifi'));
  } else if (net.hotspot) {
    wifi.className = 'tb-ind warn'; wifi.title = `Setup hotspot: ${net.ssid || ''}`;
    wifi.replaceChildren(el('span', { class: 'tb-tag' }, 'AP'));
  } else if (net.signal > 0) {
    wifi.className = 'tb-ind'; wifi.title = `${net.ssid || 'Wi-Fi'} · ${net.signal}%`;
    wifi.replaceChildren(bars(wifiBars(net.signal), 'wifi'));
  } else if (net.ip) {
    wifi.className = 'tb-ind'; wifi.title = `Wired · ${net.ip}`;
    wifi.replaceChildren(el('span', { class: 'tb-tag' }, 'LAN'));
  } else wifi.hidden = true;
  paintLink();
}

/* A drop-down hung under the header, anchored to one of the indicators beside it.
 *
 * The bell opens a pushed screen because its list is long and worth reading. A timer is four
 * numbers and three buttons, and pushing a whole screen for that loses the page you were on for no
 * gain. So this is the app's one drop-down: it comes down from under the header, covers the width
 * on a phone and narrows to the indicator on a desktop, and closes on Escape, on a tap outside, or
 * on a second tap of the button that opened it. */
let openPanel = null;
function headerPanel(btn, render) {
  if (openPanel?.btn === btn) { closePanel(); return; }
  closePanel();
  const box = el('div', { class: 'hpanel', role: 'dialog' });
  const scrim = el('div', { class: 'hpanel-scrim', onclick: () => closePanel() });
  const paint = () => { box.replaceChildren(render(closePanel)); };
  document.body.append(scrim, box);
  /* Hung off the button that opened it, measured rather than computed: the header's height is the
     safe-area inset plus its padding, and guessing that in CSS goes wrong on exactly the notched
     phones this app is built for. */
  const r = btn.getBoundingClientRect();
  box.style.top = `${Math.round(r.bottom + 8)}px`;
  box.style.setProperty('--hp-right', `${Math.round(Math.max(8, window.innerWidth - r.right - 6))}px`);
  btn.setAttribute('aria-expanded', 'true');
  openPanel = { btn, box, scrim, paint };
  paint();
  requestAnimationFrame(() => box.classList.add('in'));
  document.addEventListener('keydown', onPanelKey);
}
function closePanel() {
  if (!openPanel) return;
  const { btn, box, scrim } = openPanel;
  openPanel = null;
  btn.setAttribute('aria-expanded', 'false');
  box.classList.remove('in');
  scrim.remove();
  setTimeout(() => box.remove(), 180);
  document.removeEventListener('keydown', onPanelKey);
}
const onPanelKey = (e) => { if (e.key === 'Escape') closePanel(); };

/* The timers that are running, in the header, because a timer you cannot see is a timer you have
 * to remember. The chip carries the time itself rather than a bare glyph -- the whole point is to
 * answer "how long left" without tapping anything -- and it is not there at all when nothing is
 * counting, so the header stays quiet during a cook that needs no timer. */
let lastTimer = null, lastRecipe = null;
function timerPanel() {
  const t = lastTimer || {}, r = lastRecipe || {};
  const rows = [];
  if (t.running) {
    const send = (op, extra) => cmd({ cmd: 'timer', op, ...extra }).then(() => openPanel?.paint());
    rows.push(el('div', { class: 'tp-row' },
      el('div', { class: 'tp-head' },
        el('div', { class: 'tp-name' }, 'Timer'),
        el('div', { class: `tp-time${t.paused ? ' paused' : ''}` }, fmtDur(t.remaining))),
      el('div', { class: 'tp-bar' }, el('i', { style: `width:${t.duration > 0 ? Math.max(0, Math.min(100, 100 - (t.remaining / t.duration) * 100)) : 0}%` })),
      /* Stop ends it, so it sits on the dismissive side; the button that keeps it going sits on
         the committing side, as everywhere else in the app. */
      el('div', { class: 'tp-acts' },
        el('button', { class: 'btn sm danger', type: 'button', onclick: () => send('cancel') }, iconEl('square'), 'Stop'),
        el('button', { class: 'btn sm ghost', type: 'button', disabled: !(t.duration > 0),
          onclick: () => send('start', { seconds: Math.round(t.duration), after: t.after || 0 }) }, iconEl('rotate-ccw'), 'Reset'),
        el('button', { class: 'btn sm primary', type: 'button', onclick: () => send(t.paused ? 'resume' : 'pause') },
          iconEl(t.paused ? 'play' : 'pause'), t.paused ? 'Resume' : 'Pause'))));
  }
  /* A recipe step counting down is a running timer too, and during a six-hour cook it is the one
     that matters. It is shown, not driven: a step is advanced on the Cook page, where the rest of
     the recipe is, not from a drop-down that only knows about clocks. */
  if (r.active && r.remaining_s > 0) {
    rows.push(el('button', { class: 'tp-row tp-link', type: 'button', onclick: () => { closePanel(); location.hash = '#/cook'; } },
      el('div', { class: 'tp-head' },
        el('div', { class: 'tp-name' }, `${r.name} \u00b7 step ${(r.step ?? 0) + 1} of ${r.nsteps || 0}`),
        el('div', { class: 'tp-time' }, fmtDur(r.remaining_s)))));
  }
  if (!rows.length) rows.push(el('div', { class: 'muted', style: 'padding:6px 2px' }, 'Nothing is counting.'));
  return el('div', { class: 'tp' }, ...rows);
}
const iconEl = (name) => { const w = el('span', { class: 'ic' }); import('./icons.js').then((m) => w.append(m.icon(name))); return w; };

function paintTimer(s) {
  const btn = document.getElementById('ind-timer');
  if (!btn) return;
  lastTimer = s.timer || null;
  lastRecipe = s.recipe || null;
  const running = !!(lastTimer?.running) || !!(lastRecipe?.active && lastRecipe.remaining_s > 0);
  btn.hidden = !running;
  if (!running) { if (openPanel?.btn === btn) closePanel(); return; }
  /* Whichever is closest to going off is what the header shows, because that is the one about to
     need an answer. */
  const cands = [lastTimer?.running ? lastTimer.remaining : null,
                 lastRecipe?.active && lastRecipe.remaining_s > 0 ? lastRecipe.remaining_s : null].filter((v) => v != null);
  const soonest = Math.min(...cands);
  btn.className = `tb-ind tb-timer${lastTimer?.running && lastTimer.paused && cands.length === 1 ? ' paused' : ''}`;
  const txt = fmtDur(soonest);
  if (btn.dataset.txt !== txt) {
    btn.dataset.txt = txt;
    btn.replaceChildren(iconEl('timer'), el('span', { class: 'tb-tag' }, txt));
  }
  openPanel?.paint();
}

onStatus((s) => {
  const b = document.getElementById('banner');
  if (!s) { b.hidden = !PF.lost; if (PF.lost) { b.className = 'banner warn'; b.textContent = 'Connecting to grill…'; } return; }

  lastNet = s.net || {};
  paintNet();
  paintTimer(s);

  /* The mode, and the temperature the grill is at. The same two things on every page: this used to
     show the set point (or a countdown) on Home and the actual temperature everywhere else, so the
     same plate in the same place meant two different things depending on which tab you were on.
     The target and the countdown are both on Home already, beside the gauge that gives them
     context. */
  const readout = document.getElementById('readout');
  const rdMode = document.getElementById('rd-mode'), rdVal = document.getElementById('rd-val');
  const primary = s.probes?.find((p) => p.role === 'Primary');
  const value = s.mode === 'Stop' ? `0${degUnit()}` : primary?.valid ? `${fmtTemp(primary.temp)}${degUnit()}` : '—';
  /* A tuning run holds set points like any cook, so the mode alone says Hold and gives no hint that
     the grill is deliberately swinging either side of its target. Name what it is actually doing. */
  const tuning = !!(s.tuning?.running || s.autotune?.active);
  /* While a recipe runs, where you are in it IS the mode. "Hold" is true and useless -- a recipe
     holds for most of its length -- and the question the plate is being asked is which step is
     running and whether it is waiting on you. */
  const rc = s.recipe;
  /* The step number goes on the front of the plate as its own segment -- [1][STARTUP][109 F] --
     counting every step including lighting and shutting down, which is how the recipe's own
     screen counts them. The mode word stays the mode; "Your turn" takes its place while the
     recipe is waiting on the cook. */
  const modeName = tuning ? 'Auto Tuning' : rc?.active && rc.waiting ? 'Continue?' : s.mode;
  const nameEl = document.getElementById('rd-name');
  nameEl.textContent = modeName;
  nameEl.title = rc?.active ? `${rc.name}: step ${(rc.step ?? 0) + 1} of ${rc.nsteps}` : '';
  const stepEl = document.getElementById('rd-step');
  if (stepEl) { stepEl.textContent = rc?.active ? String((rc.step ?? 0) + 1) : ''; stepEl.hidden = !rc?.active; }
  /* The plate is the one thing on every screen, so while the recipe waits on the cook the plate
     is where "Continue?" is answered: a tap asks, in the step's own words, and carries on. */
  readout.classList.toggle('tappable', !!(rc?.active && rc.waiting));
  readout.onclick = rc?.active && rc.waiting ? async () => {
    if (rc.needs_lid) { toast('Open the lid first, then continue'); return; }
    if (await confirmDialog('Continue to the next step?', rc.message || 'This step is done.', 'Continue')) cmd({ cmd: 'recipe', op: 'next' });
  } : null;
  /* The same mark the mode carries everywhere else, so the plate reads as part of the interface
     rather than as a label that happens to be near it. */
  const ico = document.getElementById('rd-ico');
  const want = MODE_ICON[tuning ? 'Tuning' : rc?.active && rc.waiting ? 'Waiting' : s.mode];
  if (ico && ico.dataset.icon !== want) { ico.dataset.icon = want || ''; ico.replaceChildren(want ? lucide(want) : ''); }
  rdVal.textContent = value;
  rdVal.hidden = !value;
  readout.dataset.mode = tuning ? 'Tuning' : rc?.active && rc.waiting ? 'Waiting' : s.mode;

  if (PF.lost) { b.hidden = false; b.className = 'banner warn'; b.textContent = 'Connection lost — reconnecting…'; return; }
  if (s.safety.error_code) {
    b.hidden = false; b.className = 'banner';
    b.innerHTML = '';
    b.append(el('div', {}, el('strong', {}, s.safety.error_code.replace(/_/g, ' ')), el('div', { class: 'help' }, s.safety.error_msg)),
      el('button', { class: 'btn sm', onclick: () => cmd({ cmd: 'stop' }) }, 'Clear & Stop'));
  } else b.hidden = true;
});

// ---------- viewport: iOS standalone apps get the real height late; keep --vh honest ----------
function fitViewport() {
  // the shell is sized by CSS (body: fixed; inset: 0); --vh only remains for the on-screen keyboard case
  const h = window.innerHeight;
  document.documentElement.style.setProperty('--vh', `${Math.round(h)}px`);
}
fitViewport();
for (const ev of ['resize', 'orientationchange', 'pageshow']) window.addEventListener(ev, fitViewport);
window.visualViewport?.addEventListener('resize', fitViewport);
function repaintHeader() { const h = document.querySelector('.topbar'); if (!h) return; h.style.display = 'none'; void h.offsetHeight; h.style.display = ''; }
document.addEventListener('visibilitychange', () => { if (!document.hidden) { fitViewport(); setTimeout(fitViewport, 300); setTimeout(repaintHeader, 350); } });
window.addEventListener('pageshow', () => setTimeout(repaintHeader, 350));
setTimeout(repaintHeader, 1200);
setTimeout(fitViewport, 500);

// ---------- boot ----------
(async () => {
  try { PF.settings = await api('/settings'); PF.units = PF.settings.globals.units; applyTheme(); } catch (e) { toast('Could not load settings', true); }
  try { PF.status = await api('/status'); } catch (e) { /* ws will fill in */ }
  route();
  emit();
  connect();
  updateBadge();
  document.getElementById('bell')?.addEventListener('click', openNotifications);
  document.getElementById('ind-timer')?.addEventListener('click', (e) => headerPanel(e.currentTarget, timerPanel));
  setTimeout(installHint, 2500);
  refreshAlarms();
  document.addEventListener('click', requestAlertPermission, { once: true });
  ensurePushSubscription();   /* keeps a refreshed subscription current without waiting for a tap */
  if ('serviceWorker' in navigator && location.protocol !== 'file:') navigator.serviceWorker.register('/sw.js').catch(() => {});
  checkVersion();
})();
