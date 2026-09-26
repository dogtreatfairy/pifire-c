import { PF, el, api, cmd, onStatus, fmtTemp, degUnit, fmtDur, dialog, pushScreen, numberDialog, toast, confirmDialog, segmented, actionBtn, itemRow, iconBtn, addRow, transferRow, patchSettings, screenActions } from '../app.js';
import { fmtEta } from './probes.js';
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
const PRESETS = {
  Beef: [
    { name: 'Rare', to: 125, carry: 5, note: 'Cool red centre' },
    { name: 'Medium rare', to: 135, carry: 5, note: 'Warm red centre' },
    { name: 'Medium', to: 145, carry: 5, note: 'Warm pink centre' },
    { name: 'Medium well', to: 150, carry: 5, note: 'Slightly pink' },
    { name: 'Well done', to: 160, carry: 5, note: 'Cooked through' },
  ],
  Brisket: [{ name: 'Probe tender', to: 203, carry: 0, note: 'Then rest, an hour or more' }],
  Pork: [
    { name: 'Chops and loin', to: 145, carry: 5, note: 'Rest three minutes' },
    { name: 'Pulled pork', to: 203, carry: 0, note: 'Falls apart' },
  ],
  Ribs: [{ name: 'Bend test', to: 195, carry: 0, note: 'Bones begin to show' }],
  Chicken: [
    { name: 'Breast', to: 165, carry: 5, note: 'Safe and still juicy' },
    { name: 'Thighs', to: 175, carry: 5, note: 'Dark meat, better higher' },
  ],
  Turkey: [{ name: 'Whole bird', to: 165, carry: 8, note: 'Measured in the breast' }],
  Fish: [{ name: 'Flaky', to: 145, carry: 3, note: 'Just opaque' }],
  Lamb: [
    { name: 'Medium rare', to: 135, carry: 5, note: '' },
    { name: 'Medium', to: 145, carry: 5, note: '' },
  ],
  Sausage: [{ name: 'Cooked through', to: 160, carry: 5, note: '' }],
};
const AFTER = [[0, 'Notify only'], [1, 'Keep warm'], [2, 'Shutdown']];
const toUser = (f) => (PF.units === 'C' ? Math.round((f - 32) * 5 / 9) : f);
const deltaUser = (f) => (PF.units === 'C' ? Math.round(f * 5 / 9) : f);

export async function targetDialog(p) {
  return dialog((close) => {
    let after = p.after || 0;
    let meat = 'Beef';

    const now = p.valid ? `${fmtTemp(p.temp)}${degUnit()}` : '—';
    const head = el('div', { class: 'sheet-head' },
      el('div', {}, el('h3', {}, p.name), el('div', { class: 'muted' }, `Now ${now}`)),
      p.target > 0 ? el('div', { class: 'sheet-now' }, `${fmtTemp(p.target)}${degUnit()}`, el('small', {}, 'set')) : null);

    const chips = el('div', { class: 'chiprow' });
    const list = el('div', { class: 'donelist' });

    const showMeat = (m) => {
      meat = m;
      chips.querySelectorAll('button').forEach((b) => b.classList.toggle('on', b.dataset.meat === m));
      list.innerHTML = '';
      for (const d of PRESETS[m]) {
        const to = toUser(d.to), pull = toUser(d.to - d.carry), carry = deltaUser(d.carry);
        list.append(el('button', { class: 'done', type: 'button', onclick: () => close({ target: pull, after }) },
          el('div', { class: 'done-main' },
            el('div', { class: 'done-name' }, d.name),
            el('div', { class: 'done-note' }, carry > 0
              ? `${d.note ? d.note + '. ' : ''}Climbs about ${carry}${degUnit()} once it is off the heat`
              : d.note || 'Cook until it probes tender')),
          el('div', { class: 'done-temps' },
            el('div', { class: 'done-pull' }, `${pull}${degUnit()}`),
            carry > 0 ? el('div', { class: 'done-final' }, `ready at ${to}${degUnit()}`) : null)));
      }
      const custom = el('input', { type: 'text', inputmode: 'decimal', placeholder: degUnit(), 'aria-label': 'Custom target' });
      list.append(el('form', { class: 'done custom', onsubmit: (e) => { e.preventDefault(); const v = parseFloat(custom.value); if (!Number.isNaN(v) && v > 0) close({ target: v, after }); } },
        el('div', { class: 'done-main' }, el('div', { class: 'done-name' }, 'Something else'), el('div', { class: 'done-note' }, 'Set the alarm temperature yourself')),
        el('div', { class: 'row', style: 'gap:6px' }, custom, el('button', { class: 'btn sm primary', type: 'submit' }, 'Set'))));
    };

    for (const m of Object.keys(PRESETS)) chips.append(el('button', { class: 'chip-btn', type: 'button', 'data-meat': m, onclick: () => showMeat(m) }, m));
    showMeat(meat);

    return el('div', { class: 'sheet' }, head, chips, list,
      el('div', { class: 'sheet-foot' },
        el('label', {}, 'When it gets there'),
        segmented(AFTER, after, (v) => (after = v))),
      /* dismissive left, committing right: see docs/design-language.md */
      el('div', { class: 'btnrow' },
        el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'),
        p.target > 0 ? el('button', { class: 'btn ghost', type: 'button', onclick: () => close({ target: 0, after: 0 }) }, 'Clear target') : null));
  });
}

/* The steps on the way to the target: a temperature with a name on it that says something once,
 * when it is crossed. It is what MEATER, Chef iQ and Combustion all give you and what actually
 * gets meat cooked properly -- the target is where it comes off, and a step is what you have to be
 * at the grill for before then. Four is enough for flip, wrap, probe-tender and a spare. */
