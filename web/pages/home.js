import { PF, el, api, cmd, onStatus, fmtTemp, degUnit, fmtDur, numberDialog, dialog, confirmDialog, patchSettings, toast } from '../app.js';

// Home: status row (AUG/FAN/IGN, P-mode), the gauge with the grill temperature (reads 0 while stopped),
// target line, run timer + hopper, the PiFire-style control bar, probe cells, and manual output switches
// while monitoring. The mode with its countdown or target lives in the app header (app.js).

const presetsF = [180, 200, 225, 250, 275, 300, 350, 400];
const presetsC = [80, 95, 107, 120, 135, 150, 175, 205];
const presets = () => (PF.units === 'C' ? presetsC : presetsF);
const gaugeMax = () => (PF.units === 'C' ? 320 : 600);

// ---- icons (inline SVG, 24px viewBox)
const I = {
  play: 'M8 5v14l11-7z',
  stop: 'M6 6h12v12H6z',
  glasses: 'M6 10a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7zm12 0a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7zM9.5 13.5h5M2 12l2-4h3M22 12l-2-4h-3',
  target: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zm0 4a5 5 0 1 0 0 10 5 5 0 0 0 0-10zm0 3.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z',
  prime: 'M5 6l6 6-6 6M12 6l6 6-6 6',
  smoke: 'M6 15a4 4 0 0 1 .5-8A5.5 5.5 0 0 1 17 8.5 3.5 3.5 0 0 1 17 15H6z',
  power: 'M12 3v9M6.3 7.3a8 8 0 1 0 11.4 0',
  chevron: 'M7 14l5-5 5 5',
  wrench: 'M14.7 6.3a4 4 0 0 0-5.4 5.4L3 18l3 3 6.3-6.3a4 4 0 0 0 5.4-5.4l-2.4 2.4-2.1-2.1z',
};
const STROKED = ['glasses', 'prime', 'power', 'chevron'];
const icon = (name) => {
  const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  s.setAttribute('viewBox', '0 0 24 24');
  s.setAttribute('class', 'ic');
  const p = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  p.setAttribute('d', I[name]);
  if (STROKED.includes(name)) { p.setAttribute('fill', 'none'); p.setAttribute('stroke', 'currentColor'); p.setAttribute('stroke-width', '2'); p.setAttribute('stroke-linecap', 'round'); p.setAttribute('stroke-linejoin', 'round'); }
  else p.setAttribute('fill', 'currentColor');
  s.append(p);
  return s;
};

// ---- gauge (270° ring, temperature inside)
const R = 100, CX = 120, CY = 120, START = 135, SWEEP = 270;
const polar = (deg, r = R) => [CX + r * Math.cos((deg * Math.PI) / 180), CY + r * Math.sin((deg * Math.PI) / 180)];
const arcPath = (from, to, r = R) => { const [x1, y1] = polar(from, r), [x2, y2] = polar(to, r); return `M ${x1} ${y1} A ${r} ${r} 0 ${to - from > 180 ? 1 : 0} 1 ${x2} ${y2}`; };
function buildGauge() {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 240 240'); svg.setAttribute('class', 'gauge'); svg.setAttribute('role', 'img'); svg.setAttribute('aria-label', 'Grill temperature');
  svg.innerHTML = `<path class="track" d="${arcPath(START, START + SWEEP)}" fill="none" stroke-width="12"/>
    <path class="arc" id="g-arc" d="${arcPath(START, START + 0.01)}" fill="none" stroke-width="12"/>
    <line class="sp" id="g-sp" x1="0" y1="0" x2="0" y2="0" stroke-width="4" visibility="hidden"/>
    <text class="label" x="120" y="82" text-anchor="middle" id="g-label">Grill</text>
    <text class="big" x="120" y="146" text-anchor="middle" id="g-temp">0</text>
    <text class="unit" x="120" y="176" text-anchor="middle" id="g-unit">°F</text>`;
  return svg;
}
function updateGauge(svg, s, primary, stopped) {
  const max = gaugeMax();
  const t = stopped ? 0 : primary?.valid ? primary.temp : null;
  const frac = t == null ? 0 : Math.min(1, Math.max(0, t / max));
  svg.querySelector('#g-arc').setAttribute('d', arcPath(START, START + Math.max(0.01, SWEEP * frac)));
  svg.querySelector('#g-temp').textContent = t == null ? '—' : fmtTemp(t);
  svg.querySelector('#g-temp').classList.toggle('muted', stopped || t == null);
  svg.querySelector('#g-unit').textContent = degUnit();
  svg.querySelector('#g-label').textContent = primary?.name || 'Grill';
  const sp = svg.querySelector('#g-sp');
  const holdLike = s.mode === 'Hold' || s.mode === 'Reignite' || (s.mode === 'Startup' && s.next_mode === 'Hold');
  if (holdLike && s.setpoint > 0) {
    const a = START + SWEEP * Math.min(1, s.setpoint / max);
    const [x1, y1] = polar(a, R - 11), [x2, y2] = polar(a, R + 11);
    sp.setAttribute('x1', x1); sp.setAttribute('y1', y1); sp.setAttribute('x2', x2); sp.setAttribute('y2', y2);
    sp.setAttribute('visibility', 'visible');
  } else sp.setAttribute('visibility', 'hidden');
}

