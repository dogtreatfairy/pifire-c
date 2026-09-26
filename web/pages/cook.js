import { PF, el, api, cmd, onStatus, fmtTemp, degUnit, fmtDur, dialog, pushScreen, numberDialog, toast, confirmDialog, segmented, actionBtn, itemRow, iconBtn, addRow, patchSettings, screenActions } from '../app.js';
import { fmtEta } from './probes.js';
/* The same condition cards, rows and picker the notification editor is made of. A step ending is
   the same kind of question -- "when is this true" -- and has to be asked in the same shapes.
   See web/conditions.js and docs/design-language.md. */
import { catalogue, condNode, describeNode } from '../conditions.js';

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

/* One line saying what a step does, for the row in the list and for the header of the card when it
   is folded shut -- the same rule the notification editor follows: a card you cannot read without
   opening it is a card that has to be opened. The ending is described by the shared code, so a
   step and a notification say the same condition in the same words. */
function stepSummary(s) {
  const parts = [s.mode + (s.mode === 'Hold' && s.setpoint ? ` ${s.setpoint}${degUnit()}` : '')];
  const said = s.ends ? describeNode(s.ends, 'step', true) : '';
  if (said) parts.push(said);
  return parts.join(' \u00b7 ');
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

    const stepCard = (s, i, redraw) => {
      const det = el('details', { class: 'fold cond-card', open: false });
      const title = el('span', { class: 'cc-title' });
      const head = el('summary', { class: 'cc-head' },
        el('span', { class: 'cc-glyph' }, String(i + 1)), title,
        iconBtn('trash-2', 'Remove this step', { class: 'danger cc-del',
          onclick: (e) => { e.preventDefault(); e.stopPropagation(); rec.steps.splice(i, 1); touched(); redraw(); } }));
      const inner = el('div', { class: 'cc-body' });
      det.append(head, inner);
      const retitle = () => { title.textContent = stepSummary(s) || 'New step'; };
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
          s.ends ||= blankEnds();
          inner.append(el('div', { class: 'field' }, el('label', {}, 'Ends When'),
            condNode(s.ends, 'step', () => changed(), null, 0)));
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
      rec.steps.forEach((s, i) => steps.append(stepCard(s, i, draw)));
      if (!rec.steps.length) steps.append(el('div', { class: 'muted', style: 'padding:6px 2px' }, 'No steps yet.'));
      steps.append(el('div', { class: 'cc-add' }, actionBtn('add', 'Add step', {
        onclick: () => { rec.steps.push(blankStep()); touched(); draw(); } })));
      body.append(steps);
    };
    draw();

    const dismiss = async () => {
      if (JSON.stringify(rec) !== JSON.stringify({ ...rec0, units: PF.units }) &&
          !await confirmDialog('Discard changes?', rec.name || '', 'Discard', true)) return;
      close(undefined);
    };
    wrap.append(el('div', { class: 'sheet-body' }, body),
      screenActions({
        onDelete: isNew ? null : () => close('delete'),
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
          if (issues.some((i) => i.code === 'no_shutdown')
              && !await confirmDialog('Leave the grill running?',
                   'This recipe does not end with a Shutdown step. When it finishes the grill will still be lit, and PiFire will ask you whether to shut it down.',
                   'Save Anyway')) { draw(); return; }
          close(rec);
        },
        dirty: isNew,
      }));
    setTimeout(() => { ready = true; }, 0);
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
    const first = r.steps?.[0];
    if (!await confirmDialog(`Run ${r.name}?`, first ? `Starts with ${stepSummary(first)}.` : '', 'Run')) return;
    cmd({ cmd: 'recipe', op: 'start', id: r.id });
  };

  const loadRecipes = () => api('/recipes').then((list) => {
    recipeList.innerHTML = '';
    /* The row shows what the recipe is and runs it; everything you set once -- the steps, the
       temperatures, the messages -- is behind it. */
    for (const r of list) {
      recipeList.append(itemRow({
        icon: 'book-open',
        title: r.name,
        meta: r.description || (r.steps || []).map(stepSummary).join(' → '),
        onclick: () => edit(r, false),
        actions: [el('button', { class: 'btn sm primary', type: 'button', onclick: (e) => { e.stopPropagation(); run(r); } }, 'Run')],
      }));
    }
    if (!list.length) recipeList.append(el('div', { class: 'muted', style: 'padding:10px 2px' },
      'No recipes yet. A recipe is a list of steps the grill runs for you.'));
    recipeList.append(addRow('Add Recipe', () => edit({ name: '', description: '', steps: [{ mode: 'Startup' }, blankStep(), { mode: 'Shutdown' }] }, true)));
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
            el('div', { class: 'help' }, `Step ${rc.step + 1} of ${rc.nsteps} · ${rc.step_mode}`)),
          rc.waiting ? null : el('div', { class: 'run-left' }, rc.remaining_s >= 0 ? fmtDur(rc.remaining_s) : '')),
        el('div', { class: 'progress' }, el('div', { style: `width:${((rc.step + (rc.waiting ? 1 : 0)) / rc.nsteps) * 100}%` })),
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
