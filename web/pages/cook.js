import { PF, el, api, cmd, onStatus, fmtTemp, degUnit, fmtDur, dialog, pushScreen, numberDialog, toast, confirmDialog, segmented, actionBtn, itemRow, iconBtn, actionBar, patchSettings, screenActions } from '../app.js';
import { fmtEta, btIcon, sigBars, battIcon } from './probes.js';
import { icon as lucide, MODE_ICON } from '../icons.js';
/* The same condition cards, rows and picker the notification editor is made of. A step ending is
   the same kind of question -- "when is this true" -- and has to be asked in the same shapes.
   See web/conditions.js and docs/design-language.md. */
import { catalogue, condNode, describeNode, condIcon, OP_SYM, fmtSecs } from '../conditions.js';
import { pickFoodProbes } from './probes.js';

/* Doneness presets, in °F and converted for °C users.
 *
 * `carry` is how far the centre keeps climbing after the meat comes off the heat, so the number to
 * cook to is the target minus the carryover. That is the number the probe alarm is set to, because
 * an alert that fires when the meat is already done is an alert that arrives too late. Carryover
 * grows with thickness, so these are the usual figures for a piece you would cook whole: a steak a
 * few degrees, a whole bird more. Cuts taken to tenderness rather than to a temperature, like
 * brisket and ribs, carry nothing worth naming, because you are pulling them when they feel right
 * and then resting them for an hour anyway. */
/* `to` is the doneness -- the temperature the meat is at when it is ready, shown large on each row.
 * `carry` is how many degrees early it comes off the heat: five for beef, a few for pork and lamb,
 * none for poultry, sausage, fish, brisket and ribs, which have to reach the number itself (and a
 * brisket pulled at 200 does not reliably climb to 205). `coast` is how far a pull at the number
 * still climbs, said but not relied on: a 155 breast comes off at 155 and coasts to about 160.
 * `steps` are the things to do on the way, in degrees F, and replace the probe's step alerts. */
const PRESETS = {
  Beef: [
    { name: 'Rare', to: 125, carry: 5, steps: [['Flip', 100]] },
    { name: 'Medium rare', to: 135, carry: 5, steps: [['Flip', 110]] },
    { name: 'Medium', to: 140, carry: 5, steps: [['Flip', 115]] },
    { name: 'Medium well', to: 150, carry: 5, steps: [['Flip', 125]] },
    { name: 'Well done', to: 160, carry: 5, steps: [['Flip', 135]] },
  ],
  Brisket: [{ name: 'Probe tender', to: 203, carry: 0, steps: [['Spritz', 150], ['Wrap', 165]] }],
  Pork: [
    { name: 'Chops and loin', to: 145, carry: 4, steps: [['Flip', 120]] },
    { name: 'Pulled pork', to: 203, carry: 0, steps: [['Spritz', 150], ['Wrap', 165]] },
  ],
  Ribs: [{ name: 'Bend test', to: 195, carry: 0, steps: [['Spritz', 150], ['Wrap', 165], ['Unwrap', 185]] }],
  Chicken: [
    { name: 'Breast 155', to: 155, carry: 0, coast: 5, steps: [['Flip', 135]] },
    { name: 'Breast 165', to: 165, carry: 0, coast: 5, steps: [['Flip', 145]] },
    { name: 'Thighs', to: 175, carry: 0, coast: 5, steps: [['Flip', 155]] },
  ],
  Turkey: [{ name: 'Whole bird', to: 165, carry: 0, coast: 5, steps: [['Baste', 145]] }],
  Fish: [{ name: 'Flaky', to: 145, carry: 0, steps: [] }],
  Lamb: [
    { name: 'Medium rare', to: 135, carry: 4, steps: [['Flip', 110]] },
    { name: 'Medium', to: 140, carry: 4, steps: [['Flip', 115]] },
  ],
  Sausage: [{ name: 'Cooked through', to: 160, carry: 0, steps: [['Flip', 140]] }],
};
const AFTER = [[0, 'Notify only'], [1, 'Keep warm'], [2, 'Shutdown']];
const toUser = (f) => (PF.units === 'C' ? Math.round((f - 32) * 5 / 9) : f);
const deltaUser = (f) => (PF.units === 'C' ? Math.round(f * 5 / 9) : f);

/* The target picker opens on Custom -- a temperature typed, with whatever step alerts it wants, and
 * the presets saved from it -- and a meat is one tap away. Picking a doneness sets the target to the
 * pull temperature, carries the meat, the doneness and what it rests to, and replaces the probe's
 * step alerts with that doneness's own. */