// ---- actions
const holdAt = async (s, change) => {
  const v = await numberDialog('Hold temperature', s.setpoint || PF.settings?.startup?.start_to_mode?.primary_setpoint || (PF.units === 'C' ? 107 : 225), { presets: presets() });
  if (!v) return;
  if (change) cmd({ cmd: 'setpoint', setpoint: v }); else cmd({ cmd: 'mode', mode: 'Hold', setpoint: v });
};
// Play: honours Settings -> Startup -> "After startup go to" and the hold prompt, like the original
async function startGrill() {
  const st = PF.settings?.startup?.start_to_mode || {};
  if (st.after_startup_mode === 'Hold') {
    if (st.start_to_hold_prompt) { const v = await numberDialog('Hold temperature after startup', st.primary_setpoint || 225, { presets: presets() }); if (v) cmd({ cmd: 'mode', mode: 'Hold', setpoint: v }); }
    else cmd({ cmd: 'mode', mode: 'Hold', setpoint: st.primary_setpoint || 225 });
  } else cmd({ cmd: 'mode', mode: 'Smoke' });
}
function primeMenu() {
  return dialog((close) => el('div', {}, el('h3', {}, 'Prime auger'),
    el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Pushes pellets into the fire pot. Use after the hopper ran empty.'),
    el('div', { class: 'opts' },
      ...[10, 15, 20, 25].map((g) => el('button', { class: 'btn', type: 'button', onclick: () => { close(); cmd({ cmd: 'prime', amount: g, next: '' }); } }, `Prime ${g} g`)),
      el('button', { class: 'btn primary', type: 'button', onclick: async () => { close(); const g = await numberDialog('Prime amount', 10, { min: 1, max: 100, step: 5, unit: ' g', presets: [5, 10, 20, 30] }); if (g) cmd({ cmd: 'prime', amount: g, next: 'Startup' }); } }, 'Prime, then start')),
    el('button', { class: 'btn ghost block', type: 'button', onclick: () => close() }, 'Cancel')));
}
function pmodeMenu() {
  const cur = PF.settings?.cycle_data?.PMode ?? 2;
  return dialog((close) => el('div', {}, el('h3', {}, 'P-Mode'),
    el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Pause between auger runs in Smoke: higher = fewer pellets, more smoke, lower temperature.'),
    el('div', { class: 'presets' }, ...Array.from({ length: 10 }, (_, n) => el('button', { class: `btn ${n === cur ? 'primary' : ''}`, type: 'button', onclick: async () => { close(); try { await patchSettings('cycle_data', { PMode: n }); toast(`P-Mode ${n}`); } catch (e) { toast(e.message, true); } } }, `P${n}`))),
    el('button', { class: 'btn ghost block', type: 'button', onclick: () => close() }, 'Cancel')));
}
function smokeMenu(s) {
  return dialog((close) => el('div', {}, el('h3', {}, 'Smoke'),
    el('div', { class: 'opts' },
      el('button', { class: 'btn', type: 'button', onclick: async () => { close(); const to = !s.s_plus; if (await confirmDialog(to ? 'Switch to Smoke+?' : 'Switch to Smoke?', to ? 'The fan cycles on and off for more smoke while the pit stays in range.' : 'The fan runs continuously again.', to ? 'Smoke+' : 'Smoke')) cmd({ cmd: 'smoke_plus', enabled: to }); } }, s.s_plus ? 'Switch to Smoke' : 'Switch to Smoke+'),
      el('button', { class: 'btn', type: 'button', onclick: () => { close(); pmodeMenu(); } }, `P-Mode (now P${PF.settings?.cycle_data?.PMode ?? '?'})`)),
    el('button', { class: 'btn ghost block', type: 'button', onclick: () => close() }, 'Cancel')));
}
const shutdown = (s) => confirmDialog('Shut down?', `Feed stops and the fan runs for ${fmtDur(s.timers.shutdown_duration)} to cool the pot.`, 'Shutdown').then((ok) => ok && cmd({ cmd: 'mode', mode: 'Shutdown' }));
const stopGrill = (s) => (s.mode === 'Error' ? cmd({ cmd: 'stop' }) : confirmDialog('Stop the grill?', 'All outputs turn off immediately.', 'Stop', true).then((ok) => ok && cmd({ cmd: 'stop' })));

