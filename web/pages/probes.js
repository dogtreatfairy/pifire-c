import { PF, el, api, patchSettings, toast, confirmDialog, dialog, degUnit } from '../app.js';
import { icon as lucide } from '../icons.js';

// One place for everything probe-related: the probe table (tap a row to edit), adding probes to free
// ADC ports or pairing Bluetooth probes, the ADC/RTD hardware, and the Steinhart-Hart profiles + tuner.

export const btIcon = () => lucide('bluetooth', 'ic bt');
const WIRELESS_MODULES = ['ibbq', 'meater'];
/** true when the named probe device is a Bluetooth probe (by module) */
export function isWireless(deviceName) {
  const d = (PF.settings?.probe_settings?.probe_map?.probe_devices || []).find((x) => x.device === deviceName);
  return !!d && WIRELESS_MODULES.includes(d.module);
}

export async function renderProbes(view) {
  const man = await api('/manifest');
  const mods = man.modules.probes;
  const moduleOf = (d) => mods[d.module] || Object.values(mods).find((m) => m.filename === d.module);
  const wirelessMod = (m) => !!m?.device_specific?.config?.some((c) => c.type === 'bt_address');
  const map = structuredClone(PF.settings.probe_settings.probe_map);
  map.probe_devices ??= []; map.probe_info ??= [];
  const profiles = PF.settings.probe_settings.probe_profiles;
  const profName = (p) => profiles[p.profile?.id || p.profile]?.name || p.profile?.name || String(p.profile || '—');

  const save = async () => {
    if (map.probe_info.filter((p) => p.type === 'Primary' && p.enabled).length !== 1) { toast('Exactly one enabled Primary (pit) probe is required', true); return false; }
    const labels = new Set();
    for (const p of map.probe_info) { p.label = p.name.replace(/[^A-Za-z0-9]/g, '') || 'Probe'; if (labels.has(p.label)) { toast(`Duplicate probe name ${p.name}`, true); return false; } labels.add(p.label); }
    try { await patchSettings('probe_settings', { probe_map: { probe_devices: map.probe_devices, probe_info: map.probe_info } }); toast('Probes saved'); renderAll(); return true; }
    catch (e) { toast(e.message, true); return false; }
  };
  const freePorts = (except) => map.probe_devices.flatMap((d) => (d.ports || []).filter((port) => !map.probe_info.some((p) => p !== except && p.device === d.device && p.port === port)).map((port) => ({ device: d.device, port, wireless: WIRELESS_MODULES.includes(d.module) })));

  // ---- editor
  const editProbe = (p, isNew = false) => dialog((close) => {
    const draft = { ...p };
    const f = (label, input) => el('div', { class: 'field inline' }, el('label', {}, label), input);
    const ports = [...freePorts(p), ...(p.device ? [{ device: p.device, port: p.port, wireless: isWireless(p.device) }] : [])];
    const portSel = el('select', { onchange: (e) => { [draft.device, draft.port] = e.target.value.split('|'); } }, ports.map((o) => el('option', { value: `${o.device}|${o.port}`, selected: o.device === p.device && o.port === p.port }, `${o.wireless ? '⌁ ' : ''}${o.device} · ${o.port}`)));
    const tog = (label, key, help) => el('div', { class: 'toggle' }, el('div', {}, label, help ? el('div', { class: 'help muted', style: 'font-size:.76rem' }, help) : null), el('label', { class: 'switch' }, el('input', { type: 'checkbox', checked: !!draft[key], onchange: (e) => (draft[key] = e.target.checked) }), el('span')));
    return el('div', {}, el('h3', {}, isNew ? 'New probe' : p.name),
      f('Name', el('input', { type: 'text', value: draft.name, onchange: (e) => (draft.name = e.target.value.trim()) })),
      f('Type', el('select', { onchange: (e) => (draft.type = e.target.value) }, [['Primary', 'Primary (pit)'], ['Food', 'Food'], ['Aux', 'Aux / ambient']].map(([v, l]) => el('option', { value: v, selected: draft.type === v }, l)))),
      f('Port', portSel),
      f('Profile', el('select', { onchange: (e) => (draft.profile = e.target.value) }, Object.values(profiles).map((pr) => el('option', { value: pr.id, selected: (draft.profile?.id || draft.profile) === pr.id }, pr.name)))),
      tog('Enabled', 'enabled'), tog('Show on Home screen', 'show_on_home', 'Also on the grill display'), tog('Ambient reference', 'ambient', 'Aux only: used by learning and cold start'),
      el('div', { class: 'btnrow', style: 'margin-top:12px' },
        isNew ? null : el('button', { class: 'btn ghost', type: 'button', onclick: async () => { if (await confirmDialog('Remove probe?', p.name, 'Remove', true)) { map.probe_info.splice(map.probe_info.indexOf(p), 1); close('removed'); } } }, 'Remove'),
        el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'),
        el('button', { class: 'btn primary', type: 'button', onclick: () => { if (!draft.name) { toast('Name required', true); return; } Object.assign(p, draft); close('saved'); } }, 'Save')));
  }).then(async (r) => { if (r) await save(); });

  // ---- add: free wired port, or pair a Bluetooth probe
  const addProbe = () => dialog((close) => {
    const free = freePorts(null).filter((o) => !o.wireless);
    const btMods = Object.entries(mods).filter(([, m]) => wirelessMod(m));
    return el('div', {}, el('h3', {}, 'Add probe'),
      el('div', { class: 'muted', style: 'font-size:.85rem;margin-bottom:6px' }, free.length ? 'Free wired ports' : 'No free wired ports (add an ADC under Probe hardware)'),
      el('div', { class: 'opts' }, ...free.map((o) => el('button', { class: 'btn', type: 'button', onclick: () => { close(); const n = map.probe_info.length + 1; const p = { type: 'Food', label: `Probe${n}`, name: `Probe ${n}`, profile: 'TWPS00', device: o.device, port: o.port, enabled: true, show_on_home: true }; map.probe_info.push(p); editProbe(p, true).then(() => { if (!map.probe_info.includes(p)) return; }); } }, `${o.device} · ${o.port}`))),
      el('div', { class: 'muted', style: 'font-size:.85rem;margin:10px 0 6px' }, 'Bluetooth'),
      el('div', { class: 'opts' }, ...btMods.map(([id, m]) => el('button', { class: 'btn', type: 'button', onclick: () => { close(); pairBluetooth(id, m); } }, btIcon(), ` Pair ${m.friendly_name}`))),
      el('button', { class: 'btn ghost block', type: 'button', style: 'margin-top:10px', onclick: () => close() }, 'Cancel'));
  });
  const pairBluetooth = async (id, m) => {
    const addr = await dialog((close) => {
      const list = el('div', { class: 'opts' }, el('div', { class: 'muted' }, 'Scanning… make sure the probe is on and nearby.'));
      api('/probes/ble/scan?seconds=8', { body: {} }).then((found) => {
        list.innerHTML = '';
        if (!found.length) list.append(el('div', { class: 'muted' }, 'Nothing found. Turn the probe on and try again.'));
        for (const f of found) list.append(el('button', { class: 'btn', type: 'button', onclick: () => close(f.address) }, `${f.name || 'Unknown'} · ${f.address}${f.rssi ? ` · ${f.rssi} dBm` : ''}`));
      }).catch((e) => { list.innerHTML = ''; list.append(el('div', { class: 'muted' }, e.message)); });
      return el('div', {}, el('h3', {}, `Pair ${m.friendly_name}`), list, el('button', { class: 'btn ghost block', type: 'button', style: 'margin-top:10px', onclick: () => close(undefined) }, 'Cancel'));
    });
    if (!addr) return;
    const n = map.probe_devices.filter((d) => d.module === m.filename).length + 1;
    const cfg = {}; for (const c of m.device_specific?.config || []) cfg[c.label] = c.type === 'bt_address' ? addr : c.default;
    cfg.transient = true;
    const dev = { device: `${m.friendly_name.replace(/[^A-Za-z0-9]/g, '')}${n}`, module: m.filename, ports: m.device_specific?.ports || [], config: cfg };
    map.probe_devices.push(dev);
    dev.ports.forEach((port, i) => map.probe_info.push({ type: 'Food', label: `${dev.device}${i + 1}`, name: `${m.friendly_name.split(' ')[0]} ${i + 1}`, profile: 'TWPS00', device: dev.device, port, enabled: true, show_on_home: i < 3 }));
    await save();
  };

  // ---- table
  const table = el('div', { class: 'list ptable' });
  const renderTable = () => {
    table.innerHTML = '';
    for (const p of map.probe_info) {
      const live = PF.status?.probes?.find((x) => x.label === p.label);
      table.append(el('button', { class: `item prow-btn ${p.enabled ? '' : 'off'}`, type: 'button', onclick: () => editProbe(p) },
        el('div', { class: 'pcol' }, el('div', { class: 'row', style: 'gap:6px' }, isWireless(p.device) ? btIcon() : null, el('strong', {}, p.name), el('span', { class: 'pill sm' }, p.type)),
          el('div', { class: 'meta' }, `${p.device} · ${p.port} · ${profName(p)}${p.show_on_home === false ? ' · hidden on Home' : ''}`)),
        el('div', { class: 'pval' }, p.enabled ? (live?.valid ? `${live.temp}${degUnit()}` : '—') : 'off')));
    }
    if (!map.probe_info.length) table.append(el('div', { class: 'muted' }, 'No probes yet.'));
  };

  // ---- hardware: wired ADC / RTD / thermocouple devices
  const hwCard = el('div', { class: 'card' });
  const renderHw = () => {
    hwCard.innerHTML = '';
    map.probe_devices.forEach((d, i) => {
      const m = moduleOf(d);
      const fs = el('fieldset', { class: 'field' }, el('legend', {}, wirelessMod(m) ? btIcon() : null, ` ${d.device} — ${m?.friendly_name || d.module}`));
      for (const c of m?.device_specific?.config || []) {
        if (c.hidden) continue;
        const v = d.config?.[c.label] ?? c.default;
        const input = c.type === 'list' ? el('select', { onchange: (e) => ((d.config ??= {})[c.label] = e.target.value) }, c.list_values.map((lv, k) => el('option', { value: lv, selected: String(v) === String(lv) }, c.list_labels?.[k] ?? lv)))
          : el('input', { type: 'text', inputmode: c.type === 'bt_address' ? 'text' : 'decimal', value: v ?? '', onchange: (e) => ((d.config ??= {})[c.label] = c.type === 'int' || c.type === 'float' ? Number(e.target.value) : e.target.value.trim()) });
        fs.append(el('div', { class: 'field inline' }, el('div', {}, el('label', {}, c.friendly_name), el('div', { class: 'help' }, c.description)), input));
      }
      fs.append(el('button', { class: 'btn sm ghost', type: 'button', onclick: async () => { if (await confirmDialog('Remove device?', `${d.device} and its probes`, 'Remove', true)) { map.probe_devices.splice(i, 1); map.probe_info = map.probe_info.filter((p) => p.device !== d.device); renderAll(); } } }, 'Remove device'));
      hwCard.append(fs);
    });
    const wired = Object.entries(mods).filter(([, m]) => !wirelessMod(m));
    const sel = el('select', {}, wired.map(([id, m]) => el('option', { value: id }, m.friendly_name)));
    hwCard.append(el('div', { class: 'row', style: 'margin-top:8px' }, sel, el('button', { class: 'btn sm', type: 'button', onclick: () => {
      const id = sel.value, m = mods[id];
      const cfg = {}; for (const c of m.device_specific?.config || []) cfg[c.label] = c.default;
      map.probe_devices.push({ device: `${id.toUpperCase()}_${map.probe_devices.length + 1}`, module: m.filename, ports: m.device_specific?.ports || [], config: cfg });
      renderAll();
    } }, 'Add device')),
    el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'button', onclick: save }, 'Save hardware')));
  };

  // ---- profiles + tuner
  const profCard = el('div', { class: 'card' });
  const profs = structuredClone(profiles);
  const num = (v, f) => el('input', { type: 'text', inputmode: 'decimal', value: v, style: 'width:140px;font-family:ui-monospace,monospace', onchange: (e) => f(Number(e.target.value)) });
  const renderProfiles = () => {
    profCard.innerHTML = '';
    profCard.append(el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Each wired probe converts its resistance to temperature with the Steinhart–Hart equation 1/T = A + B·ln(R) + C·ln(R)³, using the divider resistor of its ADC port (Probe hardware above).'));
    for (const pr of Object.values(profs)) {
      profCard.append(el('details', { class: 'field' }, el('summary', {}, pr.name),
        el('div', { class: 'field inline' }, el('label', {}, 'Name'), el('input', { type: 'text', value: pr.name, onchange: (e) => (pr.name = e.target.value) })),
        el('div', { class: 'field inline' }, el('label', {}, 'A'), num(pr.A, (v) => (pr.A = v))),
        el('div', { class: 'field inline' }, el('label', {}, 'B'), num(pr.B, (v) => (pr.B = v))),
        el('div', { class: 'field inline' }, el('label', {}, 'C'), num(pr.C, (v) => (pr.C = v)))));
    }
    profCard.append(el('div', { class: 'form-actions' },
      el('button', { class: 'btn', type: 'button', onclick: tuner }, 'Tune from 3 readings'),
      el('button', { class: 'btn primary', type: 'button', onclick: async () => { try { await patchSettings('probe_settings', { probe_profiles: profs }); toast('Profiles saved'); } catch (e) { toast(e.message, true); } } }, 'Save profiles')));
  };
  async function tuner() {
    const pts = [{ temp: '', ohms: '' }, { temp: '', ohms: '' }, { temp: '', ohms: '' }];
    const result = await dialog((close) => {
      const sel = el('select', {}, (PF.status?.probes || []).filter((p) => p.ohms > 0).map((p) => el('option', { value: p.label }, `${p.name} · ${p.ohms} Ω`)));
      const rows = pts.map((pt, i) => {
        const ohms = el('input', { type: 'text', inputmode: 'decimal', placeholder: 'Ω', style: 'width:110px', onchange: (e) => (pt.ohms = Number(e.target.value)) });
        return el('div', { class: 'row', style: 'margin:6px 0' },
          el('input', { type: 'text', inputmode: 'decimal', placeholder: `Temp ${i + 1} ${degUnit()}`, style: 'width:110px', onchange: (e) => (pt.temp = Number(e.target.value)) }), ohms,
          el('button', { class: 'btn sm', type: 'button', onclick: () => { const p = (PF.status?.probes || []).find((x) => x.label === sel.value); if (p) { ohms.value = p.ohms; pt.ohms = p.ohms; } } }, 'Capture'));
      });
      const name = el('input', { type: 'text', placeholder: 'Profile name', value: 'My probe' });
      return el('div', {}, el('h3', {}, 'Probe tuner'),
        el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Put the probe at three known temperatures (ice water, boiling water, a reference thermometer), enter each temperature and press Capture to take the live resistance.'),
        el('div', { class: 'field' }, el('label', {}, 'Probe'), sel), ...rows,
        el('div', { class: 'field' }, el('label', {}, 'New profile name'), name),
        el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(null) }, 'Cancel'), el('button', { class: 'btn primary', type: 'button', onclick: () => close({ name: name.value.trim() || 'My probe', points: pts }) }, 'Solve')));
    });
    if (!result) return;
    try {
      const r = await api('/probes/tune', { body: { points: result.points } });
      const id = result.name.replace(/[^A-Za-z0-9-]/g, '') || 'custom';
      profs[id] = { id, name: result.name, A: r.A, B: r.B, C: r.C };
      renderProfiles();
      toast(`Solved: check temps ${r.check.join(' / ')}${degUnit()} — press Save profiles to keep it`);
    } catch (e) { toast(e.message, true); }
  }

  const renderAll = () => { renderTable(); renderHw(); };
  renderAll(); renderProfiles();
  view.append(
    el('div', { class: 'row between' }, el('h2', {}, 'Probes'), el('button', { class: 'btn sm primary', type: 'button', onclick: addProbe }, '+ Add probe')),
    el('div', { class: 'card tight' }, table),
    el('h2', {}, 'Probe hardware'), hwCard,
    el('h2', {}, 'Probe profiles'), profCard);
}
