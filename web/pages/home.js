import { PF, el, cmd, onStatus, fmtTemp, degUnit, fmtDur, numberDialog, dialog, confirmDialog, toggleRow } from '../app.js';

const R = 120, CX = 150, CY = 150, START = 135, SWEEP = 270; // gauge geometry (degrees)
const polar = (deg, r = R) => [CX + r * Math.cos((deg * Math.PI) / 180), CY + r * Math.sin((deg * Math.PI) / 180)];
function arcPath(from, to, r = R) {
  const [x1, y1] = polar(from, r), [x2, y2] = polar(to, r);
  return `M ${x1} ${y1} A ${r} ${r} 0 ${to - from > 180 ? 1 : 0} 1 ${x2} ${y2}`;
}
const gaugeMax = () => (PF.units === 'C' ? 320 : 600);

function buildGauge() {
  const svg = el('svg', { class: 'gauge', viewBox: '0 0 300 300', role: 'img', 'aria-label': 'Pit temperature' });
  svg.innerHTML = `
    <g class="ticks">${Array.from({ length: 28 }, (_, i) => { const a = START + (SWEEP * i) / 27; const [x1, y1] = polar(a, R + 14), [x2, y2] = polar(a, R + (i % 9 === 0 ? 22 : 18)); return `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke-width="2"/>`; }).join('')}</g>
    <path class="track" d="${arcPath(START, START + SWEEP)}" fill="none" stroke-width="14"/>
    <path class="arc" id="g-arc" d="${arcPath(START, START + 0.01)}" fill="none" stroke-width="14"/>
    <line class="sp" id="g-sp" x1="0" y1="0" x2="0" y2="0" stroke-width="4" visibility="hidden"/>
    <text class="label" x="150" y="98" text-anchor="middle" id="g-label">PIT</text>
    <text class="big" x="150" y="160" text-anchor="middle" id="g-temp">—</text>
    <text class="unit" x="150" y="184" text-anchor="middle" id="g-unit"></text>
    <text class="label" x="150" y="214" text-anchor="middle" id="g-sublabel"></text>
    <text class="spv" x="150" y="240" text-anchor="middle" id="g-sub"></text>`;
  return svg;
}

function updateGauge(svg, s) {
  const primary = s.probes.find((p) => p.role === 'Primary');
  const t = primary?.valid ? primary.temp : null;
  const max = gaugeMax();
  const frac = t == null ? 0 : Math.min(1, Math.max(0, t / max));
  svg.querySelector('#g-arc').setAttribute('d', arcPath(START, START + Math.max(0.01, SWEEP * frac)));
  svg.querySelector('#g-temp').textContent = t == null ? '—' : fmtTemp(t);
  svg.querySelector('#g-unit').textContent = degUnit();
  const sp = svg.querySelector('#g-sp');
  const holdLike = s.mode === 'Hold' || (s.mode === 'Startup' && s.next_mode === 'Hold') || s.mode === 'Reignite';
  if (holdLike && s.setpoint > 0) {
    const a = START + SWEEP * Math.min(1, s.setpoint / max);
    const [x1, y1] = polar(a, R - 12), [x2, y2] = polar(a, R + 12);
    sp.setAttribute('x1', x1); sp.setAttribute('y1', y1); sp.setAttribute('x2', x2); sp.setAttribute('y2', y2);
    sp.setAttribute('visibility', 'visible');
  } else sp.setAttribute('visibility', 'hidden');
  const sub = svg.querySelector('#g-sub'), subl = svg.querySelector('#g-sublabel');
  const elapsed = s.mode_elapsed;
  if (s.mode === 'Startup' || s.mode === 'Reignite') {
    const left = s.timers.mode_remaining ?? Math.max(0, s.timers.startup_duration - elapsed);
    const waiting = s.coldstart.active && !s.coldstart.reached;
    if (waiting && left <= 0) { subl.textContent = 'COLD START · WAITING FOR RISE'; sub.textContent = fmtDur(s.coldstart.remaining); }
    else {
      subl.textContent = waiting ? 'COLD START · TIME LEFT' : s.timers.startup_exit_temp > 0 ? `STARTING · OR AT ${fmtTemp(s.timers.startup_exit_temp)}${degUnit()}` : 'STARTING · TIME LEFT';
      sub.textContent = fmtDur(left);
    }
  }
  else if (s.mode === 'Shutdown') { subl.textContent = 'COOLING DOWN · TIME LEFT'; sub.textContent = fmtDur(s.timers.mode_remaining ?? s.timers.shutdown_duration - elapsed); }
  else if (s.mode === 'Prime') { subl.textContent = `PRIMING ${s.timers.prime_amount} g`; sub.textContent = fmtDur(s.timers.mode_remaining ?? s.timers.prime_duration - elapsed); }
  else if (s.mode === 'Hold') { subl.textContent = s.lid_open ? `LID OPEN · ${fmtDur(s.lid_open_remaining)}` : 'SET POINT'; sub.textContent = `${fmtTemp(s.setpoint)}${degUnit()}`; }
  else if (s.mode === 'Smoke') { subl.textContent = 'SMOKING'; sub.textContent = s.s_plus ? 'Smoke+ on' : ''; }
  else { subl.textContent = ''; sub.textContent = ''; }
}