// ---- control bar: the transitions that make sense from the current mode
function controlBar(s) {
  const b = (ic, label, opts = {}) => el('button', { class: `cb ${opts.active ? 'active' : ''} ${opts.cls || ''}`, disabled: !!opts.disabled, onclick: opts.onclick, 'aria-label': opts.aria || label }, icon(ic), label ? el('span', {}, label) : null);
  const left = [], right = [];
  const stop = b('stop', '', { cls: 'danger', active: s.mode === 'Stop', onclick: () => stopGrill(s), aria: 'Stop', disabled: s.mode === 'Stop' });
  switch (s.mode) {
    case 'Stop': case 'Monitor':
      left.push(b('prime', '', { onclick: primeMenu, aria: 'Prime' }), el('span', { class: 'cb-caret' }, icon('chevron')));
      right.push(b('play', '', { cls: 'ok', onclick: startGrill, aria: 'Start' }),
        b('glasses', '', { active: s.mode === 'Monitor', onclick: () => cmd({ cmd: 'mode', mode: s.mode === 'Monitor' ? 'Stop' : 'Monitor' }), aria: 'Monitor' }), stop);
      break;
    case 'Startup': case 'Reignite': case 'Prime':
      right.push(b('play', s.mode, { active: true, cls: 'ok', disabled: true }), b('power', '', { onclick: () => shutdown(s), aria: 'Shutdown' }), stop);
      break;
    case 'Smoke':
      right.push(b('smoke', s.s_plus ? 'Smoke+' : `P${PF.settings?.cycle_data?.PMode ?? ''}`, { active: true, cls: 'ok', onclick: () => smokeMenu(s) }),
        b('target', '', { onclick: () => holdAt(s, false), aria: 'Hold' }), b('power', '', { onclick: () => shutdown(s), aria: 'Shutdown' }), stop);
      break;
    case 'Hold':
      right.push(b('target', `${fmtTemp(s.setpoint)}°`, { active: true, cls: 'ok', onclick: () => holdAt(s, true) }),
        b('smoke', '', { onclick: () => cmd({ cmd: 'mode', mode: 'Smoke' }), aria: 'Smoke' }), b('power', '', { onclick: () => shutdown(s), aria: 'Shutdown' }), stop);
      break;
    case 'Shutdown':
      right.push(b('power', 'Shutdown', { active: true, cls: 'info', disabled: true }), stop);
      break;
    case 'Manual':
      right.push(b('wrench', 'Manual', { active: true, onclick: () => (location.hash = '#/more/manual') }), stop);
      break;
    case 'Error':
      right.push(b('stop', 'Clear & Stop', { cls: 'danger', onclick: () => stopGrill(s) }));
      break;
  }
  return el('div', { class: 'cbar' }, left.length ? el('div', { class: 'cgroup' }, ...left) : null, el('div', { class: 'cgroup' }, ...right));
}

