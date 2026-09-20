import { PF, el, api, cmd, patchSettings, toast, onStatus, confirmDialog, dialog, fmtTime, degUnit, listGroup } from '../app.js';
import { fieldInput, readField } from './settings.js';
import { renderNetwork } from './network.js';
import { renderPellets } from './pellets.js';
import { renderLearning } from './learning.js';

const subpages = { events, logs, system, hardware, probes: () => { location.hash = '#/settings/probes'; }, manual, network, about, pellets: renderPellets, learning: renderLearning };

export function renderMore(view, rest) {
  const page = rest[0];
  if (page && subpages[page]) {
    view.append(el('button', { class: 'btn ghost sm', onclick: () => (location.hash = '#/more') }, '‹ Back'));
    return subpages[page](view, rest.slice(1));
  }
  view.append(
    listGroup('Tools', [
      { href: '#/more/manual', icon: 'wrench', color: '#ff9f0a', title: 'Manual outputs', sub: 'Drive relays directly' },
      { href: '#/more/pellets', icon: 'package', color: '#ac8e68', title: 'Pellets', sub: 'Brands, hopper level and usage' },
      { href: '#/more/learning', icon: 'brain', color: '#bf5af2', title: 'Learning & autotune', sub: 'Feed-forward model, plant estimate, autotune' },
    ]),
    listGroup('Setup', [
      { href: '#/settings/probes', icon: 'thermometer', color: '#ff453a', title: 'Probes', sub: 'Wired and Bluetooth probes, profiles, tuner' },
      { href: '#/more/hardware', icon: 'cpu', color: '#64d2ff', title: 'Hardware setup', sub: 'Board, pins, display, hopper sensor' },
      { href: '#/more/network', icon: 'wifi', color: '#0a84ff', title: 'Network', sub: 'Wi-Fi and hotspot' },
    ]),
    listGroup('Diagnostics', [
      { href: '#/more/events', icon: 'scroll-text', color: '#ffd60a', title: 'Events', sub: 'Alerts and mode changes' },
      { href: '#/more/logs', icon: 'file-text', color: '#8e8e93', title: 'Logs', sub: 'Daemon log' },
      { href: '#/more/system', icon: 'monitor', color: '#8e8e93', title: 'System', sub: 'Health, updates, restart, power' },
      { href: '#/more/about', icon: 'info', color: '#8e8e93', title: 'About', sub: '' },
    ]));
}

function events(view) {
  const list = el('div', { class: 'list' });
  view.append(el('h2', {}, 'Events'), el('div', { class: 'card' }, list));
  api('/events?limit=200').then((evs) => {
    list.innerHTML = '';
    for (const e of evs) list.append(el('div', { class: 'item' }, el('div', {}, el('div', { class: e.level >= 3 ? 'lvl-error' : e.level === 2 ? 'lvl-warn' : '' }, e.message), el('div', { class: 'meta' }, `${e.code} · ${new Date(e.ts * 1000).toLocaleString()}`))));
    if (!evs.length) list.append(el('div', { class: 'muted' }, 'No events yet'));
  });
}

function logs(view) {
  const box = el('div', { class: 'mono card' });
  view.append(el('h2', {}, 'Log'), box);
  const load = () => api('/logs?limit=300').then((lines) => { box.innerHTML = ''; for (const l of lines) box.append(el('div', { class: `log-line lvl-${l.level}` }, `${fmtTime(l.ts)} ${l.msg}`)); box.scrollTop = box.scrollHeight; });
  load();
  const t = setInterval(load, 5000);
  return () => clearInterval(t);
}

