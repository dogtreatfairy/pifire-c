import { PF, el, cmd, onStatus, fmtTemp, degUnit, fmtDur, numberDialog, dialog, confirmDialog } from '../app.js';

// Home fits one phone screen:  Mode | AUG | FAN | IGN  /  grill temp  /  target  /  [Mode] [Stop]  /  probes

const presetsF = [180, 200, 225, 250, 275, 300, 350, 400];
const presetsC = [80, 95, 107, 120, 135, 150, 175, 205];
const presets = () => (PF.units === 'C' ? presetsC : presetsF);
const timed = (m) => m === 'Startup' || m === 'Reignite' || m === 'Shutdown' || m === 'Prime';
const modeColor = { Hold: 'var(--ok)', Smoke: 'var(--ok)', Error: 'var(--danger)', Shutdown: 'var(--info)', Stop: 'var(--muted)', Monitor: 'var(--muted)' };

function clockText(s) {
  if (timed(s.mode)) {
    const waiting = (s.mode === 'Startup' || s.mode === 'Reignite') && s.coldstart.active && !s.coldstart.reached && s.timers.mode_remaining <= 0;
    return `${fmtDur(waiting ? s.coldstart.remaining : s.timers.mode_remaining)} left`;
  }
  return s.cook_elapsed > 0 ? fmtDur(s.cook_elapsed) : '';
}

// the line under the hero: what the grill is aiming for
function targetText(s) {
  const u = degUnit();
  switch (s.mode) {
    case 'Hold': return { text: `Target ${fmtTemp(s.setpoint)}${u}`, cls: '', tap: true };
    case 'Startup': case 'Reignite': return s.next_mode === 'Hold' && s.setpoint > 0 ? { text: `Igniting → hold ${fmtTemp(s.setpoint)}${u}`, cls: '', tap: true } : { text: 'Igniting → smoke', cls: '' };
    case 'Smoke': return { text: s.s_plus ? 'Smoke+' : `Smoke · P${PF.settings?.cycle_data?.PMode ?? ''}`, cls: 'ok' };
    case 'Shutdown': return { text: 'Cooling down', cls: 'info' };
    case 'Prime': return { text: `Priming ${s.timers.prime_amount} g`, cls: '' };
    case 'Error': return { text: s.safety.error_code.replace(/_/g, ' '), cls: 'danger' };
    case 'Manual': return { text: 'Manual outputs', cls: 'warn' };
    case 'Monitor': return { text: 'Monitoring', cls: 'muted' };
    default: return { text: 'Ready', cls: 'muted' };
  }
}

function detailText(s) {
  const bits = [];
  const clock = clockText(s);
  if (clock) bits.push(clock);
  if (s.mode === 'Hold' && s.lid_open) bits.push('Lid open · auger paused');
  else if ((s.mode === 'Startup' || s.mode === 'Reignite') && s.coldstart.active && !s.coldstart.reached) bits.push('Cold start · waiting for rise');
  else if ((s.mode === 'Startup' || s.mode === 'Reignite') && s.timers.startup_exit_temp > 0) bits.push(`or at ${fmtTemp(s.timers.startup_exit_temp)}${degUnit()}`);
  if (s.hopper_pct >= 0) bits.push(`Hopper ${s.hopper_pct}%`);
  return bits.join(' · ');
}

const holdAt = async (s, change) => {
  const v = await numberDialog('Hold temperature', s.setpoint || (PF.units === 'C' ? 107 : 225), { presets: presets() });
  if (!v) return;
  if (change) cmd({ cmd: 'setpoint', setpoint: v }); else cmd({ cmd: 'mode', mode: 'Hold', setpoint: v });
};

// Mode picker: the transitions that make sense from the current mode (same set as the grill's menu)
async function modeDialog(s) {
  const opts = [];
  const opt = (label, primary, fn) => opts.push(el('button', { class: `btn ${primary ? 'primary' : ''}`, type: 'button', onclick: () => { close(); fn(); } }, label));
  let close = () => {};
  switch (s.mode) {
    case 'Stop': case 'Monitor':
      opt('Start → Hold', true, () => holdAt(s, false));
      opt('Start → Smoke', false, () => cmd({ cmd: 'mode', mode: 'Smoke' }));
      if (s.mode === 'Stop') opt('Monitor only', false, () => cmd({ cmd: 'mode', mode: 'Monitor' }));
      opt('Prime auger', false, async () => { const g = await numberDialog('Prime amount', 10, { min: 1, max: 100, step: 5, unit: ' g', presets: [5, 10, 20, 30] }); if (g) cmd({ cmd: 'prime', amount: g, next: '' }); });
      break;
    case 'Smoke':
      opt('Hold…', true, () => holdAt(s, false));
      opt(s.s_plus ? 'Switch to Smoke' : 'Switch to Smoke+', false, async () => {
        const to = !s.s_plus;
        if (await confirmDialog(to ? 'Switch to Smoke+?' : 'Switch to Smoke?', to ? 'The fan cycles on and off for more smoke while the pit stays in range.' : 'The fan runs continuously again.', to ? 'Smoke+' : 'Smoke')) cmd({ cmd: 'smoke_plus', enabled: to });
      });
      opt('Shutdown', false, () => cmd({ cmd: 'mode', mode: 'Shutdown' }));
      break;
    case 'Hold':
      opt('Change target…', true, () => holdAt(s, true));
      opt('Smoke', false, () => cmd({ cmd: 'mode', mode: 'Smoke' }));
      opt('Shutdown', false, () => cmd({ cmd: 'mode', mode: 'Shutdown' }));
      break;
    case 'Startup': case 'Reignite': case 'Prime':
      opt('Shutdown', false, () => cmd({ cmd: 'mode', mode: 'Shutdown' }));
      break;
    case 'Manual':
      opt('Manual outputs', true, () => (location.hash = '#/more/manual'));
      break;
    default: break;
  }
  if (!opts.length) return;
  return dialog((c) => { close = c; return el('div', {}, el('h3', {}, `${s.mode} → …`), el('div', { class: 'opts' }, ...opts), el('button', { class: 'btn ghost block', type: 'button', onclick: () => c() }, 'Cancel')); });
}

