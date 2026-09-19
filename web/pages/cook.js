import { PF, el, api, cmd, onStatus, fmtTemp, degUnit, fmtDur, dialog, numberDialog, toast, confirmDialog, segmented } from '../app.js';

// Doneness presets in °F (converted for °C users)
const PRESETS = {
  Beef: [['Rare', 125, 'Cool red center'], ['Medium rare', 135, 'Warm red center'], ['Medium', 145, 'Warm pink center'], ['Medium well', 150, 'Slightly pink center'], ['Well done', 160, 'Cooked through']],
  Brisket: [['Probe tender', 203, 'Pull and rest']],
  Pork: [['Chops / loin', 145, 'Rest 3 min'], ['Pulled pork', 203, 'Falls apart']],
  Ribs: [['Bend test', 195, 'Bones show']],
  Chicken: [['Safe', 165, 'Breast'], ['Thighs', 175, 'Dark meat']],
  Turkey: [['Safe', 165, 'Breast']],
  Fish: [['Flaky', 145, '']],
  Lamb: [['Medium rare', 135, ''], ['Medium', 145, '']],
  Sausage: [['Cooked', 160, '']],
};
const AFTER = [[0, 'Notify only'], [1, 'Keep warm'], [2, 'Shutdown']];
const toUser = (f) => (PF.units === 'C' ? Math.round((f - 32) * 5 / 9) : f);

