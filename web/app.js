// PiFire UI core: API client, live status over WebSocket, router, shared widgets.
import { renderHome } from './pages/home.js';
import { renderHistory } from './pages/history.js';
import { renderCook } from './pages/cook.js';
import { renderSettings } from './pages/settings.js';
import { icon as lucide } from './icons.js';
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
export async function api(path, opts = {}) {
  const r = await fetch('/api/v1' + path, {
    method: opts.method || (opts.body ? 'POST' : 'GET'),
    headers: opts.body ? { 'Content-Type': 'application/json' } : undefined,
    body: opts.body ? JSON.stringify(opts.body) : undefined,
  });
  const j = await r.json().catch(() => ({}));
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
for (const ev of ['online', 'pageshow', 'focus']) window.addEventListener(ev, () => setTimeout(reconnectNow, 150));
document.addEventListener('visibilitychange', () => { if (!document.hidden) setTimeout(reconnectNow, 150); });
async function pollStatus() {
  if (PF.connected || document.hidden) return;
  try { const st = await api('/status'); PF.status = st; PF.units = st.units; if (PF.lost) { /* reachable again: the socket will follow */ } emit(); } catch { /* still down */ }
}
let lostTimer = null;
function setConnected(on) {
  PF.connected = on;
  document.getElementById('conn-dot').classList.toggle('on', on);
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
const NOTIF_KEY = 'pf.notifs', SEEN_KEY = 'pf.lastEventTs';
const store = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} };
const load = (k, d) => { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } };
PF.notifs = load(NOTIF_KEY, []);
const KIND_ICON = { error: 'circle-x', warn: 'triangle-alert', ok: 'circle-check', info: 'info' };
function kindOf(code = '', level = 1) {
  if (/^E\d/.test(code) || level >= 3) return 'error';
  if (/Limit_Alarm|Pellet_Level_Low|Autotune_Failed|W\d\d/.test(code) || level === 2) return 'warn';
  if (/Achieved|Timer_Expired|Probe_ETA|Recipe_|Autotune_Done|Tuning_Applied/.test(code)) return 'ok';
  return 'info';
}
function updateBadge() {
  const b = document.getElementById('bell-badge');
  if (!b) return;
  const n = PF.notifs.length;
  b.hidden = n === 0; b.textContent = n > 99 ? '99+' : String(n);
  const bell = document.getElementById('bell');
  if (bell) bell.classList.toggle('has-error', PF.notifs.some((x) => x.kind === 'error'));
}
export function notify({ kind = 'info', title = '', body = '', code = '', ts = Date.now() / 1000 }, { banner = true, keep = true } = {}) {
  const n = { id: `${Math.round(ts * 1000)}-${Math.random().toString(36).slice(2, 6)}`, kind, title, body, code, ts };
  if (keep) { PF.notifs.unshift(n); PF.notifs = PF.notifs.slice(0, 100); store(NOTIF_KEY, PF.notifs); updateBadge(); }
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
  if (m.ts) store(SEEN_KEY, Math.max(load(SEEN_KEY, 0), m.ts));
  try {
    if (document.visibilityState !== 'visible' && 'Notification' in window && Notification.permission === 'granted') new Notification(m.title, { body: m.body, tag: m.code });
    if (navigator.vibrate && kind !== 'info') navigator.vibrate([120, 60, 120]);
  } catch {}
}
// events that happened while the app was closed
async function catchUpEvents() {
  try {
    const evs = await api('/events?limit=40');
    const seen = load(SEEN_KEY, 0);
    let newest = seen;
    if (!seen) { if (evs.length) store(SEEN_KEY, Math.max(...evs.map((e) => e.ts))); return; }   /* first run on this device: start from now */
    for (const e of evs.slice().reverse()) {
      if (e.ts <= seen) continue;
      newest = Math.max(newest, e.ts);
      const kind = kindOf(e.code, e.level);
      if (kind === 'info' && !/Achieved|Timer|ETA|Recipe/.test(e.code)) continue;   /* mode changes and housekeeping stay in the log */
      if (e.code === 'MODE' || e.code.startsWith('SYS_') || e.code.startsWith('UPDATE_')) continue;
      notify({ kind, title: e.code.replace(/_/g, ' '), body: e.message, code: e.code, ts: e.ts }, { banner: false });
    }
    store(SEEN_KEY, newest);
  } catch { /* offline */ }
}
export function requestAlertPermission() {
  if ('Notification' in window && Notification.permission === 'default') Notification.requestPermission().catch(() => {});
}
// Short confirmations ("Saved") are banners only; errors also land in the centre.
export function toast(msg, err = false) {
  notify({ kind: err ? 'error' : 'ok', title: err ? 'Something went wrong' : '', body: msg }, { keep: err });
}
export function openNotifications() {
  return dialog((close) => {
    const wrap = el('div', { class: 'ncenter' });
    const render = () => {
      wrap.innerHTML = '';
      wrap.append(el('div', { class: 'row between' }, el('h3', {}, 'Notifications'), el('button', { class: 'btn sm ghost', type: 'button', disabled: !PF.notifs.length, onclick: () => { PF.notifs = []; store(NOTIF_KEY, []); updateBadge(); render(); } }, 'Clear all')));
      if (!PF.notifs.length) wrap.append(el('div', { class: 'muted', style: 'padding:14px 0' }, 'Nothing to review.'));
      const list = el('div', { class: 'nlist' });
      for (const n of PF.notifs) {
        const w = el('span', { class: `ic-wrap ${n.kind}` }); import('./icons.js').then((m) => w.append(m.icon(KIND_ICON[n.kind] || 'info')));
        list.append(el('div', { class: `nitem ${n.kind}` }, w,
          el('div', { class: 'nt-body' }, n.title ? el('div', { class: 'nt-title' }, n.title) : null, el('div', { class: 'nt-text' }, n.body), el('div', { class: 'meta' }, new Date(n.ts * 1000).toLocaleString([], { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' }))),
          el('button', { class: 'nt-close', 'aria-label': 'Clear', onclick: () => { PF.notifs = PF.notifs.filter((x) => x.id !== n.id); store(NOTIF_KEY, PF.notifs); updateBadge(); render(); } }, '×')));
      }
      wrap.append(list, el('button', { class: 'btn ghost block', type: 'button', style: 'margin-top:10px', onclick: () => close() }, 'Close'));
    };
    render();
    return wrap;
  });
}
export function dialog(build) {
  const d = document.getElementById('dlg');
  d.innerHTML = '';
  const close = (v) => { d.close(); d._resolve?.(v); };
  return new Promise((resolve) => {
    d._resolve = resolve;
    d.append(build(close));
    // the close event is queued asynchronously; ignore one that belongs to a previous dialog
    // when a new one has already been opened in its place
    d.onclose = () => { if (!d.open) d._resolve?.(undefined); };
    d.onclick = (e) => { if (e.target === d) close(undefined); };
    d.showModal();
  });
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
    el('div', {}, el('div', {}, label), help ? el('div', { class: 'help muted', style: 'font-size:.76rem' }, help) : null),
    el('span', { class: 'switch' }, input, el('span')));
}
export function segmented(options, value, onchange) {
  const wrap = el('div', { class: 'segmented' });
  for (const [v, label] of options) {
    wrap.append(el('button', { type: 'button', class: v === value ? 'active' : '', onclick: (e) => { wrap.querySelectorAll('button').forEach((b) => b.classList.remove('active')); e.currentTarget.classList.add('active'); onchange(v); } }, label));
  }
  return wrap;
}

// ---------- iOS-style grouped list: [icon tile][title / subtitle][chevron] ----------
export function listGroup(title, rows, footer) {
  const list = el('div', { class: 'ios-list' }, ...rows.filter(Boolean).map((r) => {
    const tile = r.icon ? el('span', { class: 'tile', style: r.color ? `--tile:${r.color}` : '' }, lucide(r.icon)) : null;
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
  document.getElementById('grill-name').textContent = PF.settings?.globals?.grill_name || 'PiFire';
}

// ---------- router ----------
const pages = { home: renderHome, history: renderHistory, cook: renderCook, settings: renderSettings, more: renderMore, setup: (v) => renderNetwork(v, { captive: true }) };
let teardown = null;
function route() {
  // captive-portal browsers land on /setup by path rather than by hash
  const hash = location.hash.replace(/^#\/?/, '') || (location.pathname === '/setup' ? 'setup' : 'home');
  const [page, ...rest] = hash.split('/');
  const fn = pages[page] || renderHome;
  document.documentElement.dataset.page = pages[page] ? page : 'home';
  const tt = document.getElementById('top-temp'); if (tt) tt.hidden = (pages[page] ? page : 'home') === 'home';
  document.querySelectorAll('.tabbar a').forEach((a) => a.classList.toggle('active', a.dataset.tab === (pages[page] ? page : 'home')));
  if (teardown) { teardown(); teardown = null; }
  const view = document.getElementById('view');
  view.innerHTML = '';
  view.scrollTop = 0;
  const r = fn(view, rest);
  if (typeof r === 'function') teardown = r;
}
window.addEventListener('hashchange', route);

// ---------- banner ----------
onStatus((s) => {
  const b = document.getElementById('banner');
  const pill = document.getElementById('mode-pill');
  if (!s) { b.hidden = !PF.lost; if (PF.lost) { b.className = 'banner warn'; b.textContent = 'Connecting to grill…'; } return; }
  // mode pill: "Startup | 1:15" while a mode counts down, "Hold | 225°F" while holding
  let extra = '';
  if (s.mode === 'Startup' || s.mode === 'Reignite' || s.mode === 'Shutdown' || s.mode === 'Prime') {
    const waiting = (s.mode === 'Startup' || s.mode === 'Reignite') && s.coldstart?.active && !s.coldstart?.reached && s.timers.mode_remaining <= 0;
    extra = fmtDur(waiting ? s.coldstart.remaining : s.timers.mode_remaining);
  } else if (s.mode === 'Hold') extra = `${fmtTemp(s.setpoint)}${degUnit()}`;
  pill.textContent = extra ? `${s.mode} | ${extra}` : s.mode; pill.dataset.mode = s.mode;
  // grill temperature in the header on every page but Home (which shows it large)
  const tt = document.getElementById('top-temp');
  const primary = s.probes?.find((p) => p.role === 'Primary');
  tt.textContent = s.mode === 'Stop' ? `0${degUnit()}` : primary?.valid ? `${fmtTemp(primary.temp)}${degUnit()}` : '—';
  tt.hidden = document.documentElement.dataset.page === 'home';
  if (PF.lost) { b.hidden = false; b.className = 'banner warn'; b.textContent = 'Connection lost — reconnecting…'; return; }
  if (s.safety.error_code) {
    b.hidden = false; b.className = 'banner';
    b.innerHTML = '';
    b.append(el('div', {}, el('strong', {}, s.safety.error_code.replace(/_/g, ' ')), el('div', { class: 'muted', style: 'font-size:.85rem' }, s.safety.error_msg)),
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
  catchUpEvents();
  document.addEventListener('click', requestAlertPermission, { once: true });
  if ('serviceWorker' in navigator && location.protocol !== 'file:') navigator.serviceWorker.register('/sw.js').catch(() => {});
  // after a daemon upgrade the cached shell may be older than the server: reload once so modules match
  try {
    const sys = await api('/system');
    let seen = null;
    try { seen = localStorage.getItem('pf_version'); localStorage.setItem('pf_version', sys.version); } catch { /* storage unavailable */ }
    if (seen && sys.version && seen !== sys.version) { const regs = await navigator.serviceWorker?.getRegistrations?.() || []; for (const r of regs) await r.update(); location.reload(); }
  } catch { /* offline: keep the cached shell */ }
})();
