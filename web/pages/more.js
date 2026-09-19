import { PF, el, api, cmd, patchSettings, toast, onStatus, confirmDialog, dialog, fmtTime, degUnit } from '../app.js';
import { fieldInput, readField } from './settings.js';
import { renderNetwork } from './network.js';
import { renderPellets } from './pellets.js';

const subpages = { events, logs, system, hardware, probes, manual, network, about, pellets: renderPellets };

export function renderMore(view, rest) {
  const page = rest[0];
  if (page && subpages[page]) {
    view.append(el('button', { class: 'btn ghost sm', onclick: () => (location.hash = '#/more') }, '‹ Back'));
    return subpages[page](view, rest.slice(1));
  }
  view.append(el('div', { class: 'menu' },
    ...[['pellets', 'Pellets', 'Brands, hopper level and usage'], ['hardware', 'Hardware setup', 'Board, pins and probe devices'], ['probes', 'Probes', 'Names, types and profiles'], ['network', 'Network', 'Wi-Fi and hotspot'],
        ['manual', 'Manual outputs', 'Drive relays directly'], ['events', 'Events', 'Alerts and mode changes'], ['logs', 'Logs', 'Daemon log'], ['system', 'System', 'Health, restart, power'], ['about', 'About', '']]
      .map(([id, title, sub]) => el('button', { class: 'btn', onclick: () => (location.hash = `#/more/${id}`) }, el('div', {}, el('div', {}, title), el('div', { class: 'muted', style: 'font-size:.76rem;font-weight:400' }, sub))))));
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
  view.append(el('div', { class: 'btnrow' },
    el('button', { class: 'btn', onclick: async () => { if (await confirmDialog('Reboot?', 'The grill must be stopped first.', 'Reboot')) api('/admin/reboot', { body: {} }).then(() => toast('Rebooting…')).catch((e) => toast(e.message, true)); } }, 'Reboot'),
    el('button', { class: 'btn danger', onclick: async () => { if (await confirmDialog('Power off?', 'The grill must be stopped first.', 'Power off', true)) api('/admin/poweroff', { body: {} }).then(() => toast('Powering off…')).catch((e) => toast(e.message, true)); } }, 'Power off')));
  const t = setInterval(load, 10000);
  return () => clearInterval(t);
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

// ---- probes: names, roles, profiles ----
function probes(view) {
  const map = structuredClone(PF.settings.probe_settings.probe_map);
  const profiles = PF.settings.probe_settings.probe_profiles;
  const card = el('div', { class: 'card' });
  const render = () => {
    card.innerHTML = '';
    map.probe_info.forEach((p, i) => {
      const f = el('fieldset', { class: 'field' }, el('legend', {}, p.label),
        el('div', { class: 'field inline' }, el('label', {}, 'Name'), el('input', { type: 'text', value: p.name, onchange: (e) => (p.name = e.target.value) })),
        el('div', { class: 'field inline' }, el('label', {}, 'Type'), el('select', { onchange: (e) => (p.type = e.target.value) }, ['Primary', 'Food', 'Aux'].map((t) => el('option', { value: t, selected: p.type === t }, t)))),
        el('div', { class: 'field inline' }, el('label', {}, 'Device / port'), el('select', { onchange: (e) => { [p.device, p.port] = e.target.value.split('|'); } },
          map.probe_devices.flatMap((d) => (d.ports || []).map((port) => el('option', { value: `${d.device}|${port}`, selected: p.device === d.device && p.port === port }, `${d.device} · ${port}`))))),
        el('div', { class: 'field inline' }, el('label', {}, 'Profile'), el('select', { onchange: (e) => (p.profile = e.target.value) }, Object.values(profiles).map((pr) => el('option', { value: pr.id, selected: (p.profile?.id || p.profile) === pr.id }, pr.name)))),
        el('div', { class: 'toggle' }, el('div', {}, 'Enabled'), el('label', { class: 'switch' }, el('input', { type: 'checkbox', checked: p.enabled, onchange: (e) => (p.enabled = e.target.checked) }), el('span'))),
        p.type === 'Aux' ? el('div', { class: 'toggle' }, el('div', {}, 'Ambient reference', el('div', { class: 'help muted', style: 'font-size:.76rem' }, 'Used by the adaptive controller and cold-start')), el('label', { class: 'switch' }, el('input', { type: 'checkbox', checked: !!p.ambient, onchange: (e) => (p.ambient = e.target.checked) }), el('span'))) : null,
        el('button', { class: 'btn sm ghost', type: 'button', onclick: () => { map.probe_info.splice(i, 1); render(); } }, 'Remove probe'));
      card.append(f);
    });
    card.append(el('div', { class: 'form-actions' },
      el('button', { class: 'btn', type: 'button', onclick: () => { const n = map.probe_info.length + 1; map.probe_info.push({ type: 'Food', label: `Probe${n}`, name: `Probe-${n}`, profile: 'TWPS00', device: map.probe_devices[0]?.device || '', port: map.probe_devices[0]?.ports?.[0] || '', enabled: true }); render(); } }, 'Add probe'),
      el('button', { class: 'btn primary', type: 'button', onclick: async () => {
        if (map.probe_info.filter((p) => p.type === 'Primary' && p.enabled).length !== 1) { toast('Exactly one enabled Primary probe is required', true); return; }
        const labels = new Set();
        for (const p of map.probe_info) { p.label = p.name.replace(/[^A-Za-z0-9]/g, '') || 'Probe'; if (labels.has(p.label)) { toast(`Duplicate probe name ${p.name}`, true); return; } labels.add(p.label); }
        try { await patchSettings('probe_settings', { probe_map: { probe_info: map.probe_info } }); toast('Probes saved'); } catch (e) { toast(e.message, true); }
      } }, 'Save probes')));
  };
  render();
  view.append(el('h2', {}, 'Probes'), card);
}

// ---- hardware wizard: board + pins + probe devices, from the manifest ----
async function hardware(view) {
  const man = await api('/manifest');
  const plat = structuredClone(PF.settings.platform);
  const map = structuredClone(PF.settings.probe_settings.probe_map);
  const boards = man.modules.grillplatform;
  const boardCard = el('div', { class: 'card' });
  const devCard = el('div', { class: 'card' });
  const get = (o, ks) => ks.reduce((a, k) => (a == null ? undefined : a[k]), o);
  const setp = (o, ks, v) => { let a = o; for (const k of ks.slice(0, -1)) a = a[k] ??= {}; a[ks.at(-1)] = v; };

  const renderBoard = () => {
    boardCard.innerHTML = '';
    const cur = boards[plat.current] ? plat.current : 'custom';
    boardCard.append(el('div', { class: 'field' }, el('label', {}, 'Board'), el('select', { onchange: (e) => { plat.current = e.target.value; applyDefaults(); renderBoard(); } }, Object.entries(boards).map(([id, b]) => el('option', { value: id, selected: id === cur }, b.friendly_name)))),
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

  const renderDevices = () => {
    devCard.innerHTML = '';
    map.probe_devices.forEach((d, i) => {
      const m = man.modules.probes[d.module] || Object.values(man.modules.probes).find((x) => x.filename === d.module);
      const fs = el('fieldset', { class: 'field' }, el('legend', {}, `${d.device} — ${m?.friendly_name || d.module}`));
      for (const c of m?.device_specific?.config || []) {
        if (c.hidden) continue;
        const v = d.config?.[c.label] ?? c.default;
        let input;
        if (c.type === 'list') input = el('select', { onchange: (e) => ((d.config ??= {})[c.label] = e.target.value) }, c.list_values.map((lv, k) => el('option', { value: lv, selected: String(v) === String(lv) }, c.list_labels?.[k] ?? lv)));
        else if (c.type === 'bt_address') {
          const addr = el('input', { type: 'text', value: v ?? '', placeholder: 'any / scan', style: 'width:150px', onchange: (e) => ((d.config ??= {})[c.label] = e.target.value.trim()) });
          input = el('div', { class: 'row' }, addr, el('button', { class: 'btn sm', type: 'button', onclick: async (e) => {
            e.target.disabled = true; e.target.textContent = 'Scanning…';
            try {
              const found = await api('/probes/ble/scan?seconds=8', { body: {} });
              const pick = await dialog((close) => el('div', {}, el('h3', {}, 'Bluetooth devices'),
                el('div', { class: 'opts' }, found.length ? found.map((f) => el('button', { class: 'btn', type: 'button', onclick: () => close(f.address) }, `${f.name || 'Unknown'} · ${f.address}${f.rssi ? ` · ${f.rssi} dBm` : ''}`)) : el('div', { class: 'muted' }, 'Nothing found — make sure the probe is on and nearby.')),
                el('button', { class: 'btn ghost block', type: 'button', onclick: () => close(undefined) }, 'Cancel')));
              if (pick) { addr.value = pick; (d.config ??= {})[c.label] = pick; }
            } catch (err) { toast(err.message, true); }
            e.target.disabled = false; e.target.textContent = 'Scan';
          } }, 'Scan'));
        }
        else input = el('input', { type: 'text', inputmode: 'decimal', value: v ?? '', onchange: (e) => ((d.config ??= {})[c.label] = c.type === 'int' || c.type === 'float' ? Number(e.target.value) : e.target.value) });
        fs.append(el('div', { class: 'field inline' }, el('div', {}, el('label', {}, c.friendly_name), el('div', { class: 'help' }, c.description)), input));
      }
      fs.append(el('button', { class: 'btn sm ghost', type: 'button', onclick: () => { map.probe_devices.splice(i, 1); renderDevices(); } }, 'Remove device'));
      devCard.append(fs);
    });
    const sel = el('select', {}, Object.entries(man.modules.probes).map(([id, m]) => el('option', { value: id }, m.friendly_name)));
    devCard.append(el('div', { class: 'row' }, sel, el('button', { class: 'btn sm', type: 'button', onclick: () => {
      const id = sel.value, m = man.modules.probes[id];
      const cfg = {}; for (const c of m.device_specific?.config || []) cfg[c.label] = c.default;
      map.probe_devices.push({ device: `${id}_${map.probe_devices.length + 1}`, module: m.filename, ports: m.device_specific?.ports || [], config: cfg });
      renderDevices();
    } }, 'Add device')));
  };

  renderBoard(); renderDevices();
  view.append(el('h2', {}, 'Board'), boardCard, el('h2', {}, 'Probe devices'), devCard,
    el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'button', onclick: async () => {
      try {
        plat.system_type = plat.system_type || 'rpi';
        await patchSettings('platform', plat);
        await patchSettings('probe_settings', { probe_map: { probe_devices: map.probe_devices } });
        toast(boards[plat.current]?.reboot_required ? 'Saved — reboot to apply pin changes' : 'Saved');
      } catch (e) { toast(e.message, true); }
    } }, 'Save hardware')));
}
