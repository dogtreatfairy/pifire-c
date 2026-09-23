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
    /* Erasing gets the red button, because it is the one that cannot be undone. */
    if (!await confirmDialog(label.title, label.text, label.danger ? 'Erase & Start' : 'Start', !!label.danger)) return;
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

      tuneCard.append(segmented([['full', 'Baseline'], ['one', 'One Temperature']], mode, (v) => { mode = v; renderTune(); }));

      // the daemon refuses to start on a grill that is already cooking, so say so rather than fail
      const busy = s && s.mode !== 'Stop' && s.mode !== 'Monitor';
      const units = PF.units === 'C' ? PRESETS_C : PRESETS_F;

      if (mode === 'full') {
        const p = tune.profile || [];
        const nruns = (tune.anchors || []).reduce((m, a) => Math.max(m, a.runs || 1), 0);
        const have = (tune.anchors || []).length > 0;
        tuneCard.append(
          el('p', { class: 'muted', style: 'font-size:.85rem;margin-top:10px' },
            p.length > 1
              ? `Measures ${p.map((v) => `${v}${degUnit()}`).join(', ')} in that order, and records the weather it measured them in. The first is the baseline, measured where the grill has the most room to swing either side of its centre.`
              : `Measures ${p.map((v) => `${v}${degUnit()}`).join('') || 'the baseline'} and records the weather it measured it in. This is where the grill has the most room to swing either side of its centre, which makes it the measurement worth trusting, and the schedule holds outside it — so one honest anchor governs the whole range. About an hour.`),
          /* A run is one afternoon's evidence: that day's wind, that hopper's pellets. Running it
             again should make the answer better rather than throw the previous answer away. */
          el('p', { class: 'muted', style: 'font-size:.85rem' },
            have
              ? `This refines what is already measured rather than replacing it${nruns > 1 ? ` — the library is ${nruns} runs deep` : ''}. Each run moves the numbers less than the last, so the noise of any one afternoon averages out, but never by so little that a grill which has genuinely changed cannot be followed.`
              : 'There is nothing measured yet, so this starts the library.'),
          el('button', {
            class: 'btn primary block',
            disabled: busy,
            onclick: () => start({ full_profile: true }, {
              title: have ? 'Refine the baseline?' : 'Measure the baseline?',
              text: `The grill starts itself, measures ${p.length > 1 ? 'every temperature in turn' : 'the loop'} and shuts down when it is finished. ${have ? 'What it finds refines the tuning library.' : 'What it finds becomes the tuning library.'} Do not cook during the run.`,
            }),
          }, busy ? 'Stop the grill to start a run' : have ? 'Refine Baseline' : 'Measure Baseline'),
          /* Erasing is for a grill that has genuinely changed -- re-gasketed, rebuilt, moved -- and
             is asked for by name rather than being the side effect of running a tune. */
          have ? el('button', {
            class: 'btn ghost block',
            disabled: busy,
            style: 'margin-top:8px',
            onclick: () => start({ full_profile: true, from_scratch: true }, {
              title: 'Erase and start from scratch?',
              text: 'Everything the grill has measured about itself is thrown away before the run begins, and there is no undo. Do this when the grill itself has changed — a new gasket, a rebuild, a move — rather than to take another measurement. Back the library up first if you might want it.',
              danger: true,
            }),
          }, 'Start From Scratch') : null);
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
      /* These are the numbers to keep. They go straight into the controller's own Proportional
         Band, Integral Time and Derivative Time boxes, so a tune never has to be repeated just to
         get back to a known-good setting. */
      const tbl = el('div', { class: 'tunetable' },
        el('div', { class: 'th' }, 'Set point'), el('div', { class: 'th' }, 'PB'), el('div', { class: 'th' }, 'Ti'), el('div', { class: 'th' }, 'Td'), el('div', { class: 'th' }, 'Runs'));
      for (const a of anchors) {
        /* The run count is what makes refinement visible: a number three runs agree on is worth
           more than one measured on a single windy afternoon, and they look identical otherwise. */
        tbl.append(el('div', {}, `${a.setpoint}${degUnit()}`), el('div', {}, `${a.PB}${degUnit()}`), el('div', {}, `${a.Ti} s`), el('div', {}, `${a.Td} s`), el('div', {}, `${a.runs || 1}`));
      }
      const lines = anchors.map((a) => `${a.setpoint}${degUnit()}: PB ${a.PB}${degUnit()}, Ti ${a.Ti} s, Td ${a.Td} s`
        + (a.ambient != null ? ` (measured at ${a.ambient}${degUnit()} out${a.wind_kmh ? `, ${a.wind_kmh} km/h` : ''})` : ''));
      tuneCard.append(tbl,
        el('p', { class: 'muted', style: 'font-size:.8rem' },
          `Measured ${anchors[0].ambient != null ? `at ${anchors[0].ambient}${degUnit()} outside` : 'on this grill'}. These take priority over the Proportional Band, Integral Time and Derivative Time set on this page, which are only the starting point before anything has been measured. Type them in by hand and you get the same tuning without running another autotune.`),
        el('div', { class: 'form-actions' },
          el('button', { class: 'btn sm ghost', onclick: async () => {
            try { await navigator.clipboard.writeText(lines.join('\n')); toast('Copied'); }
            catch { toast(lines.join(' | '), false); }
          } }, 'Copy values'),
          /* A tuning library is hours of the grill's own time and a hopper of pellets, and it
             lives on an SD card. The backup is canonical Celsius and carries the controller and
             the plant model with it, because the numbers mean nothing detached from those. */
          el('button', { class: 'btn sm ghost', onclick: async () => {
            try {
              const doc = await api('/tune/export');
              const name = `pifire-tuning-${new Date().toISOString().slice(0, 10)}.json`;
              const url = URL.createObjectURL(new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' }));
              const a = el('a', { href: url, download: name });
              document.body.append(a); a.click(); a.remove();
              setTimeout(() => URL.revokeObjectURL(url), 10000);
              toast('Backed up');
            } catch (e) { toast(e.message, true); }
          } }, 'Back Up'),
          el('button', { class: 'btn sm ghost', onclick: () => {
            const f = el('input', { type: 'file', accept: 'application/json,.json' });
            f.onchange = async () => {
              const file = f.files?.[0];
              if (!file) return;
              try {
                const doc = JSON.parse(await file.text());
                if (!await confirmDialog('Restore this tuning library?',
                  'Everything the grill has measured is replaced by what is in the file.', 'Restore', true)) return;
                const r = await api('/tune/import', { body: doc });
                toast(`Restored ${r.restored} set point${r.restored === 1 ? '' : 's'}`);
                renderTune();
              } catch (e) { toast(e.message || 'That file is not a tuning backup', true); }
            };
            f.click();
          } }, 'Restore')));

      if (tune.plant) {
        tuneCard.append(el('h3', { style: 'margin:16px 0 6px;font-size:.9rem' }, 'Measured Grill'),
          el('div', { class: 'kv' },
            el('div', {}, 'Gain'), el('div', {}, `${tune.plant.K}${degUnit()} per unit of feed`),
            el('div', {}, 'Time constant'), el('div', {}, `${tune.plant.tau} s`),
            el('div', {}, 'Dead time'), el('div', {}, `${tune.plant.theta} s`)),
          el('p', { class: 'muted', style: 'font-size:.8rem' }, 'How much the pit moves per unit of feed, how quickly it answers, and how long before it starts. The tuning above is derived from these three.'));
      }
    } else {
      tuneCard.append(el('p', { class: 'muted', style: 'font-size:.8rem' }, 'Nothing measured yet. Until a run finishes, the controller uses the Proportional Band, Integral Time and Derivative Time set on this page.'));
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
      // Two different things, named for what survives each one. Clearing is the ordinary one: the
      // grill keeps what it measured on purpose and starts refining it again. Erasing throws the
      // measurements away too, and costs hours and a hopper of pellets to get back.
      el('div', { class: 'form-actions' },
        el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Clear learning?', 'Observations, the fitted grill model and the corrections the controller has settled on are cleared. The tuning library is kept, and the grill starts learning again from it.', 'Clear', true)) { await api('/learning/forget', { body: {} }); load(); } } }, 'Clear learning'),
        el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Erase everything?', 'The tuning library goes too, along with everything learned. Measuring a new baseline takes hours and a hopper of pellets.', 'Erase', true)) { await api('/learning/reset', { body: {} }); load(); } } }, 'Erase everything'))].filter(Boolean));

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