const STEP_PRESETS = [['Flip', 120], ['Wrap', 165], ['Spritz', 150], ['Probe Tender', 198]];

export async function stepsDialog(p) {
  const key = p.label;
  const cur = (PF.settings?.notify?.probe_steps?.[key] || []).map((s) => ({ ...s }));
  return pushScreen((close) => {
    const wrap = el('div', { class: 'sheet-body' });
    const draw = () => {
      wrap.innerHTML = '';
      const inner = el('div', { class: 'ios-list' });
      for (const [i, st] of cur.entries()) {
        inner.append(itemRow({
          icon: 'bell', color: '#bf5af2', title: st.name,
          meta: `${st.temp}${degUnit()}`,
          onclick: async () => {
            const v = await numberDialog(st.name, st.temp, { min: 32, max: 400, step: 5 });
            if (v != null) { st.temp = v; draw(); }
          },
          actions: [iconBtn('trash-2', 'Remove', { class: 'danger', onclick: (e) => { e.stopPropagation(); cur.splice(i, 1); draw(); } })],
        }));
      }
      if (!cur.length) inner.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'No steps. Add one to be told when to flip, wrap or spritz.'));
      const left = STEP_PRESETS.filter(([n]) => !cur.some((s) => s.name === n));
      wrap.append(inner,
        cur.length >= 4 ? null : el('div', { class: 'chiprow' }, left.map(([n, t]) =>
          el('button', { class: 'chip-btn', type: 'button', onclick: () => { cur.push({ name: n, temp: PF.units === 'C' ? Math.round((t - 32) * 5 / 9) : t }); draw(); } }, `+ ${n}`))),
        cur.length >= 4 ? null : addRow('Custom Step', async () => {
          const v = await numberDialog('Alert at', PF.units === 'C' ? 60 : 140, { min: 32, max: 400, step: 5 });
          if (v != null) { cur.push({ name: 'Alert', temp: v }); draw(); }
        }));
    };
    draw();
    return el('div', { class: 'sheet' },
      el('div', { class: 'sheet-head' }, el('div', {}, el('h3', {}, 'Step Alerts'), el('div', { class: 'help' }, p.name))),
      wrap,
      el('div', { class: 'form-actions' },
        actionBtn('cancel', 'Cancel', { size: '', onclick: () => close(undefined) }),
        actionBtn('save', 'Save', { size: '', onclick: () => close(cur) })));
  }, { title: 'Step Alerts', back: p.name }).then(async (steps) => {
    if (!steps) return;
    const all = { ...(PF.settings?.notify?.probe_steps || {}) };
    if (steps.length) all[key] = steps; else delete all[key];
    try { await patchSettings('notify', { probe_steps: all }); toast('Steps saved'); }
    catch (e) { toast(e.message, true); }
  });
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
  const loadRecipes = () => api('/recipes').then((list) => {
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
          iconBtn('pencil', `Edit ${r.name}`, { onclick: (e) => { e.stopPropagation(); edit(r, false); } }),
          iconBtn('play', `Run ${r.name}`, { onclick: (e) => { e.stopPropagation(); run(r); } }),
        ],
      }));
    }
    if (!list.length) recipeList.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' },
      'No recipes yet. A recipe is a list of stages the grill runs for you.'));
    recipeList.append(addRow('Add Recipe', () => edit({ name: '', description: '', steps: [{ mode: 'Startup' }, blankStep(), { mode: 'Shutdown' }] }, true)));
    recipeList.append(transferRow({
      what: 'recipes', filename: 'pifire-recipes',
      fetchDoc: () => api('/recipes/export'),
      confirmText: 'A recipe with the same name as one on the grill replaces it; the rest are added.',
      importDoc: async (doc) => { const r = await api('/recipes/import', { body: doc }); toast(`Imported ${r.imported} recipe${r.imported === 1 ? '' : 's'}${r.replaced ? `, ${r.replaced} replaced` : ''}`); loadRecipes(); },
    }));
  }).catch(() => {});
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
      runCard.replaceChildren(
        el('div', { class: 'row between' },
          el('div', { style: 'min-width:0' },
            el('div', { class: 'run-name' }, rc.name),
            el('div', { class: 'help' }, rc.stage ? `Stage ${rc.stage} of ${rc.stages} · ${rc.step_mode}` : rc.step_mode)),
          rc.waiting ? null : el('div', { class: 'run-left' }, rc.remaining_s >= 0 ? fmtDur(rc.remaining_s) : '')),
        el('div', { class: 'progress' }, el('div', { style: `width:${rc.stages ? (Math.max(0, rc.stage - (rc.waiting ? 0 : 1)) / rc.stages) * 100 : 0}%` })),
        rc.message ? el('div', { class: rc.waiting ? 'notice warn' : 'run-msg' }, rc.message) : null,
        /* A step that wants the lid AND the answer says which half is still missing, rather than
           showing a button that quietly does nothing when it is tapped. */
        rc.waiting && rc.needs_lid
          ? el('div', { class: 'help' }, 'Open the lid, then confirm.')
          : rc.waiting
            ? el('button', { class: 'btn primary block', type: 'button', onclick: () => cmd({ cmd: 'recipe', op: 'next' }) }, 'Done, carry on')
            : null,
        el('div', { class: 'form-actions' },
          el('button', { class: 'btn sm ghost', type: 'button', onclick: async () => {
            if (await confirmDialog('Stop the recipe?', 'The grill keeps running in whatever mode the current step set.', 'Stop recipe', true)) cmd({ cmd: 'recipe', op: 'stop' });
          } }, 'Stop recipe')));
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
