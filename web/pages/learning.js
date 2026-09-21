import { PF, el, api, patchSettings, toast, onStatus, confirmDialog, numberDialog, segmented, degUnit, fmtDur } from '../app.js';

const PHASE_TEXT = {
  starting: 'Starting the grill',
  settling: 'Waiting for the grill to settle',
  testing: 'Measuring the loop',
  next: 'Moving to the next set point',
  finishing: 'Shutting the grill down',
};

// The nine temperatures the single-temperature picker offers, in the user's units.
const PRESETS_F = [180, 200, 225, 250, 275, 325, 375, 425, 450];
const PRESETS_C = [80, 95, 105, 120, 135, 165, 190, 220, 230];

export function renderLearning(view) {
  const tuneCard = el('div', { class: 'card' });
  const ffCard = el('div', { class: 'card' });
  const plantCard = el('div', { class: 'card' });
  const recent = el('div', { class: 'list' });
  view.append(
    el('h2', {}, 'Tuning in Use'),
    el('div', { class: 'card', id: 'learned-note' }, el('div', { class: 'muted' }, 'Tuning in use appears here for the adaptive controller.')),
    el('h2', {}, 'Autotune'), tuneCard,
    el('h2', {}, 'Feed-Forward Model'), ffCard,
    el('h2', {}, 'Plant Estimate'), plantCard,
    el('h2', {}, 'Recent Observations'), el('div', { class: 'card' }, recent));

  let data = null;
  let tune = null;
  let mode = 'full';                                   // 'full' or 'one'
  let pick = PF.units === 'C' ? 105 : 225;             // the single temperature to tune

  async function load() {
    [data, tune] = await Promise.all([api('/learning'), api('/tune')]);
    render();
  }

  // The run moves through phases over hours, so poll it faster than the rest of the page.
  async function loadTune() {
    tune = await api('/tune');
    renderTune();
  }

  async function start(body, label) {
    if (!await confirmDialog(label.title, label.text, 'Start')) return;
    try { await api('/tune/start', { body }); toast('Tuning started'); } catch (e) { toast(e.message, true); }
    loadTune();
  }

  function renderTune() {
    if (!tune) return;
    const s = PF.status;
    tuneCard.innerHTML = '';
    tuneCard.append(el('p', { class: 'muted', style: 'font-size:.85rem' },
      'Autotune starts the grill itself, holds a set point, oscillates the feed a few degrees around it to measure how this grill responds, and shuts down when it is done. Leave the grill empty and let it run.'));

    if (tune.running) {
      const phase = PHASE_TEXT[tune.phase] || tune.message;
      tuneCard.append(
        el('div', { class: 'notice warn' }, `${phase} — ${tune.setpoint}${degUnit()}, ${tune.step} of ${tune.steps}`),
        el('div', { class: 'kv' },
          el('div', {}, tune.full_profile ? 'Full profile' : 'One temperature'), el('div', {}, (tune.setpoints || []).map((v) => `${v}${degUnit()}`).join(' · ')),
          el('div', {}, 'Measured so far'), el('div', {}, `${tune.measured} of ${tune.steps}`),
          el('div', {}, 'Running for'), el('div', {}, fmtDur(tune.elapsed_s))),
        el('div', { class: 'form-actions' }, el('button', {
          class: 'btn sm ghost',
          onclick: async () => { if (await confirmDialog('Stop tuning?', 'The grill shuts down. Temperatures already measured are kept.', 'Stop', true)) { await api('/tune/stop', { body: {} }); loadTune(); } },
        }, 'Stop tuning')));
    } else {
      if (tune.phase === 'done' || tune.phase === 'failed') {
        tuneCard.append(el('p', { class: 'muted', style: 'font-size:.85rem' }, tune.phase === 'done'
          ? `Last run finished. ${tune.measured} temperature${tune.measured === 1 ? '' : 's'} measured.`
          : `Last run stopped. ${tune.message}`));
      }

      tuneCard.append(segmented([['full', 'Full Profile'], ['one', 'One Temperature']], mode, (v) => { mode = v; renderTune(); }));

      // the daemon refuses to start on a grill that is already cooking, so say so rather than fail
      const busy = s && s.mode !== 'Stop' && s.mode !== 'Monitor';
      const units = PF.units === 'C' ? PRESETS_C : PRESETS_F;

      if (mode === 'full') {
        tuneCard.append(
          el('p', { class: 'muted', style: 'font-size:.85rem;margin-top:10px' },
            `Measures ${(tune.profile || []).map((v) => `${v}${degUnit()}`).join(', ') || 'the whole range'} one after another, climbing, and records the weather it measured them in. This becomes the grill's new baseline and replaces everything in the tuning library. It takes a few hours.`),
          el('button', {
            class: 'btn primary block',
            disabled: busy,
            onclick: () => start({ full_profile: true }, {
              title: 'Run a full profile tune?',
              text: 'The grill starts itself, measures every temperature in turn and shuts down when it is finished. This replaces the tuning library with a new baseline. It takes a few hours, so do not cook during the run.',
            }),
          }, busy ? 'Stop the grill to start a run' : 'Run Full Profile'));
      } else {
        tuneCard.append(
          el('p', { class: 'muted', style: 'font-size:.85rem;margin-top:10px' },
            'Measures one temperature and adds it to the tuning library beside what is already there. Useful when you cook at a temperature the profile does not cover, or when one has drifted.'),
          el('div', { class: 'kv' }, el('div', {}, 'Temperature'), el('div', {},
            el('button', {
              class: 'btn sm',
              onclick: async () => {
                const v = await numberDialog('Tune at', pick, { min: units[0], max: units[8], step: 5, presets: units });
                if (v != null) { pick = v; renderTune(); }
              },
            }, `${pick}${degUnit()}`))),
          el('button', {
            class: 'btn primary block',
            disabled: busy,
            onclick: () => start({ setpoints: [pick], full_profile: false }, {
              title: `Tune at ${pick}${degUnit()}?`,
              text: 'The grill starts itself, holds this temperature while it measures, and shuts down when it is finished. It takes about an hour, so do not cook during the run.',
            }),
          }, busy ? 'Stop the grill to start a run' : `Tune at ${pick}${degUnit()}`));
      }
    }

    const anchors = tune.anchors || [];
    tuneCard.append(el('h3', { style: 'margin:16px 0 6px;font-size:.9rem' }, 'Tuning Library'));
    if (anchors.length) {
      tuneCard.append(
        el('div', { class: 'list' }, ...anchors.map((a) => el('div', { class: 'item' }, el('div', {},
          el('div', {}, `${a.setpoint}${degUnit()}`),
          el('div', { class: 'meta' }, [
            `PB ${a.PB}${degUnit()}`, `Ti ${a.Ti} s`, `Td ${a.Td} s`,
            a.ambient != null ? `${a.ambient}${degUnit()} out` : null,
            a.wind_kmh ? `${a.wind_kmh} km/h wind` : null,
            new Date(a.ts * 1000).toLocaleDateString(),
          ].filter(Boolean).join(' · ')))))),
        el('p', { class: 'muted', style: 'font-size:.8rem' }, 'The controller uses the entry for whatever it is holding and blends between them in between. It keeps adjusting from how each cook actually behaves, so these are a starting point that improves with use.'));
    } else {
      tuneCard.append(el('p', { class: 'muted', style: 'font-size:.8rem' }, 'Nothing measured yet. Until a run finishes, the controller uses what it learns from ordinary cooks.'));
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
      el('div', { class: 'form-actions' }, el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Reset learning?', 'All observations, plant estimates and the tuning library are erased.', 'Reset', true)) { await api('/learning/reset', { body: {} }); load(); } } }, 'Reset learning'))].filter(Boolean));

    const p = data.plant;
    plantCard.innerHTML = '';
    plantCard.append(p.valid
      ? el('div', { class: 'kv' }, el('div', {}, 'Gain (K)'), el('div', {}, `${p.K.toFixed(0)}° per unit feed`), el('div', {}, 'Time constant (τ)'), el('div', {}, `${p.tau.toFixed(0)} s`), el('div', {}, 'Dead time (θ)'), el('div', {}, `${p.theta.toFixed(0)} s`), el('div', {}, 'Measured'), el('div', {}, new Date(p.ts * 1000).toLocaleString()))
      : el('p', { class: 'muted' }, 'Measured automatically from the temperature rise of each startup. Not available yet.'));

    if (s?.controller?.note && s.controller.id === 'adaptive') view.querySelector('#learned-note')?.replaceChildren(el('div', { class: 'kv' }, el('div', {}, 'Controller tuning in use'), el('div', {}, s.controller.note)));

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