const presetsF = [180, 200, 225, 250, 275, 300, 350, 400];
const presetsC = [80, 95, 107, 120, 135, 150, 175, 205];
const presets = () => (PF.units === 'C' ? presetsC : presetsF);

async function startDialog() {
  const sp = PF.status?.setpoint || (PF.units === 'C' ? 107 : 225);
  return dialog((close) => el('div', {},
    el('h3', {}, 'Start cooking'),
    el('div', { class: 'opts' },
      el('button', { class: 'btn primary', type: 'button', onclick: async () => { close(); const v = await numberDialog('Hold temperature', sp, { presets: presets() }); if (v) cmd({ cmd: 'mode', mode: 'Hold', setpoint: v }); } }, `Startup → Hold`),
      el('button', { class: 'btn', type: 'button', onclick: () => { close(); cmd({ cmd: 'mode', mode: 'Smoke' }); } }, 'Startup → Smoke'),
      el('button', { class: 'btn', type: 'button', onclick: () => { close(); cmd({ cmd: 'mode', mode: 'Monitor' }); } }, 'Monitor only'),
      el('button', { class: 'btn', type: 'button', onclick: async () => { close(); const g = await numberDialog('Prime amount', 10, { min: 1, max: 100, step: 5, unit: ' g', presets: [5, 10, 20, 30] }); if (g) cmd({ cmd: 'prime', amount: g, next: '' }); } }, 'Prime auger')),
    el('button', { class: 'btn ghost block', type: 'button', onclick: () => close() }, 'Cancel')));
}

function actionButtons(s) {
  const b = [];
  const stop = el('button', { class: 'btn danger', onclick: async () => { if (await confirmDialog('Stop the grill?', 'All outputs turn off immediately.', 'Stop', true)) cmd({ cmd: 'stop' }); } }, 'Stop');
  const shutdown = el('button', { class: 'btn', onclick: async () => { if (await confirmDialog('Shut down?', `Feed stops and the fan runs for ${fmtDur(s.timers.shutdown_duration)} to cool the pot.`, 'Shutdown')) cmd({ cmd: 'mode', mode: 'Shutdown' }); } }, 'Shutdown');
  switch (s.mode) {
    case 'Stop': case 'Monitor': b.push(el('button', { class: 'btn primary', onclick: startDialog }, 'Start cooking')); if (s.mode === 'Monitor') b.push(stop); break;
    case 'Smoke': b.push(el('button', { class: 'btn primary', onclick: async () => { const v = await numberDialog('Hold temperature', s.setpoint, { presets: presets() }); if (v) cmd({ cmd: 'mode', mode: 'Hold', setpoint: v }); } }, 'Hold'), shutdown); break;
    case 'Hold': b.push(el('button', { class: 'btn', onclick: () => cmd({ cmd: 'mode', mode: 'Smoke' }) }, 'Smoke'), shutdown); break;
    case 'Startup': case 'Reignite': case 'Prime': b.push(shutdown, stop); break;
    case 'Shutdown': b.push(stop); break;
    case 'Manual': b.push(el('button', { class: 'btn', onclick: () => (location.hash = '#/more/manual') }, 'Manual outputs'), stop); break;
    case 'Error': b.push(el('button', { class: 'btn danger', onclick: () => cmd({ cmd: 'stop' }) }, 'Clear & Stop')); break;
  }
  return b;
}