export function renderHome(view) {
  const mode = el('span', { class: 'mode' });
  const outs = ['auger', 'fan', 'igniter'].map((k) => el('span', { class: 'out', 'data-k': k }, k === 'auger' ? 'AUG' : k === 'fan' ? 'FAN' : 'IGN'));
  const header = el('div', { class: 'hbar' }, mode, ...outs);
  const label = el('div', { class: 'label' });
  const big = el('div', { class: 'big' });
  const target = el('div', { class: 'line1' });
  const detail = el('div', { class: 'line2' });
  const modeBtn = el('button', { class: 'btn primary', onclick: () => modeDialog(PF.status) }, 'Mode');
  const stopBtn = el('button', { class: 'btn danger', onclick: async () => { if (await confirmDialog('Stop the grill?', 'All outputs turn off immediately.', 'Stop', true)) cmd({ cmd: 'stop' }); } }, 'Stop');
  const probes = el('div', { class: 'pgrid' });
  const ctrl = el('div', { class: 'kv' });
  view.append(
    el('div', { class: 'card hero' }, header, label, big, target, detail, el('div', { class: 'btnrow' }, modeBtn, stopBtn)),
    probes,
    el('details', { class: 'card tight' }, el('summary', { class: 'muted' }, 'Controller'), ctrl),
  );

  const update = (s) => {
    if (!s) return;
    const primary = s.probes.find((p) => p.role === 'Primary');
    mode.textContent = s.mode;
    mode.style.setProperty('--dotc', modeColor[s.mode] || 'var(--accent)');
    for (const o of outs) { const on = !!s.outputs[o.dataset.k]; o.classList.toggle('on', on); o.textContent = o.dataset.k === 'fan' && on && PF.settings?.platform?.dc_fan ? `FAN ${s.outputs.fan_pct}%` : o.dataset.k === 'auger' ? 'AUG' : o.dataset.k === 'fan' ? 'FAN' : 'IGN'; }
    label.textContent = primary?.name || 'Grill';
    big.innerHTML = '';
    big.append(primary?.valid ? fmtTemp(primary.temp) : '—', el('small', {}, degUnit()));
    big.classList.toggle('invalid', !primary?.valid);
    const t = targetText(s);
    target.textContent = t.text;
    target.className = `line1 ${t.cls}`;
    target.onclick = t.tap ? () => holdAt(s, true) : null;
    detail.textContent = detailText(s);
    modeBtn.textContent = s.mode === 'Stop' || s.mode === 'Monitor' ? 'Start' : s.mode === 'Error' ? 'Error' : 'Mode';
    modeBtn.disabled = s.mode === 'Error' || s.mode === 'Shutdown';
    stopBtn.textContent = s.mode === 'Error' ? 'Clear & Stop' : 'Stop';
    stopBtn.disabled = s.mode === 'Stop';

    probes.innerHTML = '';
    const food = s.probes.filter((p) => p.role === 'Food' && p.enabled && p.home !== false).slice(0, 3);
    probes.style.gridTemplateColumns = `repeat(${Math.max(1, food.length)}, 1fr)`;
    for (const p of food) {
      const hit = p.target > 0 && p.valid && p.temp >= p.target;
      probes.append(el('div', { class: `pcell ${p.valid ? '' : 'invalid'} ${hit ? 'hit' : ''}`, onclick: () => (location.hash = '#/cook') },
        el('div', { class: 'n' }, p.name),
        el('div', { class: 't' }, p.valid ? fmtTemp(p.temp) : '—'),
        el('div', { class: 'tg' }, p.target > 0 ? `→ ${fmtTemp(p.target)}°` : ' ')));
    }
    probes.hidden = !food.length;

    ctrl.innerHTML = '';
    const c = s.controller;
    for (const [k, v] of [['Controller', c.id], ['Auger duty (raw / applied)', `${s.cycle.u_raw.toFixed(2)} / ${s.cycle.u_applied.toFixed(2)}`], ['Cycle', `${s.cycle.cycle_s}s`], ['P / I / D', `${c.p.toFixed(2)} / ${c.i.toFixed(2)} / ${c.d.toFixed(2)}`], ['Error', `${c.error.toFixed(1)}${degUnit()}`], ['Ambient', s.ambient == null ? '—' : `${fmtTemp(s.ambient)}${degUnit()}`], ['Power', s.outputs.power ? 'on' : 'off']])
      ctrl.append(el('div', {}, k), el('div', {}, v));
  };
  update(PF.status);
  return onStatus(update);
}