export async function targetDialog(p) {
  return dialog((close) => {
    let after = p.after || 0;
    let meat = p.meat && PRESETS[p.meat] ? p.meat : 'Custom';
    const u = degUnit();

    const now = p.valid ? `${fmtTemp(p.temp)}${u}` : '\u2014';
    const head = el('div', { class: 'sheet-head' },
      el('div', {}, el('h3', {}, p.name), el('div', { class: 'muted' }, `Now ${now}`)),
      p.target > 0 ? el('div', { class: 'sheet-now' }, `${fmtTemp(p.target)}${u}`, el('small', {}, 'set')) : null);

    const chips = el('div', { class: 'chiprow' });
    const list = el('div', { class: 'donelist' });
    /* the note is the plan on the way; the pull is said once, under the large number */
    const summary = (steps, pull) => steps.length ? steps.map(([n, t]) => `${n} ${t}${u}`).join(' \u00b7 ') : `off at ${pull}${u}`;

    /* the probe's step alerts, edited on the Custom tab and saved as they change */
    const cur = (PF.settings?.notify?.probe_steps?.[p.label] || []).map((x) => ({ ...x }));
    const stepsBox = el('div', { class: 'sheet-foot tsteps' });
    const ask = (title, v) => numberDialog(title, v, { min: 32, max: 400, step: 5 });
    const drawSteps = () => {
      stepsBox.innerHTML = '';
      const inner = el('div', { class: 'ios-list' });
      for (const [i, st] of cur.entries()) {
        inner.append(itemRow({
          icon: 'bell', color: '#bf5af2', title: st.name, value: `${st.temp}${u}`, chevron: false,
          onclick: async () => { const v = await ask(st.name, st.temp); if (v != null) { st.temp = v; await saveProbeSteps(p.label, cur); drawSteps(); } },
          actions: [iconBtn('trash-2', `Remove ${st.name}`, { class: 'danger', onclick: async (e) => { e.stopPropagation(); cur.splice(i, 1); await saveProbeSteps(p.label, cur); drawSteps(); } })],
        }));
      }
      const left = STEP_PRESETS.filter(([n]) => !cur.some((x) => x.name === n));
      stepsBox.append(el('label', {}, 'Step alerts'), cur.length ? inner : null,
        cur.length >= 4 ? null : el('div', { class: 'chiprow steps' },
          left.map(([n, t]) => el('button', { class: 'chip-btn', type: 'button', onclick: async () => {
            cur.push({ name: n, temp: toUser(t) }); await saveProbeSteps(p.label, cur); drawSteps();
          } }, `+ ${n}`)),
          el('button', { class: 'chip-btn', type: 'button', onclick: async () => {
            const v = await ask('Alert at', toUser(140)); if (v != null) { cur.push({ name: 'Alert', temp: v }); await saveProbeSteps(p.label, cur); drawSteps(); }
          } }, '+ Custom')));
    };

    /* `restTo` set: the daemon works out when it comes off from how fast it is climbing */
    const pick = async (target, m, done, rest, steps, restTo = 0) => {
      cur.length = 0;
      for (const [n, t] of steps) cur.push({ name: n, temp: t });
      await saveProbeSteps(p.label, cur);
      close(restTo ? { rest: restTo, after, meat: m, done } : { target, after, meat: m, done, finish: rest || 0 });
    };

    /* The one-off target is always first: type a number, add its alerts, Set. Naming it saves it
       to the list below; leaving the name blank uses it once. */
    const showCustom = () => {
      /* Off at: the alert fires at the number. Rest to: the number is what it should read after
         resting, and the daemon calls the take-off from how fast it is climbing, aiming 2 degrees
         over so the rest lands on it. */
      let kind = p.rest > 0 ? 'rest' : 'off';
      const tv = el('input', { type: 'text', inputmode: 'decimal', placeholder: u, 'aria-label': 'Target', value: p.rest > 0 ? String(fmtTemp(p.rest)) : p.target > 0 ? String(fmtTemp(p.target)) : '' });
      const nm = el('input', { type: 'text', placeholder: 'Optional', 'aria-label': 'Name' });
      const read = () => { const v = parseFloat(tv.value); if (Number.isNaN(v) || v <= 0) { tv.focus(); return null; } return v; };
      /* the name labels the probe on its own -- no meat in front of it -- and is only kept in the
         list when Save is pressed; Set Target uses it once */
      const setIt = () => {
        const v = read(); if (v == null) return;
        const name = nm.value.trim();
        close(kind === 'rest' ? { rest: v, after, meat: '', done: name } : { target: v, after, meat: '', done: name, finish: 0 });
      };
      const saveIt = async () => {
        const v = read(); if (v == null) return;
        const name = nm.value.trim();
        if (!name) { nm.focus(); toast('Name it to save it', true); return; }
        const all = customPresets().filter((x) => x.name !== name);
        all.push({ name, target: v, rest: kind === 'rest', steps: cur.map((x) => ({ ...x })) });
        await saveCustomPresets(all);
        toast(`Saved ${name}`);
        showMeat('Custom');
      };
      list.append(el('form', { class: 'custom-target', onsubmit: (e) => { e.preventDefault(); setIt(); } },
        segmented([['off', 'Off at'], ['rest', 'Rest to']], kind, (v) => (kind = v)),
        el('div', { class: 'ct-row' },
          el('label', { class: 'field' }, el('span', {}, `Target (${u})`), tv),
          el('label', { class: 'field' }, el('span', {}, 'Name'), nm)),
        /* dismissive or secondary left, committing right */
        el('div', { class: 'btnrow' },
          el('button', { class: 'btn', type: 'button', onclick: saveIt }, 'Save'),
          el('button', { class: 'btn primary', type: 'submit' }, 'Set Target'))));
      list.append(stepsBox);
      drawSteps();
      const saved = customPresets();
      for (const [i, c] of saved.entries()) {
        const steps = (c.steps || []).map((x) => [x.name, x.temp]);
        list.append(el('div', { class: 'done' },
          el('button', { class: 'done-main', type: 'button', style: 'all:unset;cursor:pointer;flex:1;min-width:0', onclick: () => pick(c.target, '', c.name, 0, steps, c.rest ? c.target : 0) },
            el('div', { class: 'done-name' }, c.name),
            el('div', { class: 'done-note' }, steps.length ? summary(steps, c.target) : c.rest ? 'rest to' : 'off at')),
          el('div', { class: 'done-temps' }, el('div', { class: 'done-pull' }, `${c.target}${u}`), c.rest ? el('div', { class: 'done-final' }, 'rested') : null),
          iconBtn('trash-2', `Remove ${c.name}`, { class: 'danger', onclick: async () => { saved.splice(i, 1); await saveCustomPresets(saved); showMeat('Custom'); } })));
      }
    };

    const showMeat = (m) => {
      meat = m;
      chips.querySelectorAll('button').forEach((b) => b.classList.toggle('on', b.dataset.meat === m));
      list.innerHTML = '';
      if (m === 'Custom') { showCustom(); return; }
      for (const d of PRESETS[m]) {
        /* large: the doneness itself; under it, when it comes off if that is earlier, or how far a
           pull at the number coasts */
        const done = toUser(d.to), pull = toUser(d.to - d.carry), coast = d.coast ? toUser(d.to + d.coast) : 0;
        const rest = d.carry > 0 ? done : coast;
        const steps = d.steps.map(([n, t]) => [n, toUser(t)]);
        list.append(el('button', { class: 'done', type: 'button', onclick: () => pick(pull, m, d.name, rest, steps, d.carry > 0 ? done : 0) },
          el('div', { class: 'done-main' },
            el('div', { class: 'done-name' }, d.name),
            el('div', { class: 'done-note' }, summary(steps, pull))),
          el('div', { class: 'done-temps' },
            el('div', { class: 'done-pull' }, `${done}${u}`),
            d.carry > 0 ? el('div', { class: 'done-final' }, `off \u2248${pull}${u}`) : coast ? el('div', { class: 'done-final' }, `coasts to ${coast}${u}`) : null)));
      }
    };

    for (const m of ['Custom', ...Object.keys(PRESETS)]) chips.append(el('button', { class: 'chip-btn', type: 'button', 'data-meat': m, onclick: () => showMeat(m) }, m));
    showMeat(meat);

    return el('div', { class: 'sheet' }, head, chips, list,
      el('div', { class: 'sheet-foot' },
        el('label', {}, 'At target'),
        segmented(AFTER, after, (v) => (after = v))),
      /* dismissive left, committing right: see docs/design-language.md */
      el('div', { class: 'btnrow' },
        el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'),
        p.target > 0 ? el('button', { class: 'btn ghost', type: 'button', onclick: async () => { cur.length = 0; await saveProbeSteps(p.label, cur); close({ target: 0, after: 0 }); } }, 'Clear target') : null));
  });
}

/* Custom presets: a name, a target and its step alerts, saved from the Custom tab. Kept in the
 * grill's settings (notify.custom_presets), with the unit they were saved in so a unit change
 * converts them rather than reading 60 F as 60 C. */
function customPresets() {
  const raw = PF.settings?.notify?.custom_presets || [];
  const conv = (v, from) => (from === PF.units ? v : from === 'C' ? Math.round(v * 9 / 5 + 32) : Math.round((v - 32) * 5 / 9));
  return raw.filter((c) => c && c.name && c.target > 0).map((c) => ({
    name: c.name, target: conv(c.target, c.units || 'F'), rest: !!c.rest,
    steps: (c.steps || []).map((x) => ({ name: x.name, temp: conv(x.temp, c.units || 'F') })),
  }));
}
async function saveCustomPresets(list) {
  try { await patchSettings('notify', { custom_presets: list.map((c) => ({ ...c, units: PF.units })) }); } catch (e) { toast(e.message, true); }
}

