import { PF, el, onStatus, fmtTemp, degUnit, toast } from '../app.js';

// Probe targets, timers and doneness presets arrive with the notification engine (phase 5).
// Until then this page shows live probe detail.
export function renderCook(view) {
  const list = el('div', { class: 'list' });
  view.append(el('h2', {}, 'Probes'), el('div', { class: 'card' }, list),
    el('div', { class: 'card muted' }, 'Probe targets, alerts, timers and doneness presets are coming in the next build.'));
  const update = (s) => {
    if (!s) return;
    list.innerHTML = '';
    for (const p of s.probes) {
      list.append(el('div', { class: 'item' },
        el('div', {}, el('div', {}, p.name), el('div', { class: 'meta' }, `${p.role} · ${p.device}/${p.port}${p.ohms ? ` · ${p.ohms} Ω` : ''}`)),
        el('div', { style: 'font-size:1.3rem;font-weight:600' }, p.valid ? `${fmtTemp(p.temp)}${degUnit()}` : '—')));
    }
  };
  update(PF.status);
  return onStatus(update);
}