function system(view) {
  const kv = el('div', { class: 'kv' });
  view.append(el('h2', {}, 'System'), el('div', { class: 'card' }, kv));
  const load = async () => {
    const s = await api('/system');
    kv.innerHTML = '';
    const rows = [['Version', s.version], ['Hostname', s.hostname], ['Uptime', `${Math.floor(s.uptime_s / 3600)}h ${Math.floor((s.uptime_s % 3600) / 60)}m`],
      ['CPU temperature', s.cpu_temp_c > 0 ? `${s.cpu_temp_c.toFixed(1)} °C` : '—'], ['Load', s.load1?.toFixed(2)], ['Memory free', `${(s.mem_available / 1048576).toFixed(0)} MB of ${(s.mem_total / 1048576).toFixed(0)} MB`],
      ['Wi-Fi quality', s.wifi_quality_pct >= 0 ? `${s.wifi_quality_pct.toFixed(0)}%` : '—'], ['Throttled', s.throttled == null ? '—' : s.throttled ? 'YES' : 'no'], ['Under-voltage', s.under_voltage == null ? '—' : s.under_voltage ? 'YES' : 'no']];
    for (const i of s.interfaces || []) rows.push([i.name, `${i.ip}${i.mac ? ' · ' + i.mac : ''}`]);
    for (const [k, v] of rows) kv.append(el('div', {}, k), el('div', {}, v ?? '—'));
  };
  load();

  // ---- software updates from GitHub Releases ----
  const upd = el('div', { class: 'card' });
  const renderUpd = (u) => {
    upd.innerHTML = '';
    const busy = u.busy;
    const rows = el('div', { class: 'kv' }, el('div', {}, 'Installed'), el('div', {}, `${u.current} (${u.arch})`), el('div', {}, 'Latest release'), el('div', {}, u.latest || '—'),
      el('div', {}, 'Source'), el('div', {}, u.repo ? el('a', { href: `https://github.com/${u.repo}/releases`, target: '_blank' }, u.repo) : '— (set in Settings → Admin)'));
    upd.append(el('h3', {}, 'Software'), rows, el('p', { class: 'muted', style: 'font-size:.85rem;margin:8px 0' }, u.state === 'error' ? `⚠ ${u.message}` : u.message + (u.state === 'downloading' ? ` ${(u.progress * 100).toFixed(0)}%` : '')));
    if (u.state === 'downloading') upd.append(el('div', { class: 'progress' }, el('div', { style: `width:${(u.progress * 100).toFixed(0)}%` })));
    if (u.available && u.notes) upd.append(el('details', {}, el('summary', { class: 'muted' }, `What's new in ${u.latest}`), el('div', { class: 'mono', style: 'margin-top:6px' }, u.notes)));
    upd.append(el('div', { class: 'btnrow', style: 'margin-top:10px' },
      el('button', { class: 'btn', disabled: busy, onclick: async () => { try { await api('/update/check', { body: {} }); poll(); } catch (e) { toast(e.message, true); } } }, 'Check for updates'),
      el('button', { class: 'btn primary', disabled: busy || !u.installable, onclick: async () => {
        if (!await confirmDialog(`Install ${u.latest}?`, 'The grill must be stopped. The release is downloaded, its checksum verified, then the service reinstalls and restarts (about a minute). This page reloads when it is back.', 'Install')) return;
        try { await api('/update/install', { body: {} }); poll(); } catch (e) { toast(e.message, true); }
      } }, u.available ? `Install ${u.latest}` : 'Up to date')));
  };
  let pollT = null;
  const poll = async () => {
    try { const u = await api('/update'); renderUpd(u); if (u.busy) { clearTimeout(pollT); pollT = setTimeout(poll, 1000); } } catch { /* daemon restarting during install */ }
  };
  poll();
  view.append(upd);

  view.append(el('div', { class: 'btnrow' },
    el('button', { class: 'btn', onclick: async () => { if (await confirmDialog('Reboot?', 'The grill must be stopped first.', 'Reboot')) api('/admin/reboot', { body: {} }).then(() => toast('Rebooting…')).catch((e) => toast(e.message, true)); } }, 'Reboot'),
    el('button', { class: 'btn danger', onclick: async () => { if (await confirmDialog('Power off?', 'The grill must be stopped first.', 'Power off', true)) api('/admin/poweroff', { body: {} }).then(() => toast('Powering off…')).catch((e) => toast(e.message, true)); } }, 'Power off')));
  const t = setInterval(load, 10000);
  return () => { clearInterval(t); clearTimeout(pollT); };
}