/* The steps on the way to the target: a temperature with a name on it that says something once,
 * when it is crossed. It is what MEATER, Chef iQ and Combustion all give you and what actually
 * gets meat cooked properly -- the target is where it comes off, and a step is what you have to be
 * at the grill for before then. Four is enough for flip, wrap, probe-tender and a spare. */
const STEP_PRESETS = [['Flip', 120], ['Wrap', 165], ['Spritz', 150], ['Probe Tender', 198]];

async function saveProbeSteps(label, steps) {
  const all = { ...(PF.settings?.notify?.probe_steps || {}) };
  if (steps.length) all[label] = steps; else delete all[label];
  try { await patchSettings('notify', { probe_steps: all }); } catch (e) { toast(e.message, true); }
}

/* A probe's detail sheet: its reading, its link, what it is aiming at, and the things done to it
 * -- Change Target, Alarms, Timer, Clear Target -- and under those its steps and its alarms. */
export function probeSheet(label) {
  const p = () => PF.status?.probes?.find((x) => x.label === label);
  return dialog((close) => {
    const wrap = el('div', { class: 'sheet' });
    /* Redraw on the first status after a change, not on a timer: the status arrives once a
       second, and a redraw 600 ms after the command raced it -- when it lost, the sheet still
       said "Set Target" with nothing set, and the cook read that as the target not saving. */
    const onNext = (fn) => { const off = onStatus(() => { off(); fn(); }); };
    const render = () => {
      const q = p(); if (!q) { close(); return; }
      const hit = q.target > 0 && q.valid && q.temp >= q.target;
      wrap.innerHTML = '';
      wrap.append(
        el('div', { class: 'sheet-head' },
          el('div', {},
            el('h3', {}, q.wireless ? btIcon() : null, ' ', q.name),
            el('div', { class: 'help row', style: 'gap:8px' },
              q.wireless ? sigBars(q.signal || 0, q.rssi ? `${q.rssi} dBm` : 'no link') : null,
              q.wireless ? battIcon(q.battery) : null,
              q.valid ? (q.target > 0 ? (hit ? 'at target' : q.eta_s > 0 ? `${fmtEta(q.eta_s)} to target` : 'estimating…') : 'reading') : 'no reading',
              q.meat || q.done ? el('span', {}, `\u00b7 ${[q.meat, q.done].filter(Boolean).join(' \u00b7 ')}`) : null)),
          el('div', { class: 'sheet-now' }, q.valid ? fmtTemp(q.temp) : '—', el('small', {}, degUnit()))),

        el('div', { class: 'sheet-body' },
          /* The pit probe is not a food probe: what it is aiming at is the set point, set by Hold,
             and what shouts about it is a conditional notification comparing it with that set
             point. Offering a second target here would be a second answer to one question. */
          q.role === 'Primary'
            ? el('div', { class: 'btnrow' },
                actionBtn('hold', 'Hold Mode', { size: '', class: 'primary', onclick: () => { close(); location.hash = '#/settings/controller'; } }, MODE_ICON.Hold),
                actionBtn('rules', 'Alarm Rules', { size: '', onclick: () => { close(); location.hash = '#/settings/rules'; } }, 'bell'),
                actionBtn('timer', 'Timer', { size: '', onclick: async () => { const r = await timerDialog(); if (r) cmd({ cmd: 'timer', op: 'start', ...r }); } }, 'timer'))
            : el('div', { class: 'btnrow' },
                actionBtn('target', q.target > 0 ? 'Change Target' : 'Set Target', { size: '', class: 'primary', onclick: async () => { const r = await targetDialog(q); if (r) { cmd({ cmd: 'target', label: q.label, ...r }); onNext(render); } } }, MODE_ICON.Hold),
                actionBtn('alarms', 'Alarms', { size: '', onclick: async () => { const r = await limitsDialog(q); if (r) { cmd({ cmd: 'limits', label: q.label, ...r }); onNext(render); } } }, 'bell'),
                actionBtn('timer', 'Timer', { size: '', onclick: async () => { const r = await timerDialog(); if (r) cmd({ cmd: 'timer', op: 'start', ...r }); } }, 'timer')),
          q.role !== 'Primary' && q.target > 0 ? el('div', { class: 'form-actions' },
            actionBtn('delete', 'Clear Target', { onclick: () => { cmd({ cmd: 'target', label: q.label, target: 0, after: 0 }); onNext(render); } })) : null,

          q.steps?.length ? el('h2', {}, 'Steps') : null,
          q.steps?.length ? el('div', { class: 'kv' }, ...q.steps.flatMap((st) => [
            el('div', {}, st.done ? '\u2713 ' + st.name : st.name),
            el('div', {}, `${st.temp}${degUnit()}`)])) : null,
          el('h2', {}, 'Detail'),
          el('div', { class: 'kv' },
            el('div', {}, q.role === 'Primary' ? 'Set point' : 'Target'),
            el('div', {}, q.role === 'Primary'
              ? (PF.status?.mode === 'Hold' ? `${fmtTemp(PF.status.setpoint)}${degUnit()}` : PF.status?.mode || '—')
              : q.target > 0 ? `${fmtTemp(q.target)}${degUnit()}` : '—'),
            q.ambient_label ? el('div', {}, 'Ambient') : null, q.ambient_label ? el('div', {}, q.ambient == null ? '—' : `${fmtTemp(q.ambient)}${degUnit()}`) : null,
            q.role === 'Primary' ? null : el('div', {}, 'Alarm above'),
            q.role === 'Primary' ? null : el('div', {}, q.limit_high > 0 ? `${fmtTemp(q.limit_high)}${degUnit()}` : 'off'),
            q.role === 'Primary' ? null : el('div', {}, 'Alarm below'),
            q.role === 'Primary' ? null : el('div', {}, q.limit_low > 0 ? `${fmtTemp(q.limit_low)}${degUnit()}` : 'off')),
          q.role === 'Primary' ? el('p', { class: 'help' }, 'Over- and under-temperature alarms for the pit are conditional notifications, so they follow the set point when it changes.') : null),

        el('div', { class: 'form-actions' },
          actionBtn('cancel', 'Close', { size: '', onclick: () => close() })));
    };
    render();
    return wrap;
  });
}

/* What a tap on a probe does, on Home and on the Probes page alike: with no target set, straight
 * to the picker, because setting one is what the tap is for; with a target, the sheet, which is
 * where it is changed or cleared. The pit probe always opens its sheet: its target is the set
 * point. */
export async function openProbe(label) {
  const q = PF.status?.probes?.find((x) => x.label === label);
  if (!q) return;
  if (q.role !== 'Primary' && q.enabled !== false && !(q.target > 0)) {
    const r = await targetDialog(q);
    if (r) cmd({ cmd: 'target', label: q.label, ...r });
    return;
  }
  return probeSheet(label);
}