async function targetDialog(p) {
  return dialog((close) => {
    let after = p.after || 0;
    const custom = el('input', { type: 'text', inputmode: 'decimal', placeholder: `Custom ${degUnit()}`, 'aria-label': 'Custom target', style: 'width:130px' });
    const meats = el('div', { class: 'presets' });
    const options = el('div', { class: 'opts' });
    const showMeat = (meat) => {
      meats.querySelectorAll('button').forEach((b) => b.classList.toggle('primary', b.textContent === meat));
      options.innerHTML = '';
      for (const [name, f, desc] of PRESETS[meat]) {
        const v = toUser(f);
        options.append(el('button', { class: 'btn', type: 'button', onclick: () => close({ target: v, after }) },
          el('div', { class: 'row between' }, el('span', {}, name, desc ? el('span', { class: 'muted', style: 'font-size:.78rem;margin-left:8px' }, desc) : null), el('strong', {}, `${v}${degUnit()}`))));
      }
    };
    for (const m of Object.keys(PRESETS)) meats.append(el('button', { class: 'btn sm', type: 'button', onclick: () => showMeat(m) }, m));
    showMeat('Beef');
    return el('div', {},
      el('h3', {}, `${p.name} target`),
      el('div', { class: 'field' }, el('label', {}, 'After the target is reached'), segmented(AFTER, after, (v) => (after = v))),
      meats, options,
      el('form', { class: 'row', onsubmit: (e) => { e.preventDefault(); const v = parseFloat(custom.value); if (!Number.isNaN(v) && v > 0) close({ target: v, after }); } },
        custom, el('button', { class: 'btn sm primary', type: 'submit' }, 'Set custom')),
      el('div', { class: 'btnrow', style: 'margin-top:10px' },
        p.target > 0 ? el('button', { class: 'btn ghost', type: 'button', onclick: () => close({ target: 0, after: 0 }) }, 'Clear target') : null,
        el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel')));
  });
}

async function limitsDialog(p) {
  return dialog((close) => {
    const hi = el('input', { type: 'text', inputmode: 'decimal', value: p.limit_high || '', placeholder: 'off' });
    const lo = el('input', { type: 'text', inputmode: 'decimal', value: p.limit_low || '', placeholder: 'off' });
    return el('form', { onsubmit: (e) => { e.preventDefault(); close({ high: parseFloat(hi.value) || 0, low: parseFloat(lo.value) || 0 }); } },
      el('h3', {}, `${p.name} alarms`),
      el('p', { class: 'muted', style: 'font-size:.85rem' }, 'Get alerted whenever the probe leaves this range (useful for the pit while you sleep). Leave blank to disable.'),
      el('div', { class: 'field inline' }, el('label', {}, `Alarm above (${degUnit()})`), hi),
      el('div', { class: 'field inline' }, el('label', {}, `Alarm below (${degUnit()})`), lo),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
  });
}

async function timerDialog() {
  return dialog((close) => {
    let after = 0;
    const mins = el('input', { type: 'text', inputmode: 'numeric', value: 30, 'aria-label': 'Minutes' });
    return el('form', { onsubmit: (e) => { e.preventDefault(); const m = parseFloat(mins.value); if (m > 0) close({ seconds: Math.round(m * 60), after }); } },
      el('h3', {}, 'Set timer'),
      el('div', { class: 'num-input' }, el('button', { class: 'btn', type: 'button', onclick: () => (mins.value = Math.max(1, (parseFloat(mins.value) || 0) - 5)) }, '−'), mins, el('span', { class: 'muted' }, 'min'), el('button', { class: 'btn', type: 'button', onclick: () => (mins.value = (parseFloat(mins.value) || 0) + 5) }, '+')),
      el('div', { class: 'presets' }, [10, 15, 30, 45, 60, 90, 120].map((m) => el('button', { class: 'btn sm', type: 'button', onclick: () => (mins.value = m) }, `${m} min`))),
      el('div', { class: 'field' }, el('label', {}, 'When the timer ends'), segmented(AFTER, after, (v) => (after = v))),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Start')));
  });
}

export function renderCook(view) {
  const probes = el('div', { class: 'grid2' });
  const timerCard = el('div', { class: 'card' });
  const alerts = el('div', { class: 'list' });
  view.append(el('h2', {}, 'Timer'), timerCard, el('h2', {}, 'Probes'), probes, el('h2', {}, 'Recent alerts'), el('div', { class: 'card' }, alerts));

  const loadAlerts = () => api('/alerts?limit=10').then((evs) => {
    alerts.innerHTML = '';
    for (const e of evs.reverse()) alerts.append(el('div', { class: 'item' }, el('div', {}, el('div', {}, e.title), el('div', { class: 'meta' }, `${e.body} · ${new Date(e.ts * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}`))));
    if (!evs.length) alerts.append(el('div', { class: 'muted' }, 'No alerts yet'));
  }).catch(() => {});
  loadAlerts();

  let lastAlertGen = 0;
  const update = (s) => {
    if (!s) return;
    const t = s.timer;
    timerCard.innerHTML = '';
    if (t.running) {
      timerCard.append(el('div', { class: 'row between' },
        el('div', {}, el('div', { style: 'font-size:2rem;font-weight:700;font-variant-numeric:tabular-nums' }, fmtDur(t.remaining)), el('div', { class: 'muted', style: 'font-size:.8rem' }, `${t.paused ? 'Paused' : 'Running'} · ${AFTER.find((a) => a[0] === t.after)?.[1]}`)),
        el('div', { class: 'btnrow' }, el('button', { class: 'btn sm', onclick: () => cmd({ cmd: 'timer', op: t.paused ? 'resume' : 'pause' }) }, t.paused ? 'Resume' : 'Pause'), el('button', { class: 'btn sm ghost', onclick: () => cmd({ cmd: 'timer', op: 'cancel' }) }, 'Cancel'))));
      timerCard.append(el('div', { class: 'progress' }, el('div', { style: `width:${Math.max(0, Math.min(100, 100 - (t.remaining / t.duration) * 100))}%` })));
    } else {
      timerCard.append(el('button', { class: 'btn block', onclick: async () => { const r = await timerDialog(); if (r) cmd({ cmd: 'timer', op: 'start', ...r }); } }, 'Set a timer'));
    }

    probes.innerHTML = '';
    for (const p of s.probes) {
      if (!p.enabled || p.role === 'Aux') continue;
      const hit = p.target > 0 && p.valid && p.temp >= p.target;
      const eta = p.target > 0 && p.eta_s >= 0 ? `ETA ${fmtDur(p.eta_s)}` : '';
      probes.append(el('div', { class: `probe ${p.role === 'Primary' ? 'primary' : ''} ${p.valid ? '' : 'invalid'} ${hit ? 'hit' : ''}`, style: 'min-height:130px' },
        el('div', { class: 'name' }, el('span', {}, p.name), el('span', {}, p.limit_high || p.limit_low ? '⚠ alarm' : '')),
        el('div', { class: 'temp' }, p.valid ? fmtTemp(p.temp) : '—', el('small', {}, degUnit())),
        el('div', { class: 'tgt' }, p.target > 0 ? `Target ${fmtTemp(p.target)}${degUnit()} · ${AFTER.find((a) => a[0] === p.after)?.[1]}${eta ? ' · ' + eta : ''}` : 'No target'),
        el('div', { class: 'btnrow', style: 'margin-top:8px' },
          el('button', { class: 'btn sm', onclick: async (e) => { e.stopPropagation(); const r = await targetDialog(p); if (r) cmd({ cmd: 'target', label: p.label, ...r }); } }, p.target > 0 ? 'Change' : 'Set target'),
          el('button', { class: 'btn sm ghost', onclick: async (e) => { e.stopPropagation(); const r = await limitsDialog(p); if (r) cmd({ cmd: 'limits', label: p.label, ...r }); } }, 'Alarms'))));
    }
    if (PF.alertGen !== lastAlertGen) { lastAlertGen = PF.alertGen; loadAlerts(); }
  };
  update(PF.status);
  return onStatus(update);
}