function manual(view) {
  const card = el('div', { class: 'card' });
  view.append(el('h2', {}, 'Manual outputs'), el('div', { class: 'card muted', style: 'font-size:.85rem' }, 'Outputs can be driven directly in Manual mode, or temporarily while cooking if "Allow manual output changes" is enabled in Safety. The auger safety cap still applies.'), card);
  const update = (s) => {
    if (!s) return;
    card.innerHTML = '';
    const manual = s.mode === 'Manual', allowed = manual || PF.settings?.safety?.allow_manual_changes;
    if (!manual) card.append(el('button', { class: 'btn block', onclick: () => cmd({ cmd: 'mode', mode: 'Manual' }), disabled: !(s.mode === 'Stop' || s.mode === 'Monitor') }, 'Enter Manual mode (from Stop)'));
    for (const o of ['power', 'fan', 'auger', 'igniter']) {
      const on = s.outputs[o];
      card.append(el('div', { class: 'toggle' }, el('div', {}, o[0].toUpperCase() + o.slice(1)),
        el('label', { class: 'switch' }, el('input', { type: 'checkbox', checked: on, disabled: !allowed, onchange: (e) => cmd({ cmd: 'manual', output: o, on: e.target.checked }) }), el('span'))));
    }
    if (PF.settings?.platform?.dc_fan) {
      const r = el('input', { type: 'range', min: 0, max: 100, value: s.outputs.fan_pct, disabled: !allowed, onchange: (e) => cmd({ cmd: 'manual', output: 'pwm', pct: Number(e.target.value) }) });
      card.append(el('div', { class: 'field' }, el('label', {}, `Fan speed ${s.outputs.fan_pct}%`), r));
    }
    if (manual) card.append(el('button', { class: 'btn danger block', onclick: () => cmd({ cmd: 'stop' }) }, 'Stop (all off)'));
  };
  update(PF.status);
  return onStatus(update);
}

function network(view) {
  return renderNetwork(view);
}

function about(view) {
  view.append(el('div', { class: 'card' }, el('h3', {}, 'PiFire'), el('p', { class: 'muted' }, 'Pellet grill controller, rewritten in C for the Raspberry Pi Zero 2 W and up. MIT licensed. Includes civetweb, cJSON, SQLite and uPlot.')));
}