export function renderHome(view) {
  const gauge = buildGauge();
  const spRow = el('div', { class: 'setpoint-row' });
  const actions = el('div', { class: 'btnrow' });
  const toggles = el('div', { class: 'card tight' });
  const probes = el('div', { class: 'grid2' });
  const chips = el('div', { class: 'chips' });
  const ctrl = el('div', { class: 'kv' });
  view.append(
    el('div', { class: 'card gauge-card' }, gauge, spRow, actions),
    toggles,
    el('h2', {}, 'Probes'), probes,
    el('h2', {}, 'Outputs'), el('div', { class: 'card tight' }, chips),
    el('details', { class: 'card tight' }, el('summary', { class: 'muted' }, 'Controller'), ctrl),
  );

  let lastMode = null, lastFlags = '';
  const update = (s) => {
    if (!s) return;
    updateGauge(gauge, s);
    const holdLike = s.mode === 'Hold' || s.mode === 'Smoke';
    if (s.mode !== lastMode) {
      lastMode = s.mode;
      actions.innerHTML = ''; actions.append(...actionButtons(s));
      spRow.innerHTML = '';
      if (holdLike || s.mode === 'Startup') {
        const val = el('button', { class: 'btn ghost value', onclick: async () => { const v = await numberDialog('Hold temperature', s.setpoint, { presets: presets() }); if (v) cmd({ cmd: 'setpoint', setpoint: v }); } }, '');
        spRow.append(
          el('button', { class: 'btn', 'aria-label': 'Lower set point', onclick: () => cmd({ cmd: 'setpoint', setpoint: PF.status.setpoint - 5 }) }, '−'),
          val,
          el('button', { class: 'btn', 'aria-label': 'Raise set point', onclick: () => cmd({ cmd: 'setpoint', setpoint: PF.status.setpoint + 5 }) }, '+'));
      }
    }
    const val = spRow.querySelector('.value');
    if (val) val.textContent = `${fmtTemp(s.setpoint)}${degUnit()}`;

    const flags = `${s.s_plus}|${s.pwm_control}|${s.lid_open}|${s.mode}|${PF.settings?.platform?.dc_fan}`;
    if (flags !== lastFlags) {
      lastFlags = flags;
      toggles.innerHTML = '';
      if (holdLike) {
        toggles.append(toggleRow('Smoke+', s.s_plus, (v) => cmd({ cmd: 'smoke_plus', enabled: v }), 'Cycle the fan for more smoke'));
        if (PF.settings?.platform?.dc_fan) toggles.append(toggleRow('PWM fan control', s.pwm_control, (v) => cmd({ cmd: 'pwm_control', enabled: v }), 'Vary fan speed with temperature'));
        if (s.mode === 'Hold') toggles.append(toggleRow('Lid open pause', s.lid_open, () => cmd({ cmd: 'lid' }), 'Pause feed while the lid is open'));
      } else toggles.hidden = true;
      toggles.hidden = !holdLike;
    }

    probes.innerHTML = '';
    for (const p of s.probes) {
      if (!p.enabled || p.role === 'Aux') continue;
      const hit = p.target > 0 && p.valid && p.temp >= p.target;
      probes.append(el('div', { class: `probe ${p.role === 'Primary' ? 'primary' : ''} ${p.valid ? '' : 'invalid'} ${hit ? 'hit' : ''}`, onclick: () => (location.hash = '#/cook') },
        el('div', { class: 'name' }, el('span', {}, p.name), el('span', {}, p.role === 'Primary' ? 'PIT' : '')),
        el('div', { class: 'temp' }, p.valid ? fmtTemp(p.temp) : '—', el('small', {}, degUnit())),
        el('div', { class: 'tgt' }, p.target > 0 ? `Target ${fmtTemp(p.target)}${degUnit()}` : (p.valid ? '' : 'No probe'))));
    }

    chips.innerHTML = '';
    const o = s.outputs;
    chips.append(
      el('span', { class: `chip ${o.power ? 'on' : ''}` }, 'Power'),
      el('span', { class: `chip ${o.fan ? 'on' : ''}` }, PF.settings?.platform?.dc_fan && o.fan ? `Fan ${o.fan_pct}%` : 'Fan'),
      el('span', { class: `chip ${o.auger ? 'on hot' : ''}` }, 'Auger'),
      el('span', { class: `chip ${o.igniter ? 'on hot' : ''}` }, 'Igniter'));
    if (s.mode === 'Hold') chips.append(el('span', { class: 'chip on' }, `Feed ${(s.cycle.u_applied * 100).toFixed(0)}%`));

    ctrl.innerHTML = '';
    const c = s.controller;
    for (const [k, v] of [['Controller', c.id], ['u (raw / applied)', `${s.cycle.u_raw.toFixed(2)} / ${s.cycle.u_applied.toFixed(2)}`], ['Cycle', `${s.cycle.cycle_s}s`], ['P / I / D', `${c.p.toFixed(2)} / ${c.i.toFixed(2)} / ${c.d.toFixed(2)}`], ['Error', `${c.error.toFixed(1)}${degUnit()}`], ['Ambient', s.ambient == null ? '—' : `${fmtTemp(s.ambient)}${degUnit()}`]])
      ctrl.append(el('div', {}, k), el('div', {}, v));
  };
  update(PF.status);
  return onStatus(update);
}
