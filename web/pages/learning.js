import { PF, el, api, patchSettings, toast, onStatus, confirmDialog, numberDialog, dialog, degUnit, fmtDur, actionBtn, dataTable, transferRow } from '../app.js';

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

/* The page this fills is a short list of sections, so the parts are handed to whoever is laying it
   out rather than appended in one stream: `slots.tuning` gets everything about measuring the grill,
   `slots.learning` everything about what it works out for itself, and `slots.note` the one line
   saying which tuning is in force. Without slots they all go into the view in that order. */
export function renderLearning(view, slots = {}) {
  const tuneCard = el('div', { class: 'card' });
  const ffCard = el('div', { class: 'card' });
  const recent = el('div', { class: 'list' });
  const note = el('div', { class: 'card', id: 'learned-note' }, el('div', { class: 'muted' }, 'Reading the tuning in use…'));
  const tuningInto = slots.tuning || view;
  const learningInto = slots.learning || view;
  if (slots.note) slots.note.append(note);
  else view.append(el('h2', {}, 'Tuning in Use'), note);
  if (!slots.tuning) tuningInto.append(el('h2', {}, 'Autotune'));
  tuningInto.append(tuneCard);
  if (!slots.learning) learningInto.append(el('h2', {}, 'Feed-Forward Model'));
  /* No plant estimate here: the three numbers measured from the grill belong with the rest of the
     measuring, under Auto Tuning, and printing them twice would only invite them to disagree. */
  learningInto.append(ffCard,
    el('h3', { class: 'subhead' }, 'Recent Observations'), el('div', { class: 'card' }, recent));

  let data = null;
  let tune = null;
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

  /* The numbers behind one measured temperature, for when you do want them. */
  const anchorSheet = (a) => dialog((close) => el('div', { class: 'sheet' },
    el('div', { class: 'sheet-head' },
      el('div', {}, el('h3', {}, `${a.setpoint}${degUnit()}`),
        el('div', { class: 'help' }, `${a.runs || 1} run${(a.runs || 1) === 1 ? '' : 's'}`)),
      a.ambient != null ? el('div', { class: 'sheet-now' }, `${a.ambient}${degUnit()}`, el('small', {}, 'ambient')) : null),
    el('div', { class: 'sheet-body' },
      el('h2', {}, 'Tuning'),
      el('div', { class: 'kv' },
        el('div', {}, 'Proportional Band'), el('div', {}, `${a.PB}${degUnit()}`),
        el('div', {}, 'Integral Time'), el('div', {}, `${a.Ti} s`),
        el('div', {}, 'Derivative Time'), el('div', {}, `${a.Td} s`)),
      el('h2', {}, 'Measurement'),
      el('div', { class: 'kv' },
        el('div', {}, 'Ultimate gain'), el('div', {}, a.Ku ? a.Ku.toFixed(4) : '\u2014'),
        el('div', {}, 'Period'), el('div', {}, a.Pu ? `${Math.round(a.Pu)} s` : '\u2014'),
        el('div', {}, 'Wind'), el('div', {}, a.wind_kmh ? `${a.wind_kmh} km/h` : '\u2014')),
      a.K ? el('h2', {}, 'Grill Model') : null,
      a.K ? el('div', { class: 'kv' },
        el('div', {}, 'Gain'), el('div', {}, `${a.K}${degUnit()} / full feed`),
        el('div', {}, 'Time Constant'), el('div', {}, `${a.tau} s`),
        el('div', {}, 'Dead Time'), el('div', {}, `${a.theta} s`)) : null,
      a.K ? el('div', { class: 'help' }, 'Used to predict heat already on its way.') : null),
    el('div', { class: 'form-actions' },
      actionBtn('cancel', 'Close', { size: '', onclick: () => close() }))));

  function renderTune() {
    if (!tune) return;
    const s = PF.status;
    tuneCard.innerHTML = '';
    tuneCard.append(el('p', { class: 'help' }, 'Runs the grill empty. About an hour per temperature.'));

    if (tune.running) {
      const phase = PHASE_TEXT[tune.phase] || tune.message;
      tuneCard.append(
        el('div', { class: 'notice warn' }, `${phase} — ${tune.setpoint}${degUnit()}, ${tune.step}/${tune.steps}`),
        el('div', { class: 'kv' },
          el('div', {}, 'Set points'), el('div', {}, (tune.setpoints || []).map((v) => `${v}${degUnit()}`).join(' · ')),
          el('div', {}, 'Measured'), el('div', {}, `${tune.measured}/${tune.steps}`),
          el('div', {}, 'Elapsed'), el('div', {}, fmtDur(tune.elapsed_s))),
        el('div', { class: 'form-actions' }, el('button', {
          class: 'btn sm ghost',
          onclick: async () => { if (await confirmDialog('Stop tuning?', 'Grill shuts down. Measurements already taken are kept.', 'Stop', true)) { await api('/tune/stop', { body: {} }); loadTune(); } },
        }, 'Stop')));
    } else {
      if (tune.phase === 'done' || tune.phase === 'failed') {
        tuneCard.append(el('p', { class: 'help' }, tune.phase === 'done'
          ? `Last run: ${tune.measured} measured.`
          : `Last run stopped. ${tune.message}`));
      }

      // the daemon refuses to start on a grill that is already cooking, so say so rather than fail
      const busy = s && s.mode !== 'Stop' && s.mode !== 'Monitor';
      const units = PF.units === 'C' ? PRESETS_C : PRESETS_F;
      const p = tune.profile || [];
      const have = (tune.anchors || []).length > 0;
      const list = p.map((v) => `${v}${degUnit()}`).join(' · ');

      /* Two actions, two buttons. This was a segmented switcher that changed which single button
         you were looking at, which hid one of the two things you might want and needed a paragraph
         to explain which mode you were in. A choice between two actions is one button each. */
      tuneCard.append(
        el('button', {
          class: 'btn primary block', disabled: busy,
          onclick: () => start({ full_profile: true }, {
            title: have ? 'Refine baseline?' : 'Tune baseline?',
            text: `${list || 'Baseline'}. Grill starts itself and shuts down when done.`,
          }),
        }, busy ? 'Stop the grill first' : have ? 'Refine Baseline' : 'Tune Baseline'),
        el('p', { class: 'help' }, have ? `Refines ${list}. Each run moves the numbers less than the last.` : `Baseline: ${list}`),

        el('div', { class: 'kv' }, el('div', {}, 'Temperature'), el('div', {},
          el('button', {
            class: 'btn sm',
            onclick: async () => {
              const v = await numberDialog('Tune at', pick, { min: units[0], max: units[8], step: 5, presets: units });
              if (v != null) { pick = v; renderTune(); }
            },
          }, `${pick}${degUnit()}`))),
        el('button', {
          class: 'btn block', disabled: busy, style: 'margin-top:8px',
          onclick: () => start({ setpoints: [pick], full_profile: false }, {
            title: `Tune at ${pick}${degUnit()}?`,
            text: 'Grill starts itself and shuts down when done.',
          }),
        }, busy ? 'Stop the grill first' : `Tune at ${pick}${degUnit()}`));

      /* Erasing is for a grill that has genuinely changed -- re-gasketed, rebuilt, moved -- and is
         asked for by name rather than being the side effect of running a tune. */
      if (have) {
        tuneCard.append(el('button', {
          class: 'btn danger block', disabled: busy, style: 'margin-top:8px',
          onclick: () => start({ full_profile: true, from_scratch: true }, {
            title: 'Erase and start over?',
            text: 'Deletes every measurement, then runs the baseline. No undo.',
            danger: true,
          }),
        }, 'Start From Scratch'));
      }
    }

    const anchors = tune.anchors || [];
    tuneCard.append(el('h3', { class: 'subhead' }, 'Tuning Library'));
    if (anchors.length) {
      /* These are the numbers to keep. They go straight into the controller's own Proportional
         Band, Integral Time and Derivative Time boxes, so a tune never has to be repeated just to
         get back to a known-good setting. */
      /* The run count is what makes refinement visible: a number three runs agree on is worth more
         than one measured on a single windy afternoon, and they look identical otherwise. */
      const tbl = dataTable(
        /* Which temperatures are measured and how well, not the numbers themselves. The three
           numbers matter when you want to write them down or type them into another grill, which is
           occasionally; what you look at is whether 250 is measured and how many runs agree.
           Tapping a row opens the numbers and the weather they were measured in. */
        [{ key: 'sp', label: 'Set point' }, { key: 'runs', label: 'Runs' }, { key: 'amb', label: 'Measured at' }],
        anchors.map((a) => ({
          sp: `${a.setpoint}${degUnit()}`,
          runs: String(a.runs || 1),
          amb: a.ambient != null ? `${a.ambient}${degUnit()}${a.wind_kmh ? ` \u00b7 ${a.wind_kmh} km/h` : ''}` : '\u2014',
          _onclick: () => anchorSheet(a),
        })));
      const lines = anchors.map((a) => `${a.setpoint}${degUnit()}: PB ${a.PB}${degUnit()}, Ti ${a.Ti} s, Td ${a.Td} s`
        + (a.ambient != null ? ` (measured at ${a.ambient}${degUnit()} out${a.wind_kmh ? `, ${a.wind_kmh} km/h` : ''})` : ''));
      /* The library is a model of the grill across its range, not a list of separate answers, and
         saying so is the difference between "four tunes" and "a tuned grill". It also answers the
         question the table itself raises: what happens at a temperature that is not in it. */
      const lo = anchors[0].setpoint, hi = anchors[anchors.length - 1].setpoint;
      const model = anchors.length > 1
        ? `Covers ${lo}–${hi}${degUnit()}. In between is interpolated, outside is held flat. A run changes only its own temperature.`
        : `One measurement, used at every temperature. Tune a second, further away, to build a range.`;
      tuneCard.append(tbl,
        el('p', { class: 'help' }, model),
        el('p', { class: 'help' },
          `${anchors[0].ambient != null ? `Measured at ${anchors[0].ambient}${degUnit()} ambient. ` : ''}These override the values typed above.`),
        el('div', { class: 'form-actions' },
          el('button', { class: 'btn sm ghost', onclick: async () => {
            try { await navigator.clipboard.writeText(lines.join('\n')); toast('Copied'); }
            catch { toast(lines.join(' | '), false); }
          } }, 'Copy values'),
          /* A tuning library is hours of the grill's own time and a hopper of pellets, and it
             lives on an SD card. The file is canonical Celsius and carries the controller and
             the plant model with it, because the numbers mean nothing detached from those. */
          ),
        transferRow({
          what: 'the tuning library', filename: 'pifire-tuning',
          fetchDoc: () => api('/tune/export'),
          confirmText: 'Replaces every measurement on the grill with the file\u2019s.',
          importDoc: async (doc) => { const r = await api('/tune/import', { body: doc }); toast(`Imported ${r.restored} set point${r.restored === 1 ? '' : 's'}`); renderTune(); },
        }));

    } else {
      tuneCard.append(el('p', { class: 'help' }, 'Nothing measured. Running on the startup fit, or the values typed above.'));
    }

    /* The grill model is measured whether or not a tune was ever run -- every startup rise fits one
       -- so it is shown, and can be cleared, on its own. */
    if (tune.plant) {
      tuneCard.append(el('h3', { class: 'subhead' }, 'Measured Grill'),
        el('div', { class: 'kv' },
          el('div', {}, 'Gain'), el('div', {}, `${tune.plant.K}${degUnit()} per unit of feed`),
          el('div', {}, 'Time constant'), el('div', {}, `${tune.plant.tau} s`),
          el('div', {}, 'Dead time'), el('div', {}, `${tune.plant.theta} s`)),
        el('p', { class: 'help' }, 'Fitted from each startup rise. The tuning above derives from these.'));
    }

    /* The way back to the values you typed. It is a row of its own, in the section whose contents
       it removes, rather than a second clearing button next to the one under Learning. */
    if (anchors.length || tune.plant) {
      tuneCard.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn sm ghost', onclick: async () => {
        if (!await confirmDialog('Clear the measured tuning?',
          'Deletes the library, the last autotune and the grill model. Reverts to the values typed above. No undo.', 'Clear', true)) return;
        try { await api('/tune/clear', { body: {} }); toast('Back to the typed values'); setTimeout(() => load().catch(() => {}), 400); }
        catch (e) { toast(e.message, true); }
      } }, 'Clear Autotune')));
    }
  }

  function render() {
    renderTune();
    if (!data) return;
    const s = PF.status;
    const f = data.feedforward;
    ffCard.innerHTML = '';
    ffCard.append(...[
      // The switch lives in the Learning section above, once. This card only reports what it found.
      data.enabled ? null : el('div', { class: 'notice warn' }, 'Learning off. Not updating.'),
      el('p', { class: 'help' }, `feed = ${f.a.toFixed(3)} + ${f.b_per_degC.toFixed(4)} × (set point − ambient °C) · ${f.observations} obs${f.observations ? ` · rms ${f.rms.toFixed(3)}` : ' · prior'}`),
      el('div', { class: 'kv' }, ...f.examples.flatMap((e) => [el('div', {}, `Hold ${e.setpoint}${degUnit()} at ${f.example_ambient}${degUnit()} ambient`), el('div', {}, `${(e.u * 100).toFixed(0)}% feed`)])),
      s?.mode === 'Hold' ? el('p', { class: 'help' }, `Now: ff ${(s.cycle.u_ff * 100).toFixed(0)}% · applied ${(s.cycle.u_applied * 100).toFixed(0)}%`) : null,
      // Clearing what the grill taught itself belongs here, with the rest of the learning. The
      // other clearing -- throwing the measurements away and going back to the typed values -- sits
      // under Auto Tuning, beside the library it removes.
      el('div', { class: 'form-actions' },
        actionBtn('delete', 'Clear Learning', { onclick: async () => { if (await confirmDialog('Clear learning?', 'Clears the observations and the controller\'s own corrections. Measured tuning is kept.', 'Clear', true)) { await api('/learning/forget', { body: {} }); load(); } } }))].filter(Boolean));

    /* What governs the set point right now, sent with every status rather than left over from the
       last cycle the controller ran: between cooks the controller's own note is whatever was in
       force during the last one, which straight after a tuning run is the one moment it is
       certainly wrong. Where it came from is said in words, because "which of these three numbers
       am I actually running" is the whole question this card exists to answer. */
    const SRC = { tuned: 'Autotune', learned: 'Learning', typed: 'Typed' };
    const t = s?.controller?.tuning;
    if (t) {
      note.replaceChildren(
        el('h3', { class: 'subhead' }, 'Tuning In Use'),
        el('div', { class: 'kv' },
          el('div', {}, 'Proportional Band'), el('div', {}, `${t.PB}${degUnit()}`),
          el('div', {}, 'Integral Time'), el('div', {}, `${t.Ti} s`),
          el('div', {}, 'Derivative Time'), el('div', {}, `${t.Td} s`),
          el('div', {}, 'From'), el('div', {}, SRC[t.src] || t.src)),
        el('p', { class: 'help' },
          s?.mode === 'Hold' ? `In use, holding ${s.setpoint}${degUnit()}.` : 'Would be used at the next hold.'));
    }

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