export async function limitsDialog(p) {
  return dialog((close) => {
    const hi = el('input', { type: 'text', inputmode: 'decimal', value: p.limit_high || '', placeholder: 'off' });
    const lo = el('input', { type: 'text', inputmode: 'decimal', value: p.limit_low || '', placeholder: 'off' });
    return el('form', { onsubmit: (e) => { e.preventDefault(); close({ high: parseFloat(hi.value) || 0, low: parseFloat(lo.value) || 0 }); } },
      el('h3', {}, `${p.name} alarms`),
      el('p', { class: 'help' }, 'Alerts when the probe leaves this range. Blank disables.'),
      el('div', { class: 'field inline' }, el('label', {}, `Alarm above (${degUnit()})`), hi),
      el('div', { class: 'field inline' }, el('label', {}, `Alarm below (${degUnit()})`), lo),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
  });
}

export async function timerDialog() {
  return dialog((close) => {
    let after = 0;
    const mins = el('input', { type: 'text', inputmode: 'numeric', value: 30, 'aria-label': 'Minutes' });
    return el('form', { onsubmit: (e) => { e.preventDefault(); const m = parseFloat(mins.value); if (m > 0) close({ seconds: Math.round(m * 60), after }); } },
      el('h3', {}, 'Set timer'),
      el('div', { class: 'num-input' }, el('button', { class: 'btn', type: 'button', onclick: () => (mins.value = Math.max(1, (parseFloat(mins.value) || 0) - 5)) }, '−'), mins, el('span', { class: 'muted' }, 'min'), el('button', { class: 'btn', type: 'button', onclick: () => (mins.value = (parseFloat(mins.value) || 0) + 5) }, '+')),
      el('div', { class: 'presets' }, [10, 15, 30, 45, 60, 90, 120].map((m) => el('button', { class: 'btn sm', type: 'button', onclick: () => (mins.value = m) }, `${m} min`))),
      el('div', { class: 'field' }, el('label', {}, 'When the timer ends'), segmented(AFTER, after, (v) => (after = v))),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Start')));
  });
}

const MODES = [['Startup', 'Startup'], ['Smoke', 'Smoke'], ['Hold', 'Hold'], ['Shutdown', 'Shutdown']];
/* A step's ending, as a condition tree of exactly the kind a notification is built from.
 *
 * The daemon publishes a "step" domain in the same catalogue -- time in the step, the hottest and
 * the coolest food probe, the same probes rested, whether you confirmed, whether the lid was
 * opened -- so the rows, the operators and the AND / OR / NOT are the ones already learned next
 * door, and "for ten minutes" means there what it means here.
 *
 * This replaced a bespoke set of segmented controls that asked the same question in a different
 * visual language, which is the one thing this interface is not allowed to do. */
const blankEnds = () => ({ op: 'all', conditions: [] });

/* How a step carries on once its conditions are met. These are the two things only a person can
   do, and they are asked for here as one plain choice rather than as rows in the condition list:
   "after the lid opens, or I confirm" is how a cook says it, and a row reading "Lid Opened is on"
   is not. Underneath they are the prompt and lid facts the daemon already evaluates. */
const CARRY = [
  ['auto', 'Right away'],
  ['prompt', 'After I confirm'],
  ['lid_or', 'After the lid opens, or I confirm'],
  ['lid_and', 'After the lid opens and I confirm'],
];
const HANDOVER = new Set(['prompt', 'lid']);

/* A step's ending is stored as one condition tree, because that is what the daemon evaluates. The
   editor shows it as two things: the list of conditions, joined by AND or OR, and how it carries
   on. Splitting is what makes the built-in ribs step read as "3 h or 160 F, then wait for the lid
   or me" instead of a nest of groups. */
function splitEnding(tree) {
  const out = { when: { op: 'all', conditions: [] }, carry: 'auto' };
  if (!tree) return out;
  const leaves = (n, acc) => { if (Array.isArray(n?.conditions)) n.conditions.forEach((k) => leaves(k, acc)); else if (n?.trait) acc.push(n); return acc; };
  const hand = leaves(tree, []).filter((n) => HANDOVER.has(n.trait));
  const hasPrompt = hand.some((n) => n.trait === 'prompt'), hasLid = hand.some((n) => n.trait === 'lid');
  /* the group holding the handover, if it is a group, says how the two join */
  const findHandGroup = (n) => Array.isArray(n?.conditions) && n.conditions.length && n.conditions.every((k) => k.trait && HANDOVER.has(k.trait)) ? n
    : Array.isArray(n?.conditions) ? n.conditions.map(findHandGroup).find(Boolean) : null;
  const hg = findHandGroup(tree);
  out.carry = hasLid && hasPrompt ? (hg?.op === 'all' ? 'lid_and' : 'lid_or') : hasPrompt || hasLid ? 'prompt' : 'auto';
  /* everything else is the list; if it was a group of its own, keep its AND / OR */
  const rest = Array.isArray(tree.conditions) ? tree.conditions.filter((k) => !(k.trait && HANDOVER.has(k.trait)) && k !== hg) : (tree.trait && !HANDOVER.has(tree.trait) ? [tree] : []);
  if (rest.length === 1 && Array.isArray(rest[0].conditions)) out.when = { op: rest[0].op === 'any' ? 'any' : 'all', conditions: leaves(rest[0], []).filter((n) => !HANDOVER.has(n.trait)) };
  else out.when = { op: tree.op === 'any' ? 'any' : 'all', conditions: rest.flatMap((k) => leaves(k, [])).filter((n) => !HANDOVER.has(n.trait)) };
  return out;
}
function joinEnding(when, carry) {
  const terms = when.conditions || [];
  const hand = carry === 'prompt' ? { trait: 'prompt', op: 'is_on' }
    : carry === 'lid_or' || carry === 'lid_and'
      ? { op: carry === 'lid_and' ? 'all' : 'any', conditions: [{ trait: 'prompt', op: 'is_on' }, { trait: 'lid', op: 'is_on' }] }
      : null;
  const list = terms.length ? (terms.length === 1 ? terms[0] : { op: when.op || 'all', conditions: terms }) : null;
  if (list && hand) return { op: 'all', conditions: [list, hand] };
  if (hand) return hand;
  return { op: when.op || 'all', conditions: terms };
}


/* One line saying what a step does, for the row in the list and for the header of the card when it
   is folded shut -- the same rule the notification editor follows: a card you cannot read without
   opening it is a card that has to be opened. The ending is described by the shared code, so a
   step and a notification say the same condition in the same words. */
/* A term of an ending in the words a cook uses -- "for 3 h", "160°F probe", "205°F probe,
   rested" -- rather than the generic "Hottest Food Probe is at or above 160". The generic form is
   right for a notification about anything; a recipe is about the meat and the clock. */
/* Each thing that can end a stage, as a mark and a few words: a stopwatch for time on the clock,
   a thermometer for a probe, an hourglass for time still to run, a battery. The mark is the one
   every condition of that kind wears, from conditions.js, so the rail and the rule editor agree. */
function endingTerm(n) {
  const u = degUnit();
  const v = n.value;
  if (n.trait === 'elapsed') return ['timer', fmtSecs(v)];
  if (n.trait === 'food_max') return ['thermometer', `${v}${u} probe`];
  if (n.trait === 'food_min') return ['thermometer', `all probes ${v}${u}`];
  if (n.trait === 'food_avg') return ['thermometer', `probes average ${v}${u}`];
  if (n.trait === 'food_rested') return ['thermometer', `${v}${u} probe, rested`];
  if (n.trait === 'food_eta') return ['hourglass', `probe within ${fmtSecs(v)} of target`];
  if (n.trait === 'food_battery') return ['battery', `probe battery ${OP_SYM[n.op] || n.op} ${v}%`];
  return [condIcon(n, 'step'), describeNode(n, 'step', false)];
}
const inl = (name) => lucide(name, 'ic inl');
/* the terms joined by "or" / "and", as nodes for a title or a row */
function endingNodes(when) {
  const out = [];
  const terms = (when?.conditions || []).map(endingTerm);
  terms.forEach(([ic, text], i) => {
    if (i) out.push(when?.op === 'any' ? ' or ' : ' and ');
    out.push(inl(ic), text);
  });
  return out;
}
const CARRY_SAID = { prompt: 'waits for you', lid_or: 'waits for you or the lid', lid_and: 'waits for the lid, then you' };
function stepHead(s) {
  if (s.mode === 'Startup') return 'Startup' + (s.setpoint ? ` to ${s.setpoint}${degUnit()}` : '');
  if (s.mode === 'Shutdown' || s.mode === 'Stop') return s.mode;
  return `${s.mode}${s.setpoint ? ` ${s.setpoint}${degUnit()}` : ''}`;
}
/* the card's title on the rail: the mode and set point, then what ends it, with the marks */
function stepTitleNodes(s) {
  const head = stepHead(s);
  if (s.mode !== 'Hold' && s.mode !== 'Smoke') return [head];
  const nodes = endingNodes(splitEnding(s.ends).when);
  return nodes.length ? [head, ' ', ...nodes] : [head];
}
/* What happens at the end of a stage, for the lines between it and the next: the message, and who
   it waits for. Each is an event on the rail with its own mark -- what is said, the cook's hand,
   the lid. */
function handoverEvents(s) {
  const c = splitEnding(s.ends).carry;
  const out = [];
  if (s.message) out.push({ icons: ['message-square'], text: s.message, cls: 'msg' });
  if (c === 'prompt') out.push({ icons: ['hand'], text: 'Waits for you', cls: 'wait' });
  if (c === 'lid_or') out.push({ icons: ['hand'], text: 'Waits for you or the lid', cls: 'wait' });
  if (c === 'lid_and') out.push({ icons: ['door-open'], text: 'Waits for the lid, then you', cls: 'wait' });
  return out;
}

/* What is wrong with the shape of a recipe, mirroring pf_recipe_shape_warnings in
   src/features/recipe.c -- the two must agree. A recipe lights the grill before it cooks and puts
   it out when it is done; neither is enforced outright, because a recipe of nothing but Shutdown
   is a cool-down and must not gain a step that lights the grill, and one that hands back a hot
   grill on purpose is a real thing the runner already asks about when it gets there. */
const COOKING = ['Startup', 'Smoke', 'Hold'];
function shapeIssues(steps) {
  const out = [];
  if (!steps.length) return out;
  if (steps.some((s) => COOKING.includes(s.mode)) && steps[0].mode !== 'Startup') {
    out.push({ code: 'no_startup', text: 'Does not light the grill first.',
      fix: 'Add Startup', apply: () => steps.unshift({ mode: 'Startup' }) });
  }
  if (steps[steps.length - 1].mode !== 'Shutdown') {
    out.push({ code: 'no_shutdown', text: 'Leaves the grill running when it finishes.',
      fix: 'Add Shutdown', apply: () => steps.push({ mode: 'Shutdown' }) });
  }
  return out;
}

const blankStep = () => ({ mode: 'Hold', setpoint: PF.units === 'C' ? 110 : 225, ends: blankEnds(), message: '' });

/* The editor is a pushed screen with a pinned action bar, and each step is a fold -- the same two
   shapes the notification editor uses. It was a dialog full of <fieldset>s with arrow buttons,
   which is the one screen in the app that looked like a form someone had bolted on. */
function recipeEditor(rec0, isNew) {
  const rec = structuredClone(rec0);
  rec.steps ||= [];
  rec.units = PF.units;   /* the numbers on screen are in the unit shown, and are stored as such */

  return pushScreen((close) => {
    const wrap = el('div', { class: 'sheet' });
    let ready = false;
    const touched = () => { if (ready) wrap.dispatchEvent(new CustomEvent('pf-dirty', { bubbles: true })); };
    const body = el('div');

    const stepCard = (s, i, redraw, glyphNode) => {
      const det = el('details', { class: 'fold cond-card', open: false });
      const title = el('span', { class: 'cc-title' });
      const head = el('summary', { class: 'cc-head' },
        el('span', { class: 'cc-glyph' }, glyphNode), title,
        iconBtn('trash-2', 'Remove this step', { class: 'danger cc-del',
          onclick: (e) => { e.preventDefault(); e.stopPropagation(); rec.steps.splice(i, 1); touched(); redraw(); } }));
      const inner = el('div', { class: 'cc-body' });
      det.append(head, inner);
      const retitle = () => { title.replaceChildren(...stepTitleNodes(s)); if (!title.childNodes.length) title.textContent = 'New step'; };
      const changed = () => { touched(); retitle(); };

      const field = (label, node, help) => el('div', { class: 'field' },
        el('label', {}, label), help ? el('div', { class: 'help' }, help) : null, node);
      const num = (get, set, extra = {}) => el('input', {
        type: 'text', inputmode: 'decimal', value: get() || '', ...extra,
        onchange: (e) => { set(parseFloat(e.target.value) || 0); changed(); } });

      const draw = () => {
        inner.innerHTML = '';
        inner.append(field('Mode', segmented(MODES, s.mode, (v) => { s.mode = v; changed(); draw(); })));
        if (s.mode === 'Hold') inner.append(field(`Set Point (${degUnit()})`, num(() => s.setpoint, (v) => (s.setpoint = v))));
        if (s.mode === 'Hold' || s.mode === 'Smoke') {
          inner.append(el('label', { class: 'toggle' },
            el('div', {}, el('div', {}, 'Smoke+')),
            el('span', { class: 'switch' }, el('input', { type: 'checkbox', checked: !!s.s_plus,
              onchange: (e) => { s.s_plus = e.target.checked; changed(); } }), el('span'))));
          /* When the step ends, in the same cards the notification editor uses -- the fold with its
             own summary, AND / OR / NOT as & \u2265 \u2260, one Add condition that asks what kind, and
             a duration on any of them. "Three hours, or any food probe at 160, and then you
             confirm" is one tree and reads as one sentence. */
          /* Kept split while editing and joined back into s.ends on every change, so what the
             daemon reads is always the one tree. */
          s._e ||= splitEnding(s.ends);
          const sync = () => { s.ends = joinEnding(s._e.when, s._e.carry); changed(); };
          inner.append(el('div', { class: 'field' }, el('label', {}, 'Ends When'),
            condNode(s._e.when, 'step', sync, null, 0, { flat: true, exclude: ['prompt', 'lid'] })));
          inner.append(field('Then Carry On', el('select', { onchange: (e) => { s._e.carry = e.target.value; sync(); } },
            CARRY.map(([v, l]) => el('option', { value: v, selected: s._e.carry === v }, l)))));
        }
        inner.append(field('Message', el('input', { type: 'text', value: s.message || '', placeholder: 'e.g. Wrap the ribs',
          onchange: (e) => { s.message = e.target.value; changed(); } }), 'Sent when the step ends'));
        /* Being told to fetch foil at the moment the ribs need wrapping means opening the lid to go
           and find it. The warning is timed off the estimate, so it works for a step that ends on a
           temperature as well as one that ends on a clock. */
        inner.append(el('details', { class: 'fold' }, el('summary', {}, el('span', {}, 'Warn Me Before')),
          el('div', { class: 'card tight' },
            field('Minutes Before', num(() => s.lead_min, (v) => (s.lead_min = v), { placeholder: '0' })),
            field('Warning', el('input', { type: 'text', value: s.lead_message || '', placeholder: 'e.g. Get the foil out',
              onchange: (e) => { s.lead_message = e.target.value; changed(); } })))));
        retitle();
      };
      draw();
      return det;
    };

    const draw = () => {
      body.innerHTML = '';
      body.append(
        el('div', { class: 'field' }, el('label', {}, 'Name'),
          el('input', { type: 'text', value: rec.name || '', placeholder: 'e.g. Pulled pork',
            onchange: (e) => { rec.name = e.target.value; touched(); } })),
        el('div', { class: 'field' }, el('label', {}, 'Description'),
          el('input', { type: 'text', value: rec.description || '', placeholder: 'One line about it',
            onchange: (e) => { rec.description = e.target.value; touched(); } })));
      const steps = el('div', { class: 'card tight' }, el('div', { class: 'field' }, el('label', {}, 'Steps')));
      /* Said while the recipe is being written, with the remedy next to it, rather than after it
         has been saved. Each one is one tap from gone. */
      for (const iss of shapeIssues(rec.steps)) {
        steps.append(el('div', { class: 'notice warn' },
          el('span', { class: 'grow' }, iss.text),
          el('button', { class: 'btn xs ghost', type: 'button',
            onclick: () => { iss.apply(); touched(); draw(); } }, iss.fix)));
      }
      /* A timeline, read top to bottom: lighting at the top, shutting down at the bottom, and
         between them the stages of the cook, numbered as a cook counts them. What happens at the
         end of a stage -- the message, and who it waits for -- is the line between it and the
         next, because that is where it happens. It used to be a flat list of seven "Hold" rows,
         two of them waits for the cook, and the flow made no sense. */
      const tl = el('div', { class: 'tl' });
      rec.steps.forEach((s, i) => {
        const cooks = s.mode === 'Hold' || s.mode === 'Smoke';
        /* The rail is a line of events, each wearing its mark: a flame lights it, crosshairs hold
           it, a cloud smokes it, a power mark puts it out; between the stages, what is said and who
           is waited for. A number on the rail said which stage this was and nothing else. */
        tl.append(el('div', { class: `tl-item${cooks ? '' : ' tl-end'}` }, stepCard(s, i, draw, lucide(MODE_ICON[s.mode] || 'crosshair'))));
        if (cooks) for (const ev of handoverEvents(s)) {
          tl.append(el('div', { class: 'tl-hand' }, el('span', { class: `tl-flag ${ev.cls}` }, ev.icons.map((n) => lucide(n))), el('span', {}, ev.text)));
        }
      });
      steps.append(tl);
      if (!rec.steps.length) steps.append(el('div', { class: 'muted', style: 'padding:6px 2px' }, 'No stages yet.'));
      steps.append(el('div', { class: 'cc-add' }, actionBtn('add', 'Add stage', {
        onclick: () => {
          /* a new stage goes before the shutdown, not after it */
          const last = rec.steps[rec.steps.length - 1];
          const at = last && (last.mode === 'Shutdown' || last.mode === 'Stop') ? rec.steps.length - 1 : rec.steps.length;
          rec.steps.splice(at, 0, blankStep()); touched(); draw();
        } })));
      body.append(steps);
    };
    draw();

    const dismiss = async () => {
      if (snapshot() !== base &&
          !await confirmDialog('Discard changes?', rec.name || '', 'Discard', true)) return;
      close(undefined);
    };
    wrap.append(el('div', { class: 'sheet-body' }, body),
      screenActions({
        onDelete: isNew ? null : async () => {
          if (await confirmDialog('Delete recipe?', rec0.name || '', 'Delete', true)) close('delete');
        },
        deleteTitle: 'Delete recipe',
        onCancel: dismiss,
        onSave: async () => {
          if (!rec.name?.trim()) { toast('Give it a name', true); return; }
          const issues = shapeIssues(rec.steps);
          /* A cooking recipe gets its Startup step whether or not it was asked for. It costs
             nothing -- the runner skips it on a grill that is already lit -- and without it a
             recipe run on a cold grill simply never gets going. */
          const needStart = issues.find((i) => i.code === 'no_startup');
          if (needStart) { needStart.apply(); toast('Added a Startup step so it lights a cold grill'); }
          /* Ending without one is allowed, and is the thing to be warned about rather than stopped
             for: the runner asks what to do with the lit grill when it gets there. */
          for (const st of rec.steps) delete st._e;   /* editor scratch, not part of the recipe */
          if (issues.some((i) => i.code === 'no_shutdown')
              && !await confirmDialog('Leave the grill running?',
                   'This recipe does not end with a Shutdown step. When it finishes the grill will still be lit, and PiFire will ask you whether to shut it down.',
                   'Save Anyway')) { draw(); return; }
          close(rec);
        },
        dirty: isNew,
      }));
    /* Against what the first draw settled on: a step opened for editing gains its split ending and
       a blank one its defaults, and none of that is a change the person made. */
    const snapshot = () => { const c = structuredClone(rec); for (const st of c.steps) delete st._e; return JSON.stringify(c); };
    let base = snapshot();
    setTimeout(() => { ready = true; base = snapshot(); }, 0);
    return wrap;
  }, { title: isNew ? 'New Recipe' : rec0.name, back: 'Cook' });
}

/* The recipe's step group -- back, the step count, forward -- the same on Home's control bar
   and on the run card. Forward flashes while the recipe waits on the cook and asks in the step's
   own words; moving by hand in either direction always asks first. */
export function stepControls(rc) {
  const stepIx = rc.step ?? 0;
  const btn = (ic, label, opts) => el('button', { class: `cb accent ${opts.cls || ''}`, disabled: !!opts.disabled, 'aria-label': label, onclick: opts.onclick }, lucide(ic));
  return el('div', { class: 'cbar mini' }, el('div', { class: 'cgroup steps' },
    btn('chevron-left', 'Previous step', { disabled: stepIx === 0, onclick: async () => {
      if (await confirmDialog('Go back a step?', `Starts step ${stepIx} again.`, 'Go back')) cmd({ cmd: 'recipe', op: 'back' });
    } }),
    el('span', { class: 'cb-label', title: rc.name }, `${stepIx + 1}/${rc.nsteps}`),
    btn('chevron-right', rc.waiting ? 'Continue' : 'Skip to the next step', { cls: rc.waiting ? 'flash' : '', onclick: async () => {
      if (rc.waiting) {
        if (rc.needs_lid) { toast('Open the lid first, then continue'); return; }
        if (await confirmDialog('Continue to the next step?', rc.message || 'This step is done.', 'Continue')) cmd({ cmd: 'recipe', op: 'next' });
      } else if (await confirmDialog('Skip this step?', `Ends step ${stepIx + 1} now and starts step ${stepIx + 2}.`, 'Skip')) cmd({ cmd: 'recipe', op: 'skip' });
    } })));
}

export function renderCook(view) {
  const timerCard = el('div', { class: 'card' });
  const runCard = el('div', { class: 'card run-card' });
  const recipeList = el('div', { class: 'ios-list' });
  const showRecipes = PF.settings?.globals?.show_recipes !== false;
  const recipeSection = el('div', { hidden: !showRecipes }, el('h2', {}, 'Recipes'), recipeList);
  view.append(runCard, el('h2', {}, 'Timer'), timerCard, recipeSection);

  const saveRecipe = async (r) => {
    try { await api('/recipes', { body: r }); } catch (e) { toast(e.message, true); }
    loadRecipes();
  };
  const edit = async (r, isNew) => {
    /* The condition rows are built from the daemon's catalogue, so it has to be in hand before the
       editor draws rather than after. */
    await catalogue();
    const out = await recipeEditor(r, isNew);
    if (out === 'delete') { await api(`/recipes/${r.id}/delete`, { body: {} }).catch(() => {}); loadRecipes(); }
    else if (out) await saveRecipe(out);
  };

  /* One tap to run. The confirmation says what the first step will do rather than warning in the
     abstract, because "the recipe takes over the grill" is true of every recipe and tells nobody
     anything they did not already intend. */
  const run = async (r) => {
    /* One question and one button. Which probes are in the food is the only thing a recipe
       needs to know before it starts, so that is what Run asks, and the answer's button is Start;
       a "Run this?" in front of it was a second tap for nothing. */
    const labels = await pickFoodProbes(`Run ${r.name}`, 'Which probes are in the food?');
    if (labels === undefined) return;
    await cmd({ cmd: 'probes_in_use', labels });
    cmd({ cmd: 'recipe', op: 'start', id: r.id });
  };

  /* A recipe is a row, like a notification or a setting: its name, and on one line what it does
     in the cook's words -- `180\u00b0F 3 h or 160\u00b0F probe \u00b7 225\u00b0F 2 h`. The two things
     done to it are on the right of the row as marks, Play and a pencil; deleting one is done from
     its own screen, behind a confirmation, where the name of what is about to go is in the title. */
  const plan = (r) => {
    const nodes = [];
    for (const s of r.steps || []) {
      if (s.mode !== 'Hold' && s.mode !== 'Smoke') continue;
      if (nodes.length) nodes.push(' \u00b7 ');
      nodes.push(inl(MODE_ICON[s.mode]), s.setpoint ? `${s.setpoint}${degUnit()}` : s.mode);
      const ends = endingNodes(splitEnding(s.ends).when);
      if (ends.length) nodes.push(' ', ...ends);
    }
    return nodes;
  };
  /* the recipes by id, for the run card's rail; loaded with the list, or on demand when the
     list is switched off */
  const recipesById = new Map();
  let fetching = null;
  const fetchRecipes = () => fetching || (fetching = api('/recipes').then((list) => { recipesById.clear(); for (const r of list) recipesById.set(r.id, r); update(PF.status); }).catch(() => {}).finally(() => { fetching = null; }));
  const loadRecipes = () => api('/recipes').then((list) => {
    recipesById.clear(); for (const r of list) recipesById.set(r.id, r);
    recipeList.innerHTML = '';
    for (const r of list) {
      recipeList.append(itemRow({
        icon: 'book-open', color: '#ff8a1f',
        title: r.name,
        meta: (() => { const n = plan(r); return n.length ? el('span', {}, ...n) : 'No stages'; })(),
        chevron: false,
        onclick: () => edit(r, false),
        /* the pencil, then Play at the far right: the committing action ends the row, as it ends
           every button row */
        actions: [
          iconBtn('share', `Share ${r.name}`, { onclick: (e) => { e.stopPropagation(); shareRecipe(r); } }),
          iconBtn('pencil', `Edit ${r.name}`, { onclick: (e) => { e.stopPropagation(); edit(r, false); } }),
          iconBtn('play', `Run ${r.name}`, { onclick: (e) => { e.stopPropagation(); run(r); } }),
        ],
      }));
    }
    if (!list.length) recipeList.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' },
      'No recipes yet. A recipe is a list of stages the grill runs for you.'));
  }).catch(() => {});
  /* One recipe as a file, handed to the phone's share sheet where there is one -- to a friend,
     to Files, to a message -- and downloaded where there is not. The same file Import reads. */
  const shareRecipe = async (r) => {
    const doc = { app: 'pifire-c', kind: 'recipes', format: 1, created: Date.now() / 1000, recipes: [{ ...r, id: undefined }] };
    const name = `${(r.name || 'recipe').replace(/[^\w-]+/g, '-').toLowerCase()}.pifire-recipe.json`;
    const blob = new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' });
    try {
      const file = new File([blob], name, { type: 'application/json' });
      if (navigator.canShare?.({ files: [file] })) { await navigator.share({ files: [file], title: r.name }); return; }
    } catch (e) { if (e?.name === 'AbortError') return; }
    const url = URL.createObjectURL(blob);
    const a = el('a', { href: url, download: name });
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 10000);
    toast('Saved the recipe file');
  };
  const importRecipes = () => {
    const f = el('input', { type: 'file', accept: 'application/json,.json' });
    f.onchange = async () => {
      const file = f.files?.[0];
      if (!file) return;
      let doc;
      try { doc = JSON.parse(await file.text()); } catch { toast(`${file.name} is not a recipe file`, true); return; }
      const n = Array.isArray(doc) ? doc.length : (doc.recipes || []).length;
      if (!await confirmDialog(`Import ${n === 1 ? 'this recipe' : `${n} recipes`}?`, 'A recipe with the same name as one on the grill replaces it; the rest are added.', 'Import')) return;
      try { const r = await api('/recipes/import', { body: doc }); toast(`Imported ${r.imported} recipe${r.imported === 1 ? '' : 's'}${r.replaced ? `, ${r.replaced} replaced` : ''}`); loadRecipes(); }
      catch (e) { toast(e.message || 'That file does not hold recipes', true); }
    };
    f.click();
  };
  /* The two things this page does with recipes, on the bar the probes page uses: bring one in on
     the left, add one on the right in the primary colour, the Home button riding over the gap. */
  if (showRecipes) view.append(actionBar(
    [actionBtn('upload', 'Import', { size: '', onclick: importRecipes })],
    [actionBtn('add', 'Add Recipe', { size: '', class: 'primary', onclick: () => edit({ name: '', description: '', steps: [{ mode: 'Startup' }, blankStep(), { mode: 'Shutdown' }] }, true) })]));
  if (showRecipes) loadRecipes();

  /* What happened is behind the bell, which keeps it across devices and clears it on all of them
     at once. A second, shorter copy on this page could only ever disagree with it. */
  const update = (s) => {
    if (!s) return;
    const rc = s.recipe;
    runCard.hidden = !rc.active;
    if (rc.active) {
      /* While a recipe runs, this card is the page: what it is doing, how long until it needs you,
         and -- when it needs you now -- one button the width of the card, because the moment it is
         asking for something is the moment nothing else on the screen matters. */
      /* the DOM's replaceChildren() writes a null out as the word "null", unlike el(); nothing
         optional goes to it unfiltered */
      /* The run card is the recipe's own timeline, live: every step on the rail with its mark,
         the ones behind it ticked off, the one running it says what it is waiting for -- the
         climb to the set point, the clock, the probe, or you -- and the ones ahead dimmed. A
         thin grey bar said none of that and looked like nothing else on these screens. */
      const rec = recipesById.get(rc.id);
      if (!rec) { fetchRecipes(); }
      const stepIx = rc.step ?? 0;
      const flags = rc.flags || [];
      const u = degUnit();
      /* One row per step: its mark, one line of title with the live state folded in for the one
         running, and on the right the overrides the cook can set for the run -- marks only, the
         way Edit and Play sit on a recipe's row. Pause holds the step at its end until the cook
         continues; Skip passes the step over when the run reaches it; Auto answers a step's
         prompt by itself. A tap sets it, a second tap clears it. */
      const rail = el('div', { class: 'run-rail' });
      const setFlag = (i, f) => cmd({ cmd: 'recipe', op: 'flag', step: i, flag: flags[i] === f ? 'none' : f });
      const wantsPrompt = (st) => JSON.stringify(st.ends || '').includes('"prompt"');
      for (const [i, st] of (rec?.steps || []).entries()) {
        const state = i < stepIx ? 'rr-done' : i === stepIx ? 'rr-now' : 'rr-todo';
        const title = el('span', { class: 'rr-title' }, ...stepTitleNodes(st));
        if (state === 'rr-now') {
          let said = '';
          if (rc.waiting) said = rc.needs_lid ? 'lid, then continue' : 'Continue?';
          else if (st.mode === 'Hold' && rc.at_temp === false) said = `heating${rc.remaining_s >= 0 ? ` \u00b7 ~${fmtDur(rc.remaining_s)}` : ''}`;
          else if (rc.clock_s >= 0) said = `${fmtDur(rc.clock_s)} left${rc.remaining_s >= 0 && rc.remaining_s < rc.clock_s - 30 ? ` \u00b7 ~${fmtDur(rc.remaining_s)} by probe` : ''}`;
          else if (rc.remaining_s >= 0) said = `~${fmtDur(rc.remaining_s)} by probe`;
          if (said) title.append(el('span', { class: 'rr-state' }, ` \u00b7 ${said}`));
        }
        const acts = el('span', { class: 'rr-acts' });
        if (state !== 'rr-done') {
          acts.append(iconBtn('pause', 'Pause when this step ends', { class: flags[i] === 'hold' ? 'on' : '', onclick: () => setFlag(i, 'hold') }));
          if (state === 'rr-todo') acts.append(iconBtn('chevrons-right', 'Skip this step', { class: flags[i] === 'skip' ? 'on' : '', onclick: () => setFlag(i, 'skip') }));
          if (wantsPrompt(st)) acts.append(iconBtn('circle-check', 'Continue on its own', { class: flags[i] === 'auto' ? 'on' : '', onclick: () => setFlag(i, 'auto') }));
        }
        rail.append(el('div', { class: `rr-step ${state}${flags[i] ? ` rr-${flags[i]}` : ''}` },
          el('span', { class: 'rr-glyph' }, lucide(state === 'rr-done' ? 'check' : flags[i] === 'skip' ? 'chevrons-right' : (MODE_ICON[st.mode] || 'crosshair'))),
          title, acts));
      }
      runCard.replaceChildren(...[
        el('div', { class: 'row between' },
          el('div', { style: 'min-width:0' },
            el('div', { class: 'run-name' }, rc.name),
            el('div', { class: 'help' }, `Step ${stepIx + 1} of ${rc.nsteps}`)),
          stepControls(rc)),
        rail,
        rc.waiting && rc.message ? el('div', { class: 'notice warn' }, el('span', {}, rc.message)) : null,
        rc.waiting && rc.needs_lid
          ? el('div', { class: 'help' }, 'Open the lid, then confirm.')
          : rc.waiting
            ? el('button', { class: 'btn primary block', type: 'button', onclick: () => cmd({ cmd: 'recipe', op: 'next' }) }, 'Continue')
            : null,
        el('div', { class: 'form-actions' },
          el('button', { class: 'btn sm ghost', type: 'button', onclick: async () => {
            if (await confirmDialog('Stop the recipe?', 'The grill keeps running in whatever mode the current step set.', 'Stop recipe', true)) cmd({ cmd: 'recipe', op: 'stop' });
          } }, 'Stop recipe'))].filter(Boolean));
    }
    const t = s.timer;
    timerCard.innerHTML = '';
    if (t.running) {
      timerCard.append(el('div', { class: 'row between' },
        el('div', {}, el('div', { class: 'readout-xl' }, fmtDur(t.remaining)), el('div', { class: 'help' }, `${t.paused ? 'Paused' : 'Running'} · ${AFTER.find((a) => a[0] === t.after)?.[1]}`)),
        el('div', { class: 'btnrow' },
          el('button', { class: 'btn sm', onclick: () => cmd({ cmd: 'timer', op: t.paused ? 'resume' : 'pause' }) }, t.paused ? 'Resume' : 'Pause'),
          el('button', { class: 'btn sm ghost', onclick: () => cmd({ cmd: 'timer', op: 'cancel' }) }, 'Cancel'))));
      timerCard.append(el('div', { class: 'progress' }, el('div', { style: `width:${Math.max(0, Math.min(100, 100 - (t.remaining / t.duration) * 100))}%` })));
    } else {
      timerCard.append(el('button', { class: 'btn block', onclick: async () => { const r = await timerDialog(); if (r) cmd({ cmd: 'timer', op: 'start', ...r }); } }, 'Set a timer'));
    }
  };
  update(PF.status);
  return onStatus(update);
}
