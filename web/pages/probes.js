import { PF, el, api, cmd, patchSettings, toast, confirmDialog, dialog, pushScreen, degUnit, segmented, actionBtn, itemRow, iconBtn, addRow, iconField, listGroup, screenActions, actionBar } from '../app.js';
import { targetDialog, limitsDialog, stepsDialog } from './cook.js';
import { icon as lucide, MODE_ICON } from '../icons.js';

// One place for everything probe-related: the probe table (tap a row to edit), adding probes to free
// ADC ports or pairing Bluetooth probes, the ADC/RTD hardware, and the Steinhart-Hart profiles + tuner.

export const btIcon = () => lucide('bluetooth', 'ic bt');
/* A bare "42%" beside a probe name reads as anything: signal, doneness, duty. The outline fills in
   proportion so the level is legible before the number is, and it turns red when it is nearly out. */
export const battIcon = (pct) => {
  const p = Math.max(0, Math.min(100, Math.round(pct)));
  /* Getting low is amber; about to die is red. Red from a fifth remaining meant most of a cook
     spent claiming a fault that was not there. */
  const cls = `batt ${p <= 10 ? 'crit' : p <= 20 ? 'low' : ''}`;
  return el('span', { class: cls, title: `Battery ${p}%`, 'aria-label': `battery ${p} percent` },
    el('span', { class: 'batt-body' }, el('i', { style: `width:${p}%` })),
    el('span', { class: 'batt-pct' }, `${p}%`));
};
const WIRELESS_MODULES = ['ibbq', 'meater', 'chefiq'];
/** 0..4 bars from an RSSI in dBm (same thresholds as the daemon) */
export const barsFromRssi = (rssi) => (!rssi ? 0 : rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1);
/** signal-strength bars (0..4 lit) */
export const sigBars = (bars, title) => el('span', { class: `sig s${bars}`, title: title || `${bars} / 4`, 'aria-label': `signal ${bars} of 4` }, [1, 2, 3, 4].map((i) => el('i', { class: i <= bars ? 'on' : '' })));
export const fmtEta = (s) => { s = Math.max(0, Math.round(s)); const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60); return h ? `${h}h ${m}m` : m > 0 ? `${m} min` : '< 1 min'; };
/** true when the named probe device is a Bluetooth probe (by module) */
export function isWireless(deviceName) {
  const d = (PF.settings?.probe_settings?.probe_map?.probe_devices || []).find((x) => x.device === deviceName);
  return !!d && WIRELESS_MODULES.includes(d.module);
}

/* Two halves of one subject, and the split is what you are doing, not which probe it is about.
 *
 *   setup (Settings -> Probes): connect and pair, name, say what each is for, assign its curve.
 *   cooking (the Probes tab):   what everything reads, targets, step alerts and alarms.
 *
 * They share the probe list and the editor because they are the same objects; what differs is what
 * each row says and what it offers. */
