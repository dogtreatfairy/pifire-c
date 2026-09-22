import { PF, el, api, cmd, onStatus, fmtTemp, degUnit, fmtDur, dialog, numberDialog, toast, confirmDialog, segmented } from '../app.js';
import { fmtEta, battIcon } from './probes.js';

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

export async function limitsDialog(p) {
  return dialog((close) => {
    const hi = el('input', { type: 'text', inputmode: 'decimal', value: p.limit_high || '', placeholder: 'off' });
    const lo = el('input', { type: 'text', inputmode: 'decimal', value: p.limit_low || '', placeholder: 'off' });
    return el('form', { onsubmit: (e) => { e.preventDefault(); close({ high: parseFloat(hi.value) || 0, low: parseFloat(lo.value) || 0 }); } },
      el('h3', {}, `${p.name} alarms`),
      el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Get alerted whenever the probe leaves this range (useful for the pit while you sleep). Leave blank to disable.'),
      el('div', { class: 'field inline' }, el('label', {}, `Alarm above (${degUnit()})`), hi),
      el('div', { class: 'field inline' }, el('label', {}, `Alarm below (${degUnit()})`), lo),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
  });
}

async function timerDialog() {
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

function stepSummary(s) {
  const parts = [s.mode + (s.mode === 'Hold' && s.setpoint ? ` ${s.setpoint}${degUnit()}` : '')];
  if (s.timer_min) parts.push(`${s.timer_min} min`);
  if (s.probe && s.probe_temp) parts.push(`${s.probe} ≥ ${s.probe_temp}${degUnit()}`);
  if (s.pause) parts.push('then wait');
  return parts.join(' · ');
}

async function recipeDialog(r) {
  const rec = structuredClone(r || { name: '', description: '', steps: [{ mode: 'Startup' }, { mode: 'Hold', setpoint: 225, timer_min: 0, probe: '', probe_temp: 0, pause: false, message: '' }, { mode: 'Shutdown' }] });
  const probeNames = (PF.status?.probes || []).filter((p) => p.role !== 'Aux').map((p) => [p.label, p.name]);
  return dialog((close) => {
    const name = el('input', { type: 'text', value: rec.name, required: true, placeholder: 'e.g. Pulled pork' });
    const stepsEl = el('div');
    const render = () => {
      stepsEl.innerHTML = '';
      rec.steps.forEach((s, i) => {
        const box = el('fieldset', { class: 'field', style: 'gap:6px' }, el('legend', {}, `Step ${i + 1}`));
        const row = (label, input) => el('div', { class: 'field inline', style: 'padding:4px 0;border:0' }, el('label', {}, label), input);
        box.append(row('Mode', el('select', { onchange: (e) => { s.mode = e.target.value; render(); } }, MODES.map(([v, l]) => el('option', { value: v, selected: s.mode === v }, l)))));
        if (s.mode === 'Hold') box.append(row(`Set point (${degUnit()})`, el('input', { type: 'text', inputmode: 'decimal', value: s.setpoint || '', onchange: (e) => (s.setpoint = parseFloat(e.target.value) || 0) })));
        if (s.mode === 'Hold' || s.mode === 'Smoke') {
          box.append(row('Run for (min, 0 = no timer)', el('input', { type: 'text', inputmode: 'numeric', value: s.timer_min || 0, onchange: (e) => (s.timer_min = parseFloat(e.target.value) || 0) })));
          box.append(row('Until probe', el('select', { onchange: (e) => (s.probe = e.target.value) }, [el('option', { value: '', selected: !s.probe }, '— none —'), ...probeNames.map(([l, n]) => el('option', { value: l, selected: s.probe === l }, n))])));
          box.append(row(`reaches (${degUnit()})`, el('input', { type: 'text', inputmode: 'decimal', value: s.probe_temp || '', onchange: (e) => (s.probe_temp = parseFloat(e.target.value) || 0) })));
          box.append(row('Smoke+', el('input', { type: 'checkbox', checked: !!s.s_plus, onchange: (e) => (s.s_plus = e.target.checked) })));
          box.append(row('Wait for me after', el('input', { type: 'checkbox', checked: !!s.pause, onchange: (e) => (s.pause = e.target.checked) })));
        }
        box.append(row('Message', el('input', { type: 'text', value: s.message || '', placeholder: 'e.g. Wrap the brisket', onchange: (e) => (s.message = e.target.value) })));
        box.append(el('div', { class: 'btnrow' },
          el('button', { class: 'btn sm ghost', type: 'button', disabled: i === 0, onclick: () => { [rec.steps[i - 1], rec.steps[i]] = [rec.steps[i], rec.steps[i - 1]]; render(); } }, '↑'),
          el('button', { class: 'btn sm ghost', type: 'button', disabled: i === rec.steps.length - 1, onclick: () => { [rec.steps[i + 1], rec.steps[i]] = [rec.steps[i], rec.steps[i + 1]]; render(); } }, '↓'),
          el('button', { class: 'btn sm ghost', type: 'button', onclick: () => { rec.steps.splice(i, 1); render(); } }, 'Remove')));
        stepsEl.append(box);
      });
      stepsEl.append(el('button', { class: 'btn sm', type: 'button', onclick: () => { rec.steps.push({ mode: 'Hold', setpoint: 225, timer_min: 0, probe: '', probe_temp: 0, pause: false, message: '' }); render(); } }, 'Add step'));
    };
    render();
    return el('form', { onsubmit: (e) => { e.preventDefault(); rec.name = name.value.trim(); close(rec); } },
      el('h3', {}, rec.id ? 'Edit recipe' : 'New recipe'),
      el('div', { class: 'field' }, el('label', {}, 'Name'), name),
      el('div', { style: 'max-height:55vh;overflow:auto' }, stepsEl),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
  });
}

export function renderCook(view) {
  const probes = el('div');
  const timerCard = el('div', { class: 'card' });
  const alerts = el('div', { class: 'list' });
  const recipeCard = el('div', { class: 'card' });
  const recipeList = el('div', { class: 'list' });
  const showRecipes = PF.settings?.globals?.show_recipes !== false;
  const recipeSection = el('div', { hidden: !showRecipes },
    el('div', { class: 'row between' }, el('h2', {}, 'Recipes'), el('button', { class: 'btn sm', onclick: async () => { const r = await recipeDialog(); if (r) { await api('/recipes', { body: r }).catch((e) => toast(e.message, true)); loadRecipes(); } } }, 'New')),
    el('div', { class: 'card' }, recipeList));
  view.append(el('h2', {}, 'Timer'), timerCard, el('h2', {}, 'Probes'), probes,
    recipeCard, recipeSection,
    el('h2', {}, 'Recent alerts'), el('div', { class: 'card' }, alerts));

  const loadRecipes = () => api('/recipes').then((list) => {
    recipeList.innerHTML = '';
    for (const r of list) {
      recipeList.append(el('div', { class: 'item' },
        el('div', {}, el('div', {}, r.name), el('div', { class: 'meta' }, r.steps.map(stepSummary).join(' → '))),
        el('div', { class: 'btnrow' },
          el('button', { class: 'btn sm primary', onclick: async () => { if (await confirmDialog(`Run ${r.name}?`, 'The recipe takes over the grill from its first step.', 'Run')) cmd({ cmd: 'recipe', op: 'start', id: r.id }); } }, 'Run'),
          el('button', { class: 'btn sm ghost', onclick: async () => { const e = await recipeDialog(r); if (e) { await api('/recipes', { body: e }).catch((x) => toast(x.message, true)); loadRecipes(); } } }, 'Edit'),
          el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Delete recipe?', r.name, 'Delete', true)) { await api(`/recipes/${r.id}/delete`, { body: {} }); loadRecipes(); } } }, 'Delete'))));
    }
    if (!list.length) recipeList.append(el('div', { class: 'muted' }, 'No recipes yet. A recipe is a list of steps: Startup → Hold 225 until the probe hits 165 → Shutdown.'));
  }).catch(() => {});
  if (showRecipes) loadRecipes();

  const loadAlerts = () => api('/alerts?limit=10').then((evs) => {
    alerts.innerHTML = '';
    for (const e of evs.reverse()) alerts.append(el('div', { class: 'item' }, el('div', {}, el('div', {}, e.title), el('div', { class: 'meta' }, `${e.body} · ${new Date(e.ts * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}`))));
    if (!evs.length) alerts.append(el('div', { class: 'muted' }, 'No alerts yet'));
  }).catch(() => {});
  loadAlerts();

  let lastAlertGen = 0;
  const update = (s) => {
    if (!s) return;
    const rc = s.recipe;
    recipeCard.hidden = !rc.active;
    if (rc.active) {
      recipeCard.innerHTML = '';
      recipeCard.append(el('div', { class: 'row between' },
        el('div', {}, el('div', { style: 'font-weight:600' }, `${rc.name} — step ${rc.step + 1} of ${rc.nsteps}`), el('div', { class: 'muted', style: 'font-size:.8rem' }, rc.waiting ? 'Waiting for you' : `${rc.step_mode}${rc.remaining_s >= 0 ? ' · ' + fmtDur(rc.remaining_s) + ' left' : ''}${rc.message ? ' · ' + rc.message : ''}`)),
        el('div', { class: 'btnrow' }, rc.waiting ? el('button', { class: 'btn sm primary', onclick: () => cmd({ cmd: 'recipe', op: 'next' }) }, 'Next') : null, el('button', { class: 'btn sm ghost', onclick: () => cmd({ cmd: 'recipe', op: 'stop' }) }, 'Stop recipe'))));
      recipeCard.append(el('div', { class: 'progress' }, el('div', { style: `width:${((rc.step + (rc.waiting ? 1 : 0)) / rc.nsteps) * 100}%` })));
    }
    const t = s.timer;
    timerCard.innerHTML = '';
    if (t.running) {
      timerCard.append(el('div', { class: 'row between' },
        el('div', {}, el('div', { style: 'font-size:2rem;font-weight:700;font-variant-numeric:tabular-nums' }, fmtDur(t.remaining)), el('div', { class: 'muted', style: 'font-size:.8rem' }, `${t.paused ? 'Paused' : 'Running'} · ${AFTER.find((a) => a[0] === t.after)?.[1]}`)),
        el('div', { class: 'btnrow' }, el('button', { class: 'btn sm', onclick: () => cmd({ cmd: 'timer', op: t.paused ? 'resume' : 'pause' }) }, t.paused ? 'Resume' : 'Pause'), el('button', { class: 'btn sm ghost', onclick: () => cmd({ cmd: 'timer', op: 'cancel' }) }, 'Cancel'))));
      timerCard.append(el('div', { class: 'progress' }, el('div', { style: `width:${Math.max(0, Math.min(100, 100 - (t.remaining / t.duration) * 100))}%` })));
    } else {
      timerCard.append(el('button', { class: 'btn block', onclick: async () => { const r = await timerDialog(); if (r) cmd({ cmd: 'timer', op: 'start', ...r }); } }, 'Set a timer'));
    }

    probes.innerHTML = '';
    /* Grouped by what each probe is for: the pit, then the food, then anything measuring the air
       or the outside. Without the headings a long list of probes is just a wall of numbers. */
    const GROUPS = [['Primary', 'Grill'], ['Food', 'Food'], ['Aux', 'Aux & Ambient']];
    for (const [role, heading] of GROUPS) {
      const members = (s.probes || []).filter((p) => p.enabled && (p.role || 'Food') === role);
      if (!members.length) continue;
      const grid = el('div', { class: 'grid2' });
      probes.append(el('h3', { class: 'probe-group' }, heading), grid);
      for (const p of members) renderProbe(grid, p);
    }

    function renderProbe(into, p) {
      const hit = p.target > 0 && p.valid && p.temp >= p.target;
      const eta = p.target > 0 && p.eta_s >= 0 ? `${fmtEta(p.eta_s)} to target` : '';
      into.append(el('div', { class: `probe ${p.role === 'Primary' ? 'primary' : ''} ${p.valid ? '' : 'invalid'} ${hit ? 'hit' : ''}`, style: 'min-height:130px' },
        el('div', { class: 'name' }, el('span', {}, p.name),
          el('span', {}, p.wireless && p.battery >= 0 ? battIcon(p.battery) : null, p.limit_high || p.limit_low ? '⚠ alarm' : '')),
        el('div', { class: 'temp' }, p.valid ? fmtTemp(p.temp) : '—', el('small', {}, degUnit())),
        el('div', { class: 'tgt' }, p.target > 0 ? `Target ${fmtTemp(p.target)}${degUnit()} · ${AFTER.find((a) => a[0] === p.after)?.[1]}` : 'No target'),
        eta ? el('div', { class: 'tgt eta' }, eta) : null,
        el('div', { class: 'btnrow', style: 'margin-top:8px' },
          el('button', { class: 'btn sm', onclick: async (e) => { e.stopPropagation(); const r = await targetDialog(p); if (r) cmd({ cmd: 'target', label: p.label, ...r }); } }, p.target > 0 ? 'Change' : 'Set target'),
          el('button', { class: 'btn sm ghost', onclick: async (e) => { e.stopPropagation(); const r = await limitsDialog(p); if (r) cmd({ cmd: 'limits', label: p.label, ...r }); } }, 'Alarms'))));
    }
    if (PF.alertGen !== lastAlertGen) { lastAlertGen = PF.alertGen; loadAlerts(); }
  };
  update(PF.status);
  return onStatus(update);
}
