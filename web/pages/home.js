import { PF, el, cmd, onStatus, fmtTemp, degUnit, fmtDur, numberDialog, dialog, confirmDialog } from '../app.js';

// The Home screen mirrors the grill's display: the primary probe as the hero, the set point or mode
// statement under it, one clock, then the food probes as plain rows.

const presetsF = [180, 200, 225, 250, 275, 300, 350, 400];
const presetsC = [80, 95, 107, 120, 135, 150, 175, 205];
const presets = () => (PF.units === 'C' ? presetsC : presetsF);
const timed = (m) => m === 'Startup' || m === 'Reignite' || m === 'Shutdown' || m === 'Prime';
const modeColor = { Hold: 'var(--ok)', Smoke: 'var(--ok)', Error: 'var(--danger)', Shutdown: 'var(--info)', Stop: 'var(--muted)', Monitor: 'var(--muted)' };

function heroText(s) {
  const u = degUnit();
  const hop = s.hopper_pct >= 0 ? `Hopper ${s.hopper_pct}%` : '';
  const dot = ' · ';
  let clock = '';
  if (timed(s.mode)) {
    const waiting = (s.mode === 'Startup' || s.mode === 'Reignite') && s.coldstart.active && !s.coldstart.reached && s.timers.mode_remaining <= 0;
    clock = `${fmtDur(waiting ? s.coldstart.remaining : s.timers.mode_remaining)} left`;
  } else if (s.cook_elapsed > 0) clock = fmtDur(s.cook_elapsed);
  let line1 = '', cls = '', line2 = '';
  switch (s.mode) {
    case 'Hold': line1 = `Set ${fmtTemp(s.setpoint)}${u}`; line2 = s.lid_open ? `Lid open${dot}auger paused` : hop; break;
    case 'Smoke': line1 = s.s_plus ? 'Smoke+ on' : 'Smoke'; cls = 'ok'; line2 = [`P${PF.settings?.cycle_data?.PMode ?? ''}`, hop].filter(Boolean).join(dot); break;
    case 'Startup': case 'Reignite':
      line1 = s.next_mode === 'Hold' && s.setpoint > 0 ? `Then hold ${fmtTemp(s.setpoint)}${u}` : 'Then smoke';
      line2 = s.coldstart.active && !s.coldstart.reached ? `Cold start${dot}waiting for rise` : s.timers.startup_exit_temp > 0 ? `Igniting${dot}exits at ${fmtTemp(s.timers.startup_exit_temp)}${u}` : ['Igniting', hop].filter(Boolean).join(dot);
      break;
    case 'Shutdown': line1 = 'Cooling down'; cls = 'info'; line2 = 'Fan on, auger off'; break;
    case 'Prime': line1 = `Priming ${s.timers.prime_amount} g`; line2 = 'Auger running'; break;
    case 'Error': line1 = s.safety.error_code.replace(/_/g, ' '); cls = 'danger'; line2 = 'Outputs off'; break;
    case 'Manual': line1 = 'Manual outputs'; cls = 'warn'; line2 = hop; break;
    case 'Monitor': line1 = 'Monitoring'; cls = 'muted'; line2 = 'Outputs off'; break;
    default: line1 = 'Ready'; cls = 'muted'; line2 = hop;
  }
  return { clock, line1, cls, line2 };
}

async function startDialog() {
  const sp = PF.status?.setpoint || (PF.units === 'C' ? 107 : 225);
  return dialog((close) => el('div', {},
    el('h3', {}, 'Start cooking'),
    el('div', { class: 'opts' },
      el('button', { class: 'btn primary', type: 'button', onclick: async () => { close(); const v = await numberDialog('Hold temperature', sp, { presets: presets() }); if (v) cmd({ cmd: 'mode', mode: 'Hold', setpoint: v }); } }, 'Startup → Hold'),
      el('button', { class: 'btn', type: 'button', onclick: () => { close(); cmd({ cmd: 'mode', mode: 'Smoke' }); } }, 'Startup → Smoke'),
      el('button', { class: 'btn', type: 'button', onclick: () => { close(); cmd({ cmd: 'mode', mode: 'Monitor' }); } }, 'Monitor only'),
      el('button', { class: 'btn', type: 'button', onclick: async () => { close(); const g = await numberDialog('Prime amount', 10, { min: 1, max: 100, step: 5, unit: ' g', presets: [5, 10, 20, 30] }); if (g) cmd({ cmd: 'prime', amount: g, next: '' }); } }, 'Prime auger')),
    el('button', { class: 'btn ghost block', type: 'button', onclick: () => close() }, 'Cancel')));
}

