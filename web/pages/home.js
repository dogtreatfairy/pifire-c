import { PF, el, api, cmd, onStatus, fmtTemp, degUnit, fmtDur, numberDialog, dialog, confirmDialog, patchSettings, toast } from '../app.js';
import { targetDialog, limitsDialog } from './cook.js';
import { btIcon, isWireless, sigBars, fmtEta, battIcon } from './probes.js';
import { icon as lucide } from '../icons.js';

// Home: status row (AUG/FAN/IGN, P-mode), the gauge with the grill temperature (reads 0 while stopped),
// target line, run timer + hopper, the PiFire-style control bar, probe cells, and manual output switches
// while monitoring. The mode with its countdown or target lives in the app header (app.js).

const presetsF = [160, 180, 200, 225, 250, 275, 300, 350, 400];
const presetsC = [70, 80, 95, 107, 120, 135, 150, 175, 205];
const presets = () => (PF.units === 'C' ? presetsC : presetsF);
const gaugeMax = () => (PF.units === 'C' ? 320 : 600);

const LUCIDE = { play: 'play', stop: 'square', glasses: 'glasses', target: 'target', prime: 'chevrons-right', smoke: 'cloud', power: 'power', chevron: 'chevron-up', wrench: 'wrench' };
const icon = (name) => lucide(LUCIDE[name] || name);

// ---- gauge (270° ring, temperature inside)
const R = 100, CX = 120, CY = 120, START = 135, SWEEP = 270, CAP = (6 / R) * (180 / Math.PI); /* round cap = 6 px along the ring */
const polar = (deg, r = R) => [CX + r * Math.cos((deg * Math.PI) / 180), CY + r * Math.sin((deg * Math.PI) / 180)];
const arcPath = (from, to, r = R) => { const [x1, y1] = polar(from, r), [x2, y2] = polar(to, r); return `M ${x1} ${y1} A ${r} ${r} 0 ${to - from > 180 ? 1 : 0} 1 ${x2} ${y2}`; };
function buildGauge() {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 240 240'); svg.setAttribute('class', 'gauge'); svg.setAttribute('role', 'img'); svg.setAttribute('aria-label', 'Grill temperature');
  svg.innerHTML = `<path class="track" d="${arcPath(START + CAP, START + SWEEP - CAP)}" fill="none" stroke-width="12"/>
    <path class="arc" id="g-arc" d="${arcPath(START + CAP, START + CAP + 0.01)}" fill="none" stroke-width="12"/>
    <g id="g-sp" visibility="hidden"><line class="sp" x1="0" y1="0" x2="0" y2="0" stroke-width="4"/><polygon class="spm" id="g-spm" points="0,0 0,0 0,0"/></g>
    <text class="label" x="120" y="82" text-anchor="middle" id="g-label">Grill</text>
    <text class="big" x="120" y="146" text-anchor="middle" id="g-temp">0</text>
    <text class="unit" x="120" y="176" text-anchor="middle" id="g-unit">°F</text>`;
  return svg;
}
function updateGauge(svg, s, primary, stopped) {
  const max = gaugeMax();
  const t = stopped ? 0 : primary?.valid ? primary.temp : null;
  const frac = t == null ? 0 : Math.min(1, Math.max(0, t / max));
  const end = Math.max(START + CAP + 0.01, START + SWEEP * frac - CAP);
  const arc = svg.querySelector('#g-arc');
  arc.setAttribute('d', arcPath(START + CAP, end));
  // Hold: green within +/-7 F of the target, orange within +/-15 F, blue colder / red hotter than that
  let band = '';
  if (s.mode === 'Hold' && t != null && s.setpoint > 0) {
    const e = t - s.setpoint, tight = PF.units === 'C' ? 4 : 7, wide = PF.units === 'C' ? 8 : 15;
    band = Math.abs(e) <= tight ? 'ok' : Math.abs(e) <= wide ? 'warn' : e < 0 ? 'cold' : 'hot';
  }
  arc.setAttribute('class', `arc ${band}`);
  svg.querySelector('#g-temp').textContent = t == null ? '—' : fmtTemp(t);
  svg.querySelector('#g-temp').classList.toggle('muted', stopped || t == null);
  svg.querySelector('#g-unit').textContent = degUnit();
  svg.querySelector('#g-label').textContent = primary?.name || 'Grill';
  const sp = svg.querySelector('#g-sp');
  const holdLike = s.mode === 'Hold' || s.mode === 'Reignite' || (s.mode === 'Startup' && s.next_mode === 'Hold');
  if (holdLike && s.setpoint > 0) {
    const a = START + SWEEP * Math.min(1, s.setpoint / max);
    const [x1, y1] = polar(a, R - 10), [x2, y2] = polar(a, R + 10);
    const line = sp.querySelector('line');
    line.setAttribute('x1', x1); line.setAttribute('y1', y1); line.setAttribute('x2', x2); line.setAttribute('y2', y2);
    // triangle pointing at the ring from outside
    const [tx, ty] = polar(a, R + 13), [lx, ly] = polar(a - 4, R + 23), [rx, ry] = polar(a + 4, R + 23);
    sp.querySelector('#g-spm').setAttribute('points', `${tx},${ty} ${lx},${ly} ${rx},${ry}`);
    sp.setAttribute('visibility', 'visible');
  } else sp.setAttribute('visibility', 'hidden');
}