// ---- hardware wizard: board + pins + probe devices, from the manifest ----
async function hardware(view) {
  const man = await api('/manifest');
  const plat = structuredClone(PF.settings.platform);
  const map = structuredClone(PF.settings.probe_settings.probe_map);
  const boards = man.modules.grillplatform;
  const boardCard = el('div', { class: 'card' });
  const get = (o, ks) => ks.reduce((a, k) => (a == null ? undefined : a[k]), o);
  const setp = (o, ks, v) => { let a = o; for (const k of ks.slice(0, -1)) a = a[k] ??= {}; a[ks.at(-1)] = v; };

  const renderBoard = () => {
    boardCard.innerHTML = '';
    const cur = boards[plat.current] ? plat.current : 'custom';
    boardCard.append(el('div', { class: 'field' }, el('label', {}, 'Board'), el('select', { onchange: (e) => { plat.current = e.target.value; applyDefaults(); applyBoardProbes(); renderBoard(); } }, Object.entries(boards).map(([id, b]) => el('option', { value: id, selected: id === cur }, b.friendly_name)))),
      el('p', { class: 'muted', style: 'font-size:.85rem' }, boards[cur].description));
    for (const [key, dep] of Object.entries(boards[cur].settings_dependencies)) {
      if (dep.hidden || key === 'current') continue;
      const path = dep.settings.slice(1); // drop leading "platform"
      const val = get(plat, path);
      boardCard.append(el('div', { class: 'field inline' }, el('div', {}, el('label', {}, dep.friendly_name), el('div', { class: 'help' }, dep.description)),
        el('select', { onchange: (e) => setp(plat, path, coerce(e.target.value)) }, Object.entries(dep.options).map(([v, l]) => el('option', { value: v, selected: String(val ?? 'None') === v || (val === null && v === 'None') }, l)))));
    }
  };
  const coerce = (v) => (v === 'None' ? null : v === 'True' ? true : v === 'False' ? false : /^-?\d+$/.test(v) ? Number(v) : v);
  const applyDefaults = () => {
    const b = boards[plat.current];
    for (const dep of Object.values(b.settings_dependencies)) {
      const path = dep.settings.slice(1);
      const opts = Object.keys(dep.options);
      const cur = get(plat, path);
      if (dep.hidden || !opts.includes(String(cur ?? 'None'))) setp(plat, path, coerce(opts[0]));
    }
  };

  // each PCB ships with a known ADC + probe wiring; adopt it when the board changes
  const applyBoardProbes = () => {
    const b = man.boards?.[plat.current];
    if (!b?.probe_map) return;
    map.probe_devices = structuredClone(b.probe_map.probe_devices);
    map.probe_info = structuredClone(b.probe_map.probe_info);
    toast(`Default probe map for ${b.name} applied (edit under Settings → Probes)`);
  };

  // display and hopper sensor modules, each with the config fields its manifest entry declares
  const mods = structuredClone(PF.settings.modules || {});
  const dispCfg = structuredClone(PF.settings.display || {});
  const distCfg = structuredClone(PF.settings.distance || {});
  const moduleCard = (title, modules, key, cfg) => {
    const card = el('div', { class: 'card' });
    const render = () => {
      card.innerHTML = '';
      const cur = modules[mods[key]] ? mods[key] : 'none';
      card.append(el('div', { class: 'field' }, el('label', {}, title), el('select', { onchange: (e) => { mods[key] = e.target.value; render(); } }, Object.entries(modules).map(([id, m]) => el('option', { value: id, selected: id === cur }, m.friendly_name)))),
        el('p', { class: 'muted', style: 'font-size:.85rem' }, modules[cur].description || ''));
      for (const c of modules[cur].config || []) {
        if (c.hidden) continue;
        const v = cfg[c.label] ?? c.default;
        const input = c.type === 'list'
          ? el('select', { onchange: (e) => (cfg[c.label] = coerce(e.target.value)) }, c.list_values.map((lv, k) => el('option', { value: lv, selected: String(v) === String(lv) }, c.list_labels?.[k] ?? lv)))
          : el('input', { type: 'text', inputmode: 'decimal', value: v ?? '', onchange: (e) => (cfg[c.label] = c.type === 'int' || c.type === 'float' ? Number(e.target.value) : e.target.value) });
        card.append(el('div', { class: 'field inline' }, el('div', {}, el('label', {}, c.friendly_name), el('div', { class: 'help' }, c.description)), input));
      }
    };
    render();
    return card;
  };
  const displayCard = moduleCard('Display', man.modules.display, 'display', dispCfg);
  const distCard = moduleCard('Hopper level sensor', man.modules.distance, 'dist', distCfg);

  renderBoard();
  view.append(el('h2', {}, 'Board'), boardCard, el('h2', {}, 'Display'), displayCard, el('h2', {}, 'Hopper sensor'), distCard,
    el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'button', onclick: async () => {
      try {
        plat.system_type = plat.system_type || 'rpi';
        await patchSettings('platform', plat);
        await patchSettings('probe_settings', { probe_map: { probe_devices: map.probe_devices, probe_info: map.probe_info } });
        await patchSettings('modules', { display: mods.display || 'none', dist: mods.dist || 'none' });
        await patchSettings('display', dispCfg);
        await patchSettings('distance', distCfg);
        if (PF.status?.sim) { toast('Saved'); return; }
        // write the boot configuration (relay pulls, PWM overlay, I2C/SPI/1-Wire) for this board
        let boot;
        try { boot = await api('/admin/boardcfg', { body: {} }); } catch (e) { toast(`Saved, but boot config failed: ${e.message}`, true); return; }
        if (boot.reboot && await confirmDialog('Reboot now?', 'The boot configuration changed (I2C/SPI/PWM/relay pulls). A reboot is needed before the new hardware works.', 'Reboot')) {
          await api('/admin/reboot', { body: {} }); toast('Rebooting…');
        } else toast(boot.reboot ? 'Saved — reboot to apply the boot configuration' : 'Saved');
      } catch (e) { toast(e.message, true); }
    } }, 'Save hardware')));
}