function actionButtons(s) {
  const b = [];
  const stop = el('button', { class: 'btn danger', onclick: async () => { if (await confirmDialog('Stop the grill?', 'All outputs turn off immediately.', 'Stop', true)) cmd({ cmd: 'stop' }); } }, 'Stop');
  const shutdown = el('button', { class: 'btn', onclick: async () => { if (await confirmDialog('Shut down?', `Feed stops and the fan runs for ${fmtDur(s.timers.shutdown_duration)} to cool the pot.`, 'Shutdown')) cmd({ cmd: 'mode', mode: 'Shutdown' }); } }, 'Shutdown');
  const hold = el('button', { class: 'btn primary', onclick: async () => { const v = await numberDialog('Hold temperature', s.setpoint, { presets: presets() }); if (v) cmd({ cmd: 'mode', mode: 'Hold', setpoint: v }); } }, 'Hold');
  switch (s.mode) {
    case 'Stop': case 'Monitor': b.push(el('button', { class: 'btn primary', onclick: startDialog }, 'Start cooking')); if (s.mode === 'Monitor') b.push(stop); break;
    case 'Smoke': {
      // Smoke <-> Smoke+ is a confirmed toggle, only offered while smoking
      const splus = el('button', { class: `btn ${s.s_plus ? '' : 'ghost'}`, onclick: async () => {
        const to = !s.s_plus;
        if (await confirmDialog(to ? 'Switch to Smoke+?' : 'Switch to Smoke?', to ? 'The fan cycles on and off for more smoke while the pit stays in range.' : 'The fan runs continuously again.', to ? 'Smoke+' : 'Smoke')) cmd({ cmd: 'smoke_plus', enabled: to });
      } }, s.s_plus ? 'Smoke+ → Smoke' : 'Smoke → Smoke+');
      b.push(splus, hold, shutdown); break;
    }
    case 'Hold': b.push(el('button', { class: 'btn', onclick: () => cmd({ cmd: 'mode', mode: 'Smoke' }) }, 'Smoke'), shutdown); break;
    case 'Startup': case 'Reignite': case 'Prime': b.push(shutdown, stop); break;
    case 'Shutdown': b.push(stop); break;
    case 'Manual': b.push(el('button', { class: 'btn', onclick: () => (location.hash = '#/more/manual') }, 'Manual outputs'), stop); break;
    case 'Error': b.push(el('button', { class: 'btn danger', onclick: () => cmd({ cmd: 'stop' }) }, 'Clear & Stop')); break;
  }
  return b;
}