// ---- actions
const holdAt = async (s, change, force = false) => {
  const v = await numberDialog('Hold temperature', s.setpoint || PF.settings?.startup?.start_to_mode?.primary_setpoint || (PF.units === 'C' ? 107 : 225), { presets: presets() });
  if (!v) return;
  if (change) cmd({ cmd: 'setpoint', setpoint: v }); else cmd({ cmd: 'mode', mode: 'Hold', setpoint: v, force });
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
// Smoke button menu: Smoke <-> Smoke+ plus a 1-9 dial pad for the P-mode
function smokeMenu(s) {
  const cur = PF.settings?.cycle_data?.PMode ?? 2;
  return dialog((close) => el('div', {}, el('h3', {}, s.s_plus ? 'Smoke+' : 'Smoke'),
    el('button', { class: 'btn block', type: 'button', style: 'margin-bottom:12px', onclick: async () => { close(); const to = !s.s_plus; if (await confirmDialog(to ? 'Switch to Smoke+?' : 'Switch to Smoke?', to ? 'The fan cycles on and off for more smoke while the pit stays in range.' : 'The fan runs continuously again.', to ? 'Smoke+' : 'Smoke')) cmd({ cmd: 'smoke_plus', enabled: to }); } }, s.s_plus ? 'Switch to Smoke' : 'Switch to Smoke+'),
    el('div', { class: 'muted', style: 'font-size:.85rem;margin-bottom:6px' }, `P-Mode · now ${cur} · higher = fewer pellets, more smoke`),
    el('div', { class: 'presets pad' }, ...Array.from({ length: 9 }, (_, i) => i + 1).map((n) => el('button', { class: `btn ${n === cur ? 'primary' : ''}`, type: 'button', onclick: async () => { close(); try { await patchSettings('cycle_data', { PMode: n }); toast(`P-Mode ${n}`); } catch (e) { toast(e.message, true); } } }, String(n)))),
    el('button', { class: 'btn ghost block', type: 'button', onclick: () => close() }, 'Cancel')));
}
// Shutdown dialog: the normal cool-down, or an immediate Emergency Stop (everything off, no cool-down)
const shutdown = (s) => dialog((close) => el('div', {}, el('h3', {}, 'Shut down?'),
  el('p', { class: 'muted' }, `Feed stops and the fan runs for ${fmtDur(s.timers.shutdown_duration)} to cool the pot.`),
  el('div', { class: 'btnrow' },
    el('button', { class: 'btn ghost', type: 'button', onclick: () => close() }, 'Cancel'),
    el('button', { class: 'btn primary', type: 'button', onclick: () => { close(); cmd({ cmd: 'mode', mode: 'Shutdown' }); } }, 'Shutdown')),
  el('button', { class: 'btn danger block', type: 'button', style: 'margin-top:10px', onclick: () => { close(); cmd({ cmd: 'stop' }); } }, 'Emergency Stop — all outputs off now')));
const stopGrill = (s) => (s.mode === 'Error' ? cmd({ cmd: 'stop' }) : confirmDialog('Stop the grill?', 'All outputs turn off immediately.', 'Stop', true).then((ok) => ok && cmd({ cmd: 'stop' })));

// ---- control bar: the transitions that make sense from the current mode
function controlBar(s) {
  const b = (ic, label, opts = {}) => el('button', { class: `cb ${opts.active ? 'active' : ''} ${opts.cls || ''}`, disabled: !!opts.disabled, onclick: opts.onclick, 'aria-label': opts.aria || label }, icon(ic), label ? el('span', {}, label) : null, opts.caret ? el('span', { class: 'caret' }, icon('chevron')) : null);
  const left = [], right = [];
  const stop = b('stop', '', { cls: 'danger', onclick: () => stopGrill(s), aria: 'Stop' });
  switch (s.mode) {
    case 'Stop': case 'Monitor':
      left.push(b('prime', '', { onclick: primeMenu, aria: 'Prime' }), el('span', { class: 'cb-caret' }, icon('chevron')));
      right.push(b('play', '', { cls: 'ok', onclick: startGrill, aria: 'Start' }),
        b('glasses', '', { active: s.mode === 'Monitor', onclick: () => cmd({ cmd: 'mode', mode: s.mode === 'Monitor' ? 'Stop' : 'Monitor' }), aria: 'Monitor' }),
        b('stop', '', { cls: 'danger', active: s.mode === 'Stop', disabled: s.mode === 'Stop', onclick: () => cmd({ cmd: 'stop' }), aria: 'Stop' }));
      break;
    case 'Startup': case 'Reignite':
      right.push(b('play', '', { active: true, cls: 'ok', disabled: true, aria: s.mode }),
        b('smoke', '', { cls: 'accent', onclick: () => confirmDialog('Skip to Smoke?', 'Ends startup now. Only do this once the fire is clearly lit.', 'Smoke').then((ok) => ok && cmd({ cmd: 'mode', mode: 'Smoke', force: true })), aria: 'Smoke' }),
        b('target', '', { cls: 'ok', onclick: () => confirmDialog('Skip to Hold?', 'Ends startup now. Only do this once the fire is clearly lit.', 'Hold').then((ok) => ok && holdAt(s, false, true)), aria: 'Hold' }),
        b('power', '', { onclick: () => shutdown(s), aria: 'Shutdown' }));
      break;
    case 'Prime':
      right.push(b('play', 'Prime', { active: true, cls: 'ok', disabled: true }), b('power', '', { onclick: () => shutdown(s), aria: 'Shutdown' }));
      break;
    case 'Smoke':
      right.push(b('smoke', s.s_plus ? 'Smoke+' : `P${PF.settings?.cycle_data?.PMode ?? ''}`, { active: true, cls: 'accent', caret: true, onclick: () => smokeMenu(s) }),
        b('target', '', { onclick: () => holdAt(s, false), aria: 'Hold' }), b('power', '', { onclick: () => shutdown(s), aria: 'Shutdown' }));
      break;
    case 'Hold':
      right.push(b('smoke', '', { onclick: () => cmd({ cmd: 'mode', mode: 'Smoke' }), aria: 'Smoke' }),
        b('target', `${fmtTemp(s.setpoint)}°`, { active: true, cls: 'ok', onclick: () => holdAt(s, true) }), b('power', '', { onclick: () => shutdown(s), aria: 'Shutdown' }));
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

// Probe popup: live reading, target (tap to set, doneness presets), high/low alerts (alerts only)
function probePopup(label) {
  const p = () => PF.status?.probes?.find((x) => x.label === label);
  return dialog((close) => {
    const wrap = el('div');
    const render = () => {
      const q = p(); if (!q) { close(); return; }
      wrap.innerHTML = '';
      wrap.append(el('h3', {}, isWireless(q.device) ? btIcon() : null, ' ', q.name),
        el('div', { class: 'ppop-temp' }, q.valid ? fmtTemp(q.temp) : '—', el('small', {}, degUnit())),
        el('div', { class: 'kv' },
          el('div', {}, 'Target'), el('div', {}, q.target > 0 ? `${fmtTemp(q.target)}${degUnit()}` : '—'),
          q.ambient_label ? el('div', {}, 'Ambient') : null, q.ambient_label ? el('div', {}, q.ambient == null ? '—' : `${fmtTemp(q.ambient)}${degUnit()}`) : null,
          q.target > 0 ? el('div', {}, 'Time to target') : null, q.target > 0 ? el('div', {}, q.valid && q.temp >= q.target ? 'reached' : q.eta_s > 0 ? `about ${fmtEta(q.eta_s)}` : 'estimating…') : null,
          q.wireless ? el('div', {}, 'Signal') : null, q.wireless ? el('div', { class: 'row', style: 'gap:6px' }, sigBars(q.signal || 0), q.rssi ? `${q.rssi} dBm` : 'no link') : null,
          q.wireless && q.battery >= 0 ? el('div', {}, 'Battery') : null, q.wireless && q.battery >= 0 ? el('div', {}, battIcon(q.battery)) : null,
          el('div', {}, 'Alert above'), el('div', {}, q.limit_high > 0 ? `${fmtTemp(q.limit_high)}${degUnit()}` : 'off'),
          el('div', {}, 'Alert below'), el('div', {}, q.limit_low > 0 ? `${fmtTemp(q.limit_low)}${degUnit()}` : 'off')),
        el('div', { class: 'btnrow', style: 'margin-top:12px' },
          el('button', { class: 'btn primary', type: 'button', onclick: async () => { const r = await targetDialog(q); if (r) { cmd({ cmd: 'target', label: q.label, ...r }); setTimeout(render, 600); } } }, q.target > 0 ? 'Change target' : 'Set target'),
          el('button', { class: 'btn', type: 'button', onclick: async () => { const r = await limitsDialog(q); if (r) { cmd({ cmd: 'limits', label: q.label, ...r }); setTimeout(render, 600); } } }, 'Alerts')),
        el('div', { class: 'btnrow', style: 'margin-top:8px' },
          q.target > 0 ? el('button', { class: 'btn ghost', type: 'button', onclick: () => { cmd({ cmd: 'target', label: q.label, target: 0, after: 0 }); setTimeout(render, 600); } }, 'Clear target') : null,
          el('button', { class: 'btn ghost', type: 'button', onclick: () => close() }, 'Close')));
    };
    render();
    return wrap;
  });
}

export function renderHome(view) {
  const outs = ['fan', 'auger', 'igniter'].map((k) => el('span', { class: 'out', 'data-k': k }, k === 'auger' ? 'AUG' : k === 'fan' ? 'FAN' : 'IGN'));
  const header = el('div', { class: 'hbar' }, ...outs);
  const gauge = buildGauge();
  const target = el('div', { class: 'line1' });
  const detail = el('div', { class: 'line2' });
  const bar = el('div');
  const hopBrand = el('span', { class: 'muted' }), hopPct = el('span', { class: 'pct' }), hopFill = el('div');
  const hopper = el('div', { class: 'card tight hopper', hidden: true }, el('div', { class: 'row between' }, el('div', {}, el('strong', {}, 'Hopper'), ' ', hopBrand), hopPct), el('div', { class: 'progress' }, hopFill));
  const manual = el('div', { class: 'card tight', hidden: true });
  const probes = el('div', { class: 'pgrid' });
  const ctrl = el('div', { class: 'kv' });
  view.append(
    el('div', { class: 'card hero' }, header, gauge, target, detail, bar),
    hopper, manual, probes,
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

    updateGauge(gauge, s, primary, stopped);

    let t = { text: 'Ready', cls: 'muted' };
    switch (s.mode) {
      case 'Hold': t = { text: `Target ${fmtTemp(s.setpoint)}${u}`, cls: '', tap: true }; break;
      case 'Startup': case 'Reignite': t = s.next_mode === 'Hold' && s.setpoint > 0 ? { text: `Igniting → hold ${fmtTemp(s.setpoint)}${u}`, cls: '', tap: true } : { text: 'Igniting → smoke', cls: '' }; break;
      case 'Smoke': t = { text: s.s_plus ? 'Smoke+' : 'Smoke', cls: 'accent' }; break;
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
    detail.textContent = bits.join(' · ') || '\u00a0';
    hopper.hidden = !(s.hopper_pct >= 0);
    if (s.hopper_pct >= 0) {
      const low = s.hopper_pct <= (PF.settings?.pelletlevel?.warning_level ?? 25);
      const crit = s.hopper_pct <= 10;   /* amber while it is getting low, red when it is going to run out */
      hopBrand.textContent = brand || '';
      hopPct.textContent = `${s.hopper_pct}%`;
      hopFill.style.width = `${Math.max(0, Math.min(100, s.hopper_pct))}%`;
      hopper.classList.toggle('low', low && !crit);
      hopper.classList.toggle('crit', crit);
    }

    const key = `${s.mode}|${s.s_plus}|${s.setpoint}|${pm}`;
    if (key !== lastBar) { lastBar = key; bar.innerHTML = ''; bar.append(controlBar(s)); }

    // manual output switches while monitoring (auger cap and the other interlocks still apply)
    manual.hidden = s.mode !== 'Monitor';
    if (s.mode === 'Monitor') {
      const want = `${s.outputs.auger}|${s.outputs.fan}|${s.outputs.igniter}|${s.outputs.fan_pct}`;
      if (manual.dataset.state !== want) {
        manual.dataset.state = want;
        manual.innerHTML = '';
        manual.append(el('div', { class: 'muted', style: 'font-size:.8rem;margin-bottom:4px' }, 'Manual outputs — everything turns off when you press Stop'),
          manualRow('Auger', 'auger', s.outputs.auger), manualRow('Fan', 'fan', s.outputs.fan));
        /* a variable-speed fan turns on at full and is dialled down from there, because a fan you
           have to set a number on before it moves any air does not read as a switch */
        if (PF.settings?.platform?.dc_fan && s.outputs.fan) {
          const pct = el('span', { class: 'fanpct' }, `${s.outputs.fan_pct}%`);
          const sl = el('input', { type: 'range', min: 10, max: 100, step: 5, value: s.outputs.fan_pct || 100,
            oninput: (e) => (pct.textContent = `${e.target.value}%`),
            onchange: (e) => cmd({ cmd: 'manual', output: 'pwm', pct: Number(e.target.value) }) });
          manual.append(el('div', { class: 'fanrow' }, el('span', { class: 'muted' }, 'Fan speed'), sl, pct));
        }
        manual.append(manualRow('Igniter', 'igniter', s.outputs.igniter));
      }
    } else manual.dataset.state = '';

    probes.innerHTML = '';
    const food = s.probes.filter((p) => p.role === 'Food' && p.enabled && p.home !== false && !p.companion).slice(0, 3);
    probes.style.gridTemplateColumns = `repeat(${Math.max(1, food.length)}, 1fr)`;
    for (const p of food) {
      const hit = p.target > 0 && p.valid && p.temp >= p.target;
      probes.append(el('div', { class: `pcell ${p.valid ? '' : 'invalid'} ${hit ? 'hit' : ''}`, onclick: () => probePopup(p.label) },
        el('div', { class: 'n' }, p.wireless ? [btIcon(), sigBars(p.signal || 0, p.rssi ? `${p.rssi} dBm` : 'no link'), p.battery >= 0 ? battIcon(p.battery) : null, ' '] : null, p.name), el('div', { class: 't' }, p.valid ? fmtTemp(p.temp) : '—'),
        p.ambient_label ? el('div', { class: 'amb' }, `Ambient ${p.ambient == null ? '—' : fmtTemp(p.ambient) + '°'}`) : null,
        el('div', { class: `tg ${p.target > 0 ? '' : 'muted'}` }, p.target > 0 ? `Target ${fmtTemp(p.target)}°${!hit && p.eta_s > 0 ? ` · ${fmtEta(p.eta_s)}` : ''}` : 'Set target')));
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