export function renderHome(view) {
  const outs = ['auger', 'fan', 'igniter'].map((k) => el('span', { class: 'out', 'data-k': k }, k === 'auger' ? 'AUG' : k === 'fan' ? 'FAN' : 'IGN'));
  const pchip = el('button', { class: 'out pmode', onclick: () => pmodeMenu() }, 'P-');
  const header = el('div', { class: 'hbar' }, ...outs, pchip);
  const gauge = buildGauge();
  const target = el('div', { class: 'line1' });
  const detail = el('div', { class: 'line2' });
  const bar = el('div');
  const manual = el('div', { class: 'card tight', hidden: true });
  const probes = el('div', { class: 'pgrid' });
  const ctrl = el('div', { class: 'kv' });
  view.append(
    el('div', { class: 'card hero' }, header, gauge, target, detail, bar),
    manual, probes,
    el('details', { class: 'card tight' }, el('summary', { class: 'muted' }, 'Controller'), ctrl),
  );

  let brand = '';
  api('/pellets').then((p) => { brand = p.current?.brand || ''; }).catch(() => {});

  const manualRow = (label, key, on) => el('div', { class: 'toggle' }, el('div', {}, label), el('label', { class: 'switch' }, el('input', { type: 'checkbox', checked: on, onchange: (e) => cmd({ cmd: 'manual', output: key, on: e.target.checked }) }), el('span')));

  let lastBar = '';
  const update = (s) => {
    if (!s) return;
    const primary = s.probes.find((p) => p.role === 'Primary');
    const stopped = s.mode === 'Stop';
    const u = degUnit();
    for (const o of outs) { const on = !!s.outputs[o.dataset.k]; o.classList.toggle('on', on); o.textContent = o.dataset.k === 'fan' && on && PF.settings?.platform?.dc_fan ? `FAN ${s.outputs.fan_pct}%` : o.dataset.k === 'auger' ? 'AUG' : o.dataset.k === 'fan' ? 'FAN' : 'IGN'; }
    const pm = PF.settings?.cycle_data?.PMode;
    pchip.textContent = pm == null ? 'P-' : `P${pm}`;
    pchip.classList.toggle('on', s.mode === 'Smoke' || s.mode === 'Startup' || s.mode === 'Reignite');
    pchip.disabled = s.mode !== 'Smoke';

    updateGauge(gauge, s, primary, stopped);

    let t = { text: 'Ready', cls: 'muted' };
    switch (s.mode) {
      case 'Hold': t = { text: `Target ${fmtTemp(s.setpoint)}${u}`, cls: '', tap: true }; break;
      case 'Startup': case 'Reignite': t = s.next_mode === 'Hold' && s.setpoint > 0 ? { text: `Igniting → hold ${fmtTemp(s.setpoint)}${u}`, cls: '', tap: true } : { text: 'Igniting → smoke', cls: '' }; break;
      case 'Smoke': t = { text: s.s_plus ? 'Smoke+' : 'Smoke', cls: 'ok' }; break;
      case 'Shutdown': t = { text: 'Cooling down', cls: 'info' }; break;
      case 'Prime': t = { text: `Priming ${s.timers.prime_amount} g`, cls: '' }; break;
      case 'Error': t = { text: s.safety.error_code.replace(/_/g, ' '), cls: 'danger' }; break;
      case 'Manual': t = { text: 'Manual outputs', cls: 'warn' }; break;
      case 'Monitor': t = { text: 'Monitoring', cls: 'muted' }; break;
    }
    target.textContent = t.text; target.className = `line1 ${t.cls}`; target.onclick = t.tap ? () => holdAt(s, true) : null;

    const bits = [];
    if (s.cook_elapsed > 0) bits.push(`Running ${fmtDur(s.cook_elapsed)}`);
    if (s.mode === 'Hold' && s.lid_open) bits.push('Lid open · auger paused');
    if ((s.mode === 'Startup' || s.mode === 'Reignite') && s.coldstart.active && !s.coldstart.reached) bits.push('Cold start · waiting for rise');
    else if ((s.mode === 'Startup' || s.mode === 'Reignite') && s.timers.startup_exit_temp > 0) bits.push(`Exits at ${fmtTemp(s.timers.startup_exit_temp)}${u}`);
    if (s.hopper_pct >= 0) bits.push(`Hopper ${s.hopper_pct}%${brand ? ' · ' + brand : ''}`);
    detail.textContent = bits.join(' · ') || ' ';

    const key = `${s.mode}|${s.s_plus}|${s.setpoint}|${pm}`;
    if (key !== lastBar) { lastBar = key; bar.innerHTML = ''; bar.append(controlBar(s)); }

    // manual output switches while monitoring (auger cap and the other interlocks still apply)
    manual.hidden = s.mode !== 'Monitor';
    if (s.mode === 'Monitor') {
      const want = `${s.outputs.auger}|${s.outputs.fan}|${s.outputs.igniter}`;
      if (manual.dataset.state !== want) {
        manual.dataset.state = want;
        manual.innerHTML = '';
        manual.append(el('div', { class: 'muted', style: 'font-size:.8rem;margin-bottom:4px' }, 'Manual outputs — everything turns off when you press Stop'),
          manualRow('Auger', 'auger', s.outputs.auger), manualRow('Fan', 'fan', s.outputs.fan), manualRow('Igniter', 'igniter', s.outputs.igniter));
      }
    } else manual.dataset.state = '';

    probes.innerHTML = '';
    const food = s.probes.filter((p) => p.role === 'Food' && p.enabled && p.home !== false).slice(0, 3);
    probes.style.gridTemplateColumns = `repeat(${Math.max(1, food.length)}, 1fr)`;
    for (const p of food) {
      const hit = p.target > 0 && p.valid && p.temp >= p.target;
      probes.append(el('div', { class: `pcell ${p.valid ? '' : 'invalid'} ${hit ? 'hit' : ''}`, onclick: () => (location.hash = '#/cook') },
        el('div', { class: 'n' }, p.name), el('div', { class: 't' }, p.valid ? fmtTemp(p.temp) : '—'), el('div', { class: 'tg' }, p.target > 0 ? `→ ${fmtTemp(p.target)}°` : ' ')));
    }
    probes.hidden = !food.length;

    ctrl.innerHTML = '';
    const c = s.controller;
    for (const [k, v] of [['Controller', c.id], ['Tuning', c.note || '—'], ['Auger duty (raw / applied)', `${s.cycle.u_raw.toFixed(2)} / ${s.cycle.u_applied.toFixed(2)}`], ['Cycle', `${s.cycle.cycle_s}s`], ['P / I / D', `${c.p.toFixed(2)} / ${c.i.toFixed(2)} / ${c.d.toFixed(2)}`], ['Error', `${c.error.toFixed(1)}${u}`], ['Ambient', s.ambient == null ? '—' : `${fmtTemp(s.ambient)}${u}`], ['Power', s.outputs.power ? 'on' : 'off']])
      ctrl.append(el('div', {}, k), el('div', {}, v));
  };
  update(PF.status);
  return onStatus(update);
}