export async function renderProbes(view, opts = {}) {
  const setup = !!opts.setup;
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
    // a Bluetooth device without readings is an unpaired probe: never keep it (it would hide that address from pairing)
    map.probe_devices = map.probe_devices.filter((d) => !WIRELESS_MODULES.includes(d.module) || map.probe_info.some((p) => p.device === d.device));
    const labels = new Set();
    for (const p of map.probe_info) { p.label = p.name.replace(/[^A-Za-z0-9]/g, '') || 'Probe'; if (labels.has(p.label)) { toast(`Duplicate probe name ${p.name}`, true); return false; } labels.add(p.label); }
    try { await patchSettings('probe_settings', { probe_map: { probe_devices: map.probe_devices, probe_info: map.probe_info } }); toast('Probes saved'); renderAll(); return true; }
    catch (e) { toast(e.message, true); return false; }
  };
  const freePorts = (except) => map.probe_devices.flatMap((d) => (d.ports || []).filter((port) => !map.probe_info.some((p) => p !== except && p.device === d.device && p.port === port)).map((port) => ({ device: d.device, port, wireless: WIRELESS_MODULES.includes(d.module) })));

  // ---- editor
  /* One column, grouped into what the probe IS, what it is WIRED TO, and where it SHOWS -- the
     order you would think about it in. Three choices get a segmented control rather than a dropdown
     you have to open to see the options. A field that only applies to one type appears only for
     that type. The destructive action is a row of its own, in red, away from the pair that commits.
     It was a stack of ten unlabelled rows with Remove, Cancel and Save crammed on one line. */
  const editProbe = (p, isNew = false) => pushScreen((close) => {
    const draft = { ...p };
    const live = () => PF.status?.probes?.find((x) => x.label === p.label);
    const f = (label, input, help) => el('div', { class: 'field inline' },
      el('div', {}, el('label', {}, label), help ? el('div', { class: 'help' }, help) : null), input);
    const tog = (label, key, help) => el('label', { class: 'toggle' },
      el('div', {}, label, help ? el('div', { class: 'help' }, help) : null),
      el('span', { class: 'switch' }, el('input', { type: 'checkbox', checked: !!draft[key], onchange: (e) => (draft[key] = e.target.checked) }), el('span')));

    const ports = [...freePorts(p), ...(p.device ? [{ device: p.device, port: p.port, wireless: isWireless(p.device) }] : [])];
    const portSel = el('select', { onchange: (e) => { [draft.device, draft.port] = e.target.value.split('|'); } },
      ports.map((o) => el('option', { value: `${o.device}|${o.port}`, selected: o.device === p.device && o.port === p.port }, `${o.device} \u00b7 ${o.port}`)));
    const profSel = el('select', { onchange: (e) => (draft.profile = e.target.value) },
      Object.values(profiles).map((pr) => el('option', { value: pr.id, selected: (draft.profile?.id || draft.profile) === pr.id }, pr.name)));

    /* Only an Aux probe can be the ambient reference, so the switch is only there when the type says
       so -- rather than sitting greyed out, or on, under a note explaining when it counts. */
    const auxOnly = el('div');
    const paintAux = () => { auxOnly.innerHTML = ''; if (draft.type === 'Aux') auxOnly.append(tog('Ambient reference', 'ambient', 'Used by learning and cold start')); };
    const typeSeg = segmented([['Primary', 'Grill'], ['Food', 'Food'], ['Aux', 'Aux']], draft.type || 'Food', (v) => { draft.type = v; paintAux(); });
    paintAux();

    const wireless = isWireless(p.device);
    const l = live();
    return el('div', { class: 'sheet' },
      el('div', { class: 'sheet-head' },
        el('div', {}, el('h3', {}, isNew ? 'New Probe' : p.name),
          el('div', { class: 'help' }, wireless ? 'Bluetooth' : 'Wired')),
        isNew || !l?.valid ? null : el('div', { class: 'sheet-now' }, `${l.temp}${degUnit()}`)),

      el('div', { class: 'sheet-body' },
        el('h2', {}, 'Identity'),
        f('Name', iconField('thermometer', el('input', { type: 'text', value: draft.name, onchange: (e) => (draft.name = e.target.value.trim()) }))),
        f('Type', typeSeg),

        el('h2', {}, 'Connection'),
        f('Port', portSel),
        wireless ? null : f('Profile', profSel, 'Converts resistance to temperature'),

        el('h2', {}, 'Visibility'),
        tog('Enabled', 'enabled'),
        tog('Show on Home', 'show_on_home', 'Home screen and the grill display'),
        auxOnly,

        ),
      /* Remove on the left as a mark, Cancel then Save on the right, pinned to the top edge of
         the tab bar with the Home button riding over the gap between them: see
         docs/design-language.md. */
      screenActions({
        onDelete: isNew ? null : async () => {
          close(undefined);
          if (wireless) {
            // one physical probe = one device with its meat and ambient sensors: they go together
            const sibs = map.probe_info.filter((x) => x.device === p.device);
            if (await confirmDialog('Unpair probe?', `${sibs.map((x) => x.name).join(' and ')}`, 'Unpair', true)) {
              map.probe_info = map.probe_info.filter((x) => x.device !== p.device);
              const di = map.probe_devices.findIndex((d) => d.device === p.device); if (di >= 0) map.probe_devices.splice(di, 1);
              await save();
            }
          } else if (await confirmDialog('Remove probe?', p.name, 'Remove', true)) {
            const i = map.probe_info.indexOf(p); if (i >= 0) map.probe_info.splice(i, 1); await save();
          }
        },
        deleteTitle: wireless ? 'Unpair' : 'Remove',
        onCancel: () => close(undefined),
        onSave: () => {
          if (!draft.name) { toast('Name required', true); return; }
          Object.assign(p, draft); close('saved');
        },
      }));
  }, { title: isNew ? 'New Probe' : p.name, back: 'Probes' }).then(async (r) => { if (r) await save(); });

  // ---- add: free wired port, or pair a Bluetooth probe
  /* The way out when the classifier does not know a probe: say what it is, then scan for that. */
  const pickBrand = () => dialog((close) => {
    const btMods = Object.entries(mods).filter(([, m]) => wirelessMod(m));
    return el('div', {}, el('h3', {}, 'Which make?'),
      el('div', { class: 'opts' }, ...btMods.map(([, m]) => el('button', { class: 'btn', type: 'button', onclick: () => { close(); pairBluetooth(m.filename); } }, btIcon(), ` ${m.friendly_name}`))),
      el('button', { class: 'btn ghost block', type: 'button', style: 'margin-top:10px', onclick: () => close() }, 'Cancel'));
  });

  const addProbe = () => dialog((close) => {
    const free = freePorts(null).filter((o) => !o.wireless);
    return el('div', {}, el('h3', {}, 'Add probe'),
      el('div', { class: 'help' }, free.length ? 'Free wired ports' : 'No free wired ports \u2014 add a device under Settings \u2192 Grill Hardware \u2192 Probe Hardware'),
      el('div', { class: 'opts' }, ...free.map((o) => el('button', { class: 'btn', type: 'button', onclick: () => { close(); const n = map.probe_info.length + 1; const p = { type: 'Food', label: `Probe${n}`, name: `Probe ${n}`, profile: 'TWPS00', device: o.device, port: o.port, enabled: true, show_on_home: true }; map.probe_info.push(p); editProbe(p, true).then(() => { if (!map.probe_info.includes(p)) return; }); } }, `${o.device} · ${o.port}`))),
      el('div', { class: 'help' }, 'Bluetooth'),
      el('div', { class: 'opts' }, el('button', { class: 'btn', type: 'button', onclick: () => { close(); pairBluetooth(); } }, btIcon(), ' Scan and Pair')),
      el('button', { class: 'btn ghost block', type: 'button', style: 'margin-top:10px', onclick: () => close() }, 'Cancel'));
  });
  /* Pair what the scan found, rather than asking which brand it is first.
   *
   * The daemon already recognises a Chef iQ, a Meater and an iBBQ from the manufacturer id, the
   * service UUIDs and the name -- `kind` comes back on every device it sees. Asking for the brand
   * and then scanning filtered by it made the user answer a question the grill had already
   * answered, and answer it wrong if they guessed: pick Meater, see nothing, conclude the probe is
   * broken. So: scan once, list what is recognisably a probe with its make beside it, and take the
   * module from what the scan said it was. Picking the brand by hand is still there, underneath,
   * for a probe the classifier does not know. */
  const pairBluetooth = async (only) => {
    const kindName = (k) => {
      const e = Object.entries(mods).find(([, mm]) => mm.filename === k);
      return e ? e[1].friendly_name : k;
    };
    const picked = await dialog((close) => {
      const list = el('div', { class: 'opts' }, el('div', { class: 'muted' }, 'Scanning for 8 s\u2026 make sure the probe is on and nearby.'));
      const row = (f) => el('button', { class: 'btn', type: 'button', onclick: () => close(f) },
        el('div', { class: 'row between', style: 'width:100%' },
          el('span', {}, btIcon(), ' ', f.name || 'Unknown device'),
          el('span', { class: 'help row', style: 'gap:6px' },
            f.kind ? el('span', { class: 'pill sm' }, kindName(f.kind)) : null,
            f.rssi ? sigBars(barsFromRssi(f.rssi), `${f.rssi} dBm`) : null)));
      api('/probes/ble/scan?seconds=8', { body: {} }).then((found) => {
        list.innerHTML = '';
        found.sort((a, b) => (b.rssi || -999) - (a.rssi || -999));
        // already-paired addresses (any Bluetooth device's bt_address config) are left out of the list
        const paired = new Set(map.probe_devices.flatMap((d) => Object.values(d.config || {})).filter((v) => typeof v === 'string' && /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i.test(v)).map((v) => v.toUpperCase()));
        const seen = found.filter((f) => !paired.has((f.address || '').toUpperCase()));
        const known = new Set(Object.values(mods).filter(wirelessMod).map((mm) => mm.filename));
        const probes = seen.filter((f) => known.has(f.kind) && (!only || f.kind === only));
        const rest = seen.filter((f) => !probes.includes(f) && f.kind !== 'chefiq-hub');
        if (!probes.length) list.append(el('div', { class: 'muted', style: 'margin-bottom:8px' },
          'No unpaired probe seen. A probe only broadcasts while it is out of its charger and awake (Chef iQ: take it out of the dock, wait a few seconds), then scan again.'));
        for (const f of probes) list.append(row(f));
        /* Anything else nearby is a phone or a watch. It is listed so the page does not look blind,
           but it cannot be paired from here: nothing says what it is, so nothing could read it. */
        if (rest.length) {
          const more = el('div', { class: 'opts', hidden: true }, ...rest.map((f) => el('div', { class: 'row between help', style: 'padding:6px 2px' },
            el('span', {}, f.name || f.address), el('span', {}, 'not a known probe'))));
          list.append(el('button', { class: 'btn ghost sm', type: 'button', onclick: (e) => { more.hidden = !more.hidden; e.target.textContent = more.hidden ? `Show ${rest.length} other device${rest.length === 1 ? '' : 's'} nearby` : 'Hide other devices'; } }, `Show ${rest.length} other device${rest.length === 1 ? '' : 's'} nearby`), more);
        }
        list.append(el('button', { class: 'btn sm', type: 'button', onclick: () => { close(undefined); setTimeout(() => pairBluetooth(only), 50); } }, 'Scan again'));
        if (!only) list.append(el('button', { class: 'btn ghost sm', type: 'button', onclick: () => { close(undefined); setTimeout(pickBrand, 50); } }, 'Choose the make myself'));
      }).catch((e) => { list.innerHTML = ''; list.append(el('div', { class: 'muted' }, e.message)); });
      return el('div', {}, el('h3', {}, 'Pair a Probe'), list, el('button', { class: 'btn ghost block', type: 'button', style: 'margin-top:10px', onclick: () => close(undefined) }, 'Cancel'));
    });
    if (!picked) return;
    const addr = picked.address;
    const entry = Object.entries(mods).find(([, mm]) => mm.filename === picked.kind);
    if (!entry) { toast('That device is not a probe this grill can read', true); return; }
    const m = entry[1];
    if (!addr) return;
    // devices: ChefiQ1, ChefiQ2 ... (first free number); probes: BT1, BT2 ... counted across every
    // Bluetooth device, with the ambient sensor as "BTn Ambient" - hidden on Home, shown inside BTn's card
    const base = m.friendly_name.replace(/^BT\s+/i, '').replace(/\s*\(.*\)\s*$/, '').trim().replace(/[^A-Za-z0-9]/g, '');
    let n = 1; while (map.probe_devices.some((d) => d.device === `${base}${n}`)) n++;
    const cfg = {}; for (const c of m.device_specific?.config || []) cfg[c.label] = c.type === 'bt_address' ? addr : c.default;
    cfg.transient = true;
    const dev = { device: `${base}${n}`, module: m.filename, ports: m.device_specific?.ports || [], config: cfg };
    map.probe_devices.push(dev);
    const taken = new Set(map.probe_info.map((p) => p.name.toLowerCase()));
    let k = 1; const nextBt = () => { while (taken.has(`bt${k}`)) k++; taken.add(`bt${k}`); return `BT${k}`; };
    let lastMeat = null;
    dev.ports.forEach((port, i) => {
      const ambient = /ambient/i.test(port);
      const name = ambient ? `${lastMeat || nextBt()} Ambient` : (lastMeat = nextBt());
      /* The ambient sensor inside a food probe reads air, so it is Aux from the moment it is
         added, and never appears in a list of things that can be brought to a temperature. */
      map.probe_info.push({ type: ambient ? 'Aux' : 'Food', label: `${dev.device}${i + 1}`, name, profile: 'TWPS00', device: dev.device, port, enabled: true, show_on_home: !ambient && i < 3 });
    });
    await save();
  };

  // ---- the probes themselves, one list per role
  const table = el('div', {});
  /* The step you are waiting for is what you want off the row; once they are all done the target
     is again the only thing left to say. */
  const nextStep = (live) => {
    const st = (live.steps || []).find((x) => !x.done);
    return st ? `${st.name} at ${st.temp}${degUnit()}` : null;
  };
  const renderTable = () => {
    table.innerHTML = '';
    /* Grouped the way the probes are used, and named for the type that puts them there: the pit,
       the food, then everything measuring air rather than meat. Each group is a labelled list of
       its own -- the same shape Settings uses -- rather than a heading row inside one long table,
       where the three groups ran together and you could not see where one ended. */
    const GROUPS = [['Primary', 'Grill'], ['Food', 'Food'], ['Aux', 'Aux & Ambient']];
    /* A disabled probe is not part of the cook, so it is not in the way of one. It is still here
       when you go looking for it -- that is what the count at the foot is for -- but a page about
       what the grill is measuring should not be padded out with what it is not. */
    /* One rule for what is on this page: the tick in Filter. A probe switched off in Settings reads
       nothing, so it is shown as Off rather than hidden behind a second button -- there is one
       reason a probe is missing from this page and one place to change it. */
    const hidden = map.probe_info.filter((p) => p.show_on_home === false);
    const shown = setup ? map.probe_info : map.probe_info.filter((p) => p.show_on_home !== false);
    for (const [role, heading] of GROUPS) {
      const members = shown.filter((p) => (p.type || 'Food') === role);
      if (!members.length) continue;
      const list = el('div', { class: 'ios-list' });
      table.append(el('h2', {}, heading), list);
      for (const p of members) renderRow(list, p);
    }
    if (!map.probe_info.length) table.append(el('p', { class: 'help' }, 'No probes yet.'));
    else if (!shown.length) table.append(el('p', { class: 'help' }, 'Nothing is ticked in Filter.'));
    if (!setup && hidden.length) table.append(el('p', { class: 'help' }, `${hidden.length} hidden \u00b7 Filter`));

    /* One row per probe, and everything you do to a probe is on it: the reading, its target and its
       alarms, and its settings behind the chevron. The targets used to be cards on the Cook page
       while the settings were a list here, so one probe appeared in two places and neither showed
       the whole of it. */
    function renderRow(list, p) {
      const live = PF.status?.probes?.find((x) => x.label === p.label);
      const wireless = isWireless(p.device);
      const tgt = live?.target > 0;
      if (setup) {
        list.append(itemRow({
          icon: wireless ? 'bluetooth' : 'thermometer', color: wireless ? '#0a84ff' : '#ff453a',
          title: p.name,
          meta: wireless ? `${p.device} \u00b7 ${p.port}` : `${p.device} \u00b7 ${p.port} \u00b7 ${profName(p)}`,
          badge: p.enabled === false ? 'Off' : null,
          onclick: () => editProbe(p),
        }));
        return;
      }
      list.append(el('div', { class: `prow ${p.enabled ? '' : 'off'}` },
        el('button', { class: 'row prow-main', type: 'button', onclick: () => editProbe(p) },
          el('span', { class: 'body' },
            el('span', { class: 't' },
              wireless ? btIcon() : null,
              wireless && live ? sigBars(live.signal || 0, live.rssi ? `${live.rssi} dBm` : 'no link') : null,
              wireless && live?.battery >= 0 ? battIcon(live.battery) : null,
              p.name),
            /* What the probe is doing, not how it is wired. The port and the profile were here, and
             they are set once when the grill is built and then never looked at again; what changes
             during a cook is whether it is reading, what the target is and how long is left. The
             wiring is inside, where it is wanted about once. */
          el('span', { class: 's' }, !p.enabled ? 'Disabled'
              : p.type === 'Primary'
                ? (PF.status?.mode === 'Hold' ? `Holding ${PF.status.setpoint}${degUnit()}` : live?.valid ? `${PF.status?.mode || 'Reading'}` : 'No reading')
              : tgt ? `Target ${live.target}${degUnit()}${live.eta_s > 0 && live.temp < live.target ? ` \u00b7 ${fmtEta(live.eta_s)} left` : live.temp >= live.target ? ' \u00b7 reached' : ''}`
              : live?.valid ? nextStep(live) || 'Reading' : 'No reading'),
            /* A wired probe's curve, named and labelled, under the live line rather than instead
               of it: it is worth being able to see at a glance which probe is on which profile
               without opening each one. A Bluetooth probe reports degrees and has none. */
            wireless ? null : el('span', { class: 's' }, `Profile: ${profName(p)}`)),
          el('span', { class: 'v' }, p.enabled ? (live?.valid ? `${live.temp}${degUnit()}` : '\u2014') : 'off'),
          lucide('chevron-right', 'ic chev')),
        /* The pit probe has neither of these. Its target is the set point, which is what Hold mode
           is for, and a second place to type one would be a second answer to the same question.
           Its alarms are conditional notifications -- "Grill Stalled Hot", "Grill Stalled Cold" --
           which compare it with the set point and so keep working when the set point changes,
           where a fixed limit typed once would not. */
        p.type === 'Primary' ? null : el('div', { class: 'prow-acts' },
          actionBtn('target', tgt ? 'Change Target' : 'Set Target', { size: 'xs', onclick: async () => {
            const r = await targetDialog({ ...p, ...live }); if (r) cmd({ cmd: 'target', label: p.label, ...r });
          } }, MODE_ICON.Hold),
          actionBtn('steps', 'Steps', { size: 'xs', class: 'ghost', onclick: () => stepsDialog({ ...p, ...live }) }, 'flag'))));
    }
  };

  /* Which of the working probes are part of this cook. It is not the same question as whether a
     probe is switched on, so it is not the same control: this one only decides what is in the way
     while you are cooking, and lives where you are cooking rather than in Settings. */
  /* One filter, one idea: which probes this page shows.
   *
   * There were two controls for it -- "Show or Hide Probes", which listed only the switched-on ones,
   * and "Show N disabled", a separate escape hatch at the foot of the list -- and between them a
   * probe could be invisible for two different reasons with two different cures. Now every probe is
   * in one list with one tick, and a disabled one is shown as disabled rather than hidden behind a
   * second button: switching a probe off entirely is a hardware matter and stays in Settings.
   *
   * It is a screen and not a box because it is a list of everything you own, which is bigger than a
   * question. */
  const filterProbes = () => pushScreen((close) => {
    const draft = new Map(map.probe_info.map((p) => [p.label, p.show_on_home !== false]));
    const inner = el('div', {});   /* headings sit OUTSIDE the lists they label, as they do everywhere */
    const GROUPS = [['Primary', 'Grill'], ['Food', 'Food'], ['Aux', 'Aux & Ambient']];
    for (const [role, heading] of GROUPS) {
      const members = map.probe_info.filter((p) => (p.type || 'Food') === role);
      if (!members.length) continue;
      inner.append(el('h2', {}, heading));
      const grp = el('div', { class: 'ios-list' });
      for (const p of members) {
        grp.append(el('label', { class: 'toggle' },
          el('div', {}, p.name,
            el('div', { class: 'help' }, [p.device, p.enabled === false ? 'switched off in Settings' : null].filter(Boolean).join(' \u00b7 '))),
          el('span', { class: 'switch' }, el('input', {
            type: 'checkbox', checked: draft.get(p.label),
            onchange: (e) => draft.set(p.label, e.target.checked),
          }), el('span'))));
      }
      inner.append(grp);
    }
    if (!map.probe_info.length) inner.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'No probes yet.'));
    return el('div', { class: 'sheet' },
      el('div', { class: 'sheet-head' }, el('div', {},
        el('h3', {}, 'Show on This Page'),
        el('div', { class: 'help' }, 'Hidden probes keep reading and keep feeding the notifications'))),
      el('div', { class: 'sheet-body' }, inner),
      screenActions({
        onCancel: () => close(undefined),
        onSave: () => { for (const p of map.probe_info) p.show_on_home = draft.get(p.label) !== false; close('save'); },
      }));
  }, { title: 'Show Probes', back: 'Probes' }).then(async (r) => { if (r === 'save') await save(); renderTable(); });

  const renderAll = () => { renderTable(); };
  renderAll();
  /* `append` here is the DOM's, not el()'s, so a null child is written out as the word "null" --
     which is exactly what appeared under the probe list. Filter before appending. */
  view.append(...[
    el('h2', {}, 'Probes'),
    setup ? el('p', { class: 'help' }, 'Connect a probe, name it, say what it is for and which profile converts it.') : null,
    /* Adding one goes at the TOP of the section. At the foot it sits below every probe and their
       buttons, which on a phone is a screen and a half of scrolling to reach the one thing you came
       to this page to do when you have a new probe in your hand. */
    setup ? addRow('Connect a Probe', addProbe) : null,
    table,
    setup ? listGroup('Profiles', [
      { href: '#/settings/probeprofiles', icon: 'activity', color: '#ff9f0a', title: 'Probe Profiles', sub: 'Curves you assign to probes, and the 3-point tuner' },
    ]) : null,
    /* The two things this page does, on the bar: add one on the left, choose what you are looking
       at on the right, and the Home button riding over the gap between them. Adding a probe is why
       you came here with one in your hand, so it takes the primary colour; the filter only changes
       what you are looking at, so it does not. See docs/design-language.md. */
    setup ? null : actionBar(
      [actionBtn('add', 'Add Probe', { size: '', class: 'primary', onclick: () => pairBluetooth() })],
      [actionBtn('filter', 'Filter', { size: '', onclick: filterProbes })]),
  ].filter(Boolean));
}

