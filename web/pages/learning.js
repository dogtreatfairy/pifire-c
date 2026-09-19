import { PF, el, api, cmd, patchSettings, toast, onStatus, confirmDialog, degUnit } from '../app.js';

export function renderLearning(view) {
  const ffCard = el('div', { class: 'card' });
  const plantCard = el('div', { class: 'card' });
  const atCard = el('div', { class: 'card' });
  const recent = el('div', { class: 'list' });
  view.append(el('h2', {}, 'Feed-forward model'), ffCard, el('h2', {}, 'Plant estimate'), plantCard, el('h2', {}, 'Autotune'), atCard, el('h2', {}, 'Recent observations'), el('div', { class: 'card' }, recent));

  let data = null;
  async function load() {
    data = await api('/learning');
    render();
  }
  function render() {
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
      el('div', { class: 'form-actions' }, el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Reset learning?', 'All observations, plant estimates and autotune results are erased.', 'Reset', true)) { await api('/learning/reset', { body: {} }); load(); } } }, 'Reset learning'))].filter(Boolean));

    const p = data.plant;
    plantCard.innerHTML = '';
    plantCard.append(p.valid
      ? el('div', { class: 'kv' }, el('div', {}, 'Gain (K)'), el('div', {}, `${p.K.toFixed(0)}° per unit feed`), el('div', {}, 'Time constant (τ)'), el('div', {}, `${p.tau.toFixed(0)} s`), el('div', {}, 'Dead time (θ)'), el('div', {}, `${p.theta.toFixed(0)} s`), el('div', {}, 'Measured'), el('div', {}, new Date(p.ts * 1000).toLocaleString()))
      : el('p', { class: 'muted' }, 'Measured automatically from the temperature rise of each startup. Not available yet.'));

    const a = data.autotune;
    atCard.innerHTML = '';
    atCard.append(el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Autotune oscillates the feed gently around the set point for 15–40 minutes while holding, measures the response, and suggests PB/Ti/Td. Run it with the grill at temperature and no food inside.'));
    if (s?.autotune?.active) atCard.append(el('div', { class: 'banner warn', style: 'margin:0 0 10px' }, `Autotune running — ${s.autotune.crossings} of 7 crossings`), el('button', { class: 'btn block', onclick: () => cmd({ cmd: 'autotune', start: false }) }, 'Stop autotune'));
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
  return () => { off(); clearInterval(t); };
}
