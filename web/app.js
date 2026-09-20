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
let ws, wsTimer;
function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { PF.everConnected = true; PF.wsRetry = 0; setConnected(true); };
  ws.onclose = () => { setConnected(false); clearTimeout(wsTimer); const wait = Math.min(5000, 500 * 2 ** Math.min(4, PF.wsRetry++ || 0)); wsTimer = setTimeout(connect, wait); };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.type === 'status') { PF.status = m; PF.units = m.units; emit(); }
    else if (m.type === 'error') toast(m.msg, true);
    else if (m.type === 'event') { PF.alertGen = (PF.alertGen || 0) + 1; alert(m); emit(); }
  };
}
let lostTimer = null;
function setConnected(on) {
  PF.connected = on;
  document.getElementById('conn-dot').classList.toggle('on', on);
  // the banner only appears after the link has been down for a while (a reconnect takes < 1 s and must not flash)
  clearTimeout(lostTimer);
  if (on) { PF.lost = false; emit(); }
  else lostTimer = setTimeout(() => { PF.lost = true; emit(); }, 4000);
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
// Alert: in-page banner-toast plus a system notification when the page is in the background.
function alert(m) {
  const t = document.getElementById('toast');
  t.innerHTML = '';
  t.append(el('strong', {}, m.title), ' ', el('span', { class: 'muted' }, m.body));
  t.hidden = false; t.classList.toggle('err', /^E\d/.test(m.code));
  clearTimeout(t._h); t._h = setTimeout(() => (t.hidden = true), 8000);
  try {
    if (document.visibilityState !== 'visible' && 'Notification' in window && Notification.permission === 'granted') new Notification(m.title, { body: m.body, tag: m.code });
    if (navigator.vibrate) navigator.vibrate([120, 60, 120]);
  } catch {}
}
export function requestAlertPermission() {
  if ('Notification' in window && Notification.permission === 'default') Notification.requestPermission().catch(() => {});
}
export function toast(msg, err = false) {
  const t = document.getElementById('toast');
  t.textContent = msg; t.hidden = false; t.classList.toggle('err', err);
  clearTimeout(t._h); t._h = setTimeout(() => (t.hidden = true), err ? 4000 : 2000);
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
document.addEventListener('visibilitychange', () => { if (!document.hidden) { fitViewport(); setTimeout(fitViewport, 300); } });
setTimeout(fitViewport, 500);

// ---------- boot ----------
(async () => {
  try { PF.settings = await api('/settings'); PF.units = PF.settings.globals.units; applyTheme(); } catch (e) { toast('Could not load settings', true); }
  try { PF.status = await api('/status'); } catch (e) { /* ws will fill in */ }
  route();
  emit();
  connect();
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
