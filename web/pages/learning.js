import { PF, el, api, cmd, patchSettings, toast, onStatus, confirmDialog, degUnit, fmtDur } from '../app.js';

const PHASE_TEXT = {
  starting: 'Starting the grill',
  settling: 'Waiting for the grill to settle',
  testing: 'Measuring the loop',
  next: 'Moving to the next set point',
  finishing: 'Shutting the grill down',
};

export function renderLearning(view) {
  const tuneCard = el('div', { class: 'card' });
  const ffCard = el('div', { class: 'card' });
  const plantCard = el('div', { class: 'card' });
  const atCard = el('div', { class: 'card' });
  const recent = el('div', { class: 'list' });
  view.append(el('h2', {}, 'Tuning in Use'), el('div', { class: 'card', id: 'learned-note' }, el('div', { class: 'muted' }, 'Tuning in use appears here for the adaptive controller.')), el('h2', {}, 'Guided Tuning'), tuneCard, el('h2', {}, 'Feed-Forward Model'), ffCard, el('h2', {}, 'Plant Estimate'), plantCard, el('h2', {}, 'Autotune'), atCard, el('h2', {}, 'Recent Observations'), el('div', { class: 'card' }, recent));

  let data = null;
  let tune = null;
  async function load() {
    [data, tune] = await Promise.all([api('/learning'), api('/tune')]);
    render();
  }

  // The tuner moves through phases over hours, so poll it faster than the rest of the page.
  async function loadTune() {
    tune = await api('/tune');
    renderTune();
  }

  function renderTune() {
    if (!tune) return;
    tuneCard.innerHTML = '';
    const running = tune.running;
    tuneCard.append(el('p', { class: 'muted', style: 'font-size:.85rem' },
      'One run measures the grill at each set point in turn and saves what it finds. The grill loses more heat the hotter it runs, so the settings that hold 180 well are not the settings that hold 450 well. Start it with the grill empty and leave it alone; it takes a few hours and shuts the grill down at the end.'));

    if (running) {
      const phase = PHASE_TEXT[tune.phase] || tune.message;
      tuneCard.append(
        el('div', { class: 'notice warn' }, `${phase} — set point ${tune.setpoint}${degUnit()}, ${tune.step} of ${tune.steps}`),
        el('div', { class: 'kv' },
          el('div', {}, 'Set points'), el('div', {}, (tune.setpoints || []).map((v) => `${v}${degUnit()}`).join(' · ')),
          el('div', {}, 'Measured so far'), el('div', {}, `${tune.measured} of ${tune.steps}`),
          el('div', {}, 'Running for'), el('div', {}, fmtDur(tune.elapsed_s))),
        el('div', { class: 'form-actions' }, el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Stop tuning?', 'The grill shuts down. Set points already measured are kept.', 'Stop', true)) { await api('/tune/stop', { body: {} }); loadTune(); } } }, 'Stop tuning')));
    } else {
      if (tune.phase === 'done' || tune.phase === 'failed') tuneCard.append(el('p', { class: 'muted', style: 'font-size:.85rem' }, tune.phase === 'done' ? `Last run finished. ${tune.measured} set point${tune.measured === 1 ? '' : 's'} measured.` : `Last run stopped. ${tune.message}`));
      /* the daemon refuses to start a run on a grill that is already cooking, so say so here
         rather than let the button fail */
      const busy = PF.status && PF.status.mode !== 'Stop' && PF.status.mode !== 'Monitor';
      tuneCard.append(el('button', {
        class: 'btn primary block',
        disabled: busy,
        onclick: async () => {
          if (!await confirmDialog('Start guided tuning?', 'The grill starts itself, holds each set point in turn, and shuts down when it is finished. This takes a few hours. Do not cook during the run.', 'Start')) return;
          try { await api('/tune/start', { body: {} }); toast('Tuning started'); } catch (e) { toast(e.message, true); }
          loadTune();
        },
      }, busy ? 'Stop the grill to start a run' : 'Start guided tuning'));
    }

    const anchors = tune.anchors || [];
    if (anchors.length) {
      tuneCard.append(el('h3', { style: 'margin:16px 0 6px;font-size:.9rem' }, 'Measured Set Points'),
        el('div', { class: 'list' }, ...anchors.map((a) => el('div', { class: 'item' }, el('div', {},
          el('div', {}, `${a.setpoint}${degUnit()}`),
          el('div', { class: 'meta' }, `PB ${a.PB}${degUnit()} · Ti ${a.Ti} s · Td ${a.Td} s · ${new Date(a.ts * 1000).toLocaleDateString()}`))))),
        el('p', { class: 'muted', style: 'font-size:.8rem' }, 'The controller follows these while holding and blends between them at set points in between.'));
    } else if (!running) {
      tuneCard.append(el('p', { class: 'muted', style: 'font-size:.8rem;margin-top:10px' }, 'Nothing measured yet. Until a run finishes, the controller uses what it learns from ordinary cooks.'));
    }
  }
  function render() {
    renderTune();
    if (!data) return;
    const s = PF.status;
    const f = data.feedforward;
    ffCard.innerHTML = '';
    ffCard.append(...[
      el('label', { class: 'toggle' }, el('div', {}, el('div', {}, 'Learn from cooks'), el('div', { class: 'help muted', style: 'font-size:.76rem' }, 'Record steady-state feed at each set point and ambient temperature')),
        el('span', { class: 'switch' }, el('input', { type: 'checkbox', checked: data.enabled, onchange: async (e) => { await patchSettings('learning', { enabled: e.target.checked }); load(); } }), el('span'))),
      el('p', { class: 'muted', style: 'font-size:.85rem' }, `Model: feed = ${f.a.toFixed(3)} + ${f.b_per_degC.toFixed(4)} × (set point − ambient, °C). Fitted from ${f.observations} observation${f.observations === 1 ? '' : 's'}${f.observations ? `, rms error ${f.rms.toFixed(3)}` : ' (using the built-in prior until real cooks accumulate)'}.`),
      el('div', { class: 'kv' }, ...f.examples.flatMap((e) => [el('div', {}, `Hold ${e.setpoint}${degUnit()} at ${f.example_ambient}${degUnit()} ambient`), el('div', {}, `${(e.u * 100).toFixed(0)}% feed`)])),
      s?.mode === 'Hold' ? el('p', { class: 'muted', style: 'font-size:.85rem' }, `Right now: feed-forward ${(s.cycle.u_ff * 100).toFixed(0)}%, applied ${(s.cycle.u_applied * 100).toFixed(0)}%.`) : null,
      el('div', { class: 'form-actions' }, el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Reset learning?', 'All observations, plant estimates, autotune results and the set points guided tuning measured are erased.', 'Reset', true)) { await api('/learning/reset', { body: {} }); load(); } } }, 'Reset learning'))].filter(Boolean));

    const p = data.plant;
    plantCard.innerHTML = '';
    plantCard.append(p.valid
      ? el('div', { class: 'kv' }, el('div', {}, 'Gain (K)'), el('div', {}, `${p.K.toFixed(0)}° per unit feed`), el('div', {}, 'Time constant (τ)'), el('div', {}, `${p.tau.toFixed(0)} s`), el('div', {}, 'Dead time (θ)'), el('div', {}, `${p.theta.toFixed(0)} s`), el('div', {}, 'Measured'), el('div', {}, new Date(p.ts * 1000).toLocaleString()))
      : el('p', { class: 'muted' }, 'Measured automatically from the temperature rise of each startup. Not available yet.'));

    if (s?.controller?.note && s.controller.id === 'adaptive') view.querySelector('#learned-note')?.replaceChildren(el('div', { class: 'kv' }, el('div', {}, 'Controller tuning in use'), el('div', {}, s.controller.note)));
    const a = data.autotune;
    atCard.innerHTML = '';
    atCard.append(el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Autotune oscillates the feed gently around the set point for 15–40 minutes while holding, measures the response, and suggests PB/Ti/Td. Run it with the grill at temperature and no food inside.'));
    if (s?.autotune?.active) atCard.append(el('div', { class: 'notice warn' }, `Autotune running — ${s.autotune.crossings} of 7 crossings`), el('button', { class: 'btn block', onclick: () => cmd({ cmd: 'autotune', start: false }) }, 'Stop autotune'));
    else atCard.append(el('button', { class: 'btn primary block', disabled: !(s?.mode === 'Hold' && s?.target_reached), onclick: () => cmd({ cmd: 'autotune', start: true }) }, s?.mode === 'Hold' ? (s.target_reached ? 'Run autotune' : 'Waiting for the pit to reach the set point…') : 'Hold at a set point to enable'));
    if (a.valid) atCard.append(el('div', { class: 'kv', style: 'margin-top:12px' }, el('div', {}, 'Ultimate gain / period'), el('div', {}, `${a.Ku.toFixed(3)} / ${a.Pu.toFixed(0)} s`), el('div', {}, 'Oscillation'), el('div', {}, `±${a.amplitude}${degUnit()}`), el('div', {}, 'Suggested PB / Ti / Td'), el('div', {}, `${a.PB}${degUnit()} / ${a.Ti} s / ${a.Td} s`), el('div', {}, 'Measured'), el('div', {}, new Date(a.ts * 1000).toLocaleString())),
      el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', onclick: async () => { if (await confirmDialog('Apply tuning?', `Writes PB/Ti/Td into the "${s?.controller?.id}" controller settings.`, 'Apply')) cmd({ cmd: 'apply_tuning' }); } }, 'Apply to controller')));

    recent.innerHTML = '';
    for (const o of data.recent) recent.append(el('div', { class: 'item' }, el('div', {}, el('div', {}, `Hold ${o.setpoint}${degUnit()} · ambient ${o.ambient}${degUnit()} · feed ${(o.u * 100).toFixed(0)}%`), el('div', { class: 'meta' }, `${o.controller} · ${new Date(o.ts * 1000).toLocaleString()}`))));
    if (!data.recent.length) recent.append(el('div', { class: 'muted' }, 'Observations appear after a few minutes of steady holding.'));
  }
  load().catch((e) => toast(e.message, true));
  const off = onStatus(() => render());
  const t = setInterval(load, 30000);
  const tt = setInterval(() => loadTune().catch(() => {}), 5000);
  return () => { off(); clearInterval(t); clearInterval(tt); };
}