export function renderHome(view) {
  const bar = el('div', { class: 'bar' }, el('span', { class: 'mode' }), el('span', { class: 'clock' }));
  const label = el('div', { class: 'label' });
  const big = el('div', { class: 'big' });
  const line1 = el('div', { class: 'line1' });
  const line2 = el('div', { class: 'line2' });
  const spRow = el('div', { class: 'setpoint-row' });
  const actions = el('div', { class: 'btnrow' });
  const rows = el('div', { class: 'rows' });
  const rowsCard = el('div', { class: 'card tight' }, rows);
  const chips = el('div', { class: 'chips' });
  const ctrl = el('div', { class: 'kv' });
  view.append(
    el('div', { class: 'card hero' }, bar, label, big, line1, line2, spRow, actions),
    rowsCard,
    el('div', { class: 'card tight' }, chips),
    el('details', { class: 'card tight' }, el('summary', { class: 'muted' }, 'Controller'), ctrl),
  );

  const changeSetpoint = async () => {
    const s = PF.status;
    if (!s) return;
    const v = await numberDialog('Hold temperature', s.setpoint, { presets: presets() });
    if (!v) return;
    if (s.mode === 'Hold' || s.mode === 'Startup' || s.mode === 'Reignite') cmd({ cmd: 'setpoint', setpoint: v });
    else cmd({ cmd: 'mode', mode: 'Hold', setpoint: v });
  };

  let lastMode = null;
  const update = (s) => {
    if (!s) return;
    const primary = s.probes.find((p) => p.role === 'Primary');
    const t = heroText(s);
    bar.querySelector('.mode').textContent = s.mode;
    bar.querySelector('.mode').style.setProperty('--dotc', modeColor[s.mode] || 'var(--accent)');
    bar.querySelector('.clock').textContent = t.clock;
    label.textContent = primary?.name || 'Pit';
    big.innerHTML = '';
    big.append(primary?.valid ? fmtTemp(primary.temp) : '—', el('small', {}, degUnit()));
    big.classList.toggle('invalid', !primary?.valid);
    line1.textContent = t.line1;
    line1.className = `line1 ${t.cls}`;
    line1.onclick = s.mode === 'Hold' || ((s.mode === 'Startup' || s.mode === 'Reignite') && s.next_mode === 'Hold') ? changeSetpoint : null;
    line2.textContent = t.line2;

    if (s.mode !== lastMode) {
      lastMode = s.mode;
      actions.innerHTML = ''; actions.append(...actionButtons(s));
      spRow.innerHTML = '';
      if (s.mode === 'Hold') {
        spRow.append(
          el('button', { class: 'btn', 'aria-label': 'Lower set point', onclick: () => cmd({ cmd: 'setpoint', setpoint: PF.status.setpoint - 5 }) }, '−'),
          el('button', { class: 'btn ghost value', onclick: changeSetpoint }, ''),
          el('button', { class: 'btn', 'aria-label': 'Raise set point', onclick: () => cmd({ cmd: 'setpoint', setpoint: PF.status.setpoint + 5 }) }, '+'));
      }
    }
    const val = spRow.querySelector('.value');
    if (val) val.textContent = `${fmtTemp(s.setpoint)}${degUnit()}`;

    rows.innerHTML = '';
    const food = s.probes.filter((p) => p.role === 'Food' && p.enabled && p.home !== false);
    for (const p of food) {
      const hit = p.target > 0 && p.valid && p.temp >= p.target;
      rows.append(el('div', { class: `prow ${p.valid ? '' : 'invalid'} ${hit ? 'hit' : ''}`, onclick: () => (location.hash = '#/cook') },
        el('div', { class: 'n' }, p.name, p.target > 0 ? el('span', { class: 'tg' }, `→ ${fmtTemp(p.target)}°`) : null),
        el('div', { class: 't' }, p.valid ? fmtTemp(p.temp) : '—')));
    }
    rowsCard.hidden = !food.length;

    chips.innerHTML = '';
    const o = s.outputs;
    chips.append(
      el('span', { class: `chip ${o.power ? 'on' : ''}` }, 'Power'),
      el('span', { class: `chip ${o.fan ? 'on' : ''}` }, PF.settings?.platform?.dc_fan && o.fan ? `Fan ${o.fan_pct}%` : 'Fan'),
      el('span', { class: `chip ${o.auger ? 'on hot' : ''}` }, 'Auger'),
      el('span', { class: `chip ${o.igniter ? 'on hot' : ''}` }, 'Igniter'));

    ctrl.innerHTML = '';
    const c = s.controller;
    for (const [k, v] of [['Controller', c.id], ['Feed (raw / applied)', `${s.cycle.u_raw.toFixed(2)} / ${s.cycle.u_applied.toFixed(2)}`], ['Cycle', `${s.cycle.cycle_s}s`], ['P / I / D', `${c.p.toFixed(2)} / ${c.i.toFixed(2)} / ${c.d.toFixed(2)}`], ['Error', `${c.error.toFixed(1)}${degUnit()}`], ['Ambient', s.ambient == null ? '—' : `${fmtTemp(s.ambient)}${degUnit()}`]])
      ctrl.append(el('div', {}, k), el('div', {}, v));
  };
  update(PF.status);
  return onStatus(update);
}