/* Probe profiles have a settings page of their own.
 *
 * They were under the probes, and they are not something you do while cooking: a profile is the
 * curve that turns a resistance into a temperature, chosen once per kind of probe. Keeping them
 * here leaves the Probes tab about the probes -- what they read, what they are aiming at -- and
 * puts the curve where the rest of the once-only setup lives. */
export async function renderProbeProfiles(view) {
  const profiles = PF.settings.probe_settings.probe_profiles;
  const map = structuredClone(PF.settings.probe_settings.probe_map);
  const profCard = el('div', {});
  const profs = structuredClone(profiles);
  const num = (v, f) => el('input', { class: 'mono', type: 'text', inputmode: 'decimal', value: v, onchange: (e) => f(Number(e.target.value)) });
  const renderProfiles = () => {
    profCard.innerHTML = '';
    /* A table of what you own, and it becomes cards on a phone: every value keeps its column name,
       so "0.000247" is still labelled A at 402 px. Opening a row gives its fields over a footer
       that saves or deletes that profile alone. */
    const used = (id) => map.probe_info.filter((p) => (p.profile?.id ?? p.profile) === id).length;
    const entries = Object.values(profs);
    /* Saved things, listed like saved cards: what it is called, who depends on it, and the bin at
       the end for the ones nothing is using. The coefficients are inside. */
    const inner = el('div', { class: 'ios-list' });
    for (const pr of entries) {
      const n = used(pr.id);
      inner.append(itemRow({
        icon: 'thermometer', color: n ? '#ff453a' : '#8e8e93',
        title: pr.name,
        meta: n ? `${n} probe${n === 1 ? '' : 's'}` : 'Not in use',
        badge: null,
        onclick: () => editProfile(pr, n),
        actions: n ? [] : [iconBtn('trash-2', 'Delete', { class: 'danger', onclick: async (e) => {
          e.stopPropagation();
          if (!await confirmDialog('Delete profile?', pr.name, 'Delete', true)) return;
          delete profs[pr.id]; await saveProfiles(); renderProfiles();
        } })],
      }));
    }
    if (!entries.length) inner.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'No profiles.'));
    profCard.replaceChildren(addRow('Tune a New Probe', tuner), inner);
  };
  const editProfile = (pr, n) => pushScreen((close) => el('div', { class: 'sheet' },
    el('div', { class: 'sheet-head' },
      el('div', {}, el('h3', {}, pr.name), el('div', { class: 'help' }, `${n} probe${n === 1 ? '' : 's'} using this`))),
    el('div', { class: 'sheet-body' },
      el('h2', {}, 'Profile'),
      el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Name')), el('input', { type: 'text', value: pr.name, onchange: (e) => (pr.name = e.target.value) })),
      el('h2', {}, 'Steinhart\u2013Hart coefficients'),
      el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'A')), num(pr.A, (v) => (pr.A = v))),
      el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'B')), num(pr.B, (v) => (pr.B = v))),
      el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'C')), num(pr.C, (v) => (pr.C = v))),
      n ? null : el('div', { class: 'form-actions' },
        actionBtn('delete', 'Delete Profile', { onclick: async () => {
          if (!await confirmDialog('Delete profile?', pr.name, 'Delete', true)) return;
          delete profs[pr.id]; close(); await saveProfiles(); renderProfiles();
        } }))),
    el('div', { class: 'form-actions' },
      actionBtn('cancel', 'Cancel', { size: '', onclick: () => close(undefined) }),
      actionBtn('save', 'Save', { size: '', onclick: () => close('save') }))),
    { title: pr.name, back: 'Profiles' })
    .then(async (r) => { if (r === 'save') { await saveProfiles(); renderProfiles(); } });

  const saveProfiles = async () => {
    try { await patchSettings('probe_settings', { probe_profiles: profs }); toast('Saved'); }
    catch (e) { toast(e.message, true); }
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
        el('p', { class: 'help' }, 'Three known temperatures — ice water, boiling, a reference. Enter each and Capture the live resistance.'),
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
      toast(`Solved: check temps ${r.check.join(' / ')}${degUnit()} \u2014 open the profile and Save to keep it`);
    } catch (e) { toast(e.message, true); }
  }

  renderProfiles();
  view.append(
    el('h2', {}, 'Probe Profiles'),
    el('p', { class: 'help' }, 'Steinhart\u2013Hart: 1/T = A + B\u00b7ln(R) + C\u00b7ln(R)\u00b3. Assign one to a probe from its row on the Probes tab.'),
    profCard);
}
