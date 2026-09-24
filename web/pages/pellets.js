import { PF, el, api, toast, dialog, confirmDialog } from '../app.js';

async function profileDialog(p = {}) {
  return dialog((close) => {
    const brand = el('input', { type: 'text', value: p.brand || '', required: true, placeholder: 'e.g. Bear Mountain' });
    const wood = el('input', { type: 'text', value: p.wood || '', placeholder: 'e.g. Hickory' });
    const rating = el('select', {}, [1, 2, 3, 4, 5].map((r) => el('option', { value: r, selected: (p.rating || 4) === r }, '★'.repeat(r))));
    const comments = el('textarea', { rows: 2 }, p.comments || '');
    return el('form', { onsubmit: (e) => { e.preventDefault(); close({ id: p.id, brand: brand.value.trim(), wood: wood.value.trim(), rating: Number(rating.value), comments: comments.value }); } },
      el('h3', {}, p.id ? 'Edit pellets' : 'New pellets'),
      el('div', { class: 'field' }, el('label', {}, 'Brand'), brand),
      el('div', { class: 'field' }, el('label', {}, 'Wood'), wood),
      el('div', { class: 'field' }, el('label', {}, 'Rating'), rating),
      el('div', { class: 'field' }, el('label', {}, 'Notes'), comments),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
  });
}

export function renderPellets(view) {
  const current = el('div', { class: 'card' });
  const list = el('div', { class: 'list' });
  const log = el('div', { class: 'list' });
  view.append(el('h2', {}, 'Loaded Pellets'), current,
    el('div', { class: 'row between' }, el('h2', {}, 'Pellet Profiles'), el('button', { class: 'btn sm', onclick: async () => { const r = await profileDialog(); if (r) { await api('/pellets/profile', { body: r }).catch((e) => toast(e.message, true)); load(); } } }, 'Add')),
    el('div', { class: 'card' }, list), el('h2', {}, 'Log'), el('div', { class: 'card' }, log));

  async function load() {
    const d = await api('/pellets');
    current.innerHTML = '';
    const h = d.hopper;
    current.append(el('div', { class: 'row between' },
      el('div', {}, el('div', { style: 'font-size:1.2rem;font-weight:600' }, d.current.brand ? `${d.current.brand} ${d.current.wood}` : 'None selected'), el('div', { class: 'muted', style: 'font-size:.8rem' }, `≈ ${(d.current.est_usage_g / 453.6).toFixed(2)} lb (${d.current.est_usage_g.toFixed(0)} g) used since loading`)),
      h.enabled ? el('div', { class: 'stat' }, el('div', { class: 'v' }, h.pct >= 0 ? `${h.pct}%` : '—'), el('div', { class: 'l' }, 'hopper')) : null));
    if (h.enabled) {
      const cal = async (as) => {
        const what = as === 'full' ? 'full' : 'empty';
        if (!await confirmDialog(`Call this ${what}?`,
          `The sensor takes a reading now and that distance becomes ${what === 'full' ? 'the top' : 'the bottom'} of the scale. Do it with the hopper actually ${what === 'full' ? 'filled' : 'empty'}: the number depends on where the sensor sits and how the pellets heap up, which is why it is measured rather than typed.`,
          `Set ${what}`)) return;
        try { await api('/pellets/calibrate', { body: { as } }); toast(`Measuring ${what}…`); setTimeout(load, 3000); }
        catch (e) { toast(e.message, true); }
      };
      current.append(
        el('div', { class: 'progress' }, el('div', { style: `width:${Math.max(0, h.pct)}%` })),
        el('div', { class: 'muted', style: 'font-size:.76rem;margin-top:6px' },
          `${h.cm > 0 ? `${h.cm.toFixed(1)} cm to the pellets. ` : ''}Full at ${PF.settings?.pelletlevel?.full ?? '—'} cm, empty at ${PF.settings?.pelletlevel?.empty ?? '—'} cm.`),
        /* Calibration is two measurements, taken when the hopper is in the state being named. The
           dismissive end of the scale is on the left and the committing one on the right nowhere
           here -- these are two equal actions, so they read in the order you would do them. */
        el('div', { class: 'form-actions' },
          el('button', { class: 'btn sm ghost', onclick: () => cal('empty') }, 'Set Current As Empty'),
          el('button', { class: 'btn sm ghost', onclick: () => cal('full') }, 'Set Current As Full'),
          el('button', { class: 'btn sm ghost', onclick: async () => { await api('/pellets/check', { body: {} }); toast('Checking hopper…'); setTimeout(load, 2500); } }, 'Check Level Now')));
    }
    else current.append(el('p', { class: 'muted', style: 'font-size:.8rem' }, 'No hopper sensor configured (Hardware setup → distance sensor).'));

    list.innerHTML = '';
    for (const p of d.profiles) {
      const isCur = p.id === d.current.id;
      list.append(el('div', { class: 'item' },
        el('div', {}, el('div', {}, `${p.brand} ${p.wood}`, isCur ? el('span', { class: 'pill', style: 'margin-left:8px' }, 'loaded') : null), el('div', { class: 'meta' }, `${'★'.repeat(p.rating)}${p.comments ? ' · ' + p.comments : ''}`)),
        el('div', { class: 'btnrow' },
          isCur ? null : el('button', { class: 'btn sm primary', onclick: async () => { await api('/pellets/load', { body: { id: p.id } }); toast('Pellets loaded'); load(); } }, 'Load'),
          el('button', { class: 'btn sm ghost', onclick: async () => { const r = await profileDialog(p); if (r) { await api('/pellets/profile', { body: r }); load(); } } }, 'Edit'),
          isCur ? null : el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Delete profile?', `${p.brand} ${p.wood}`, 'Delete', true)) { await api('/pellets/delete', { body: { id: p.id } }).catch((e) => toast(e.message, true)); load(); } } }, 'Delete'))));
    }
    log.innerHTML = '';
    for (const e of d.log) log.append(el('div', { class: 'item' }, el('div', {}, el('div', {}, `${e.text} — ${e.brand} ${e.wood}`), el('div', { class: 'meta' }, `${new Date(e.ts * 1000).toLocaleString()}${e.hopper_pct >= 0 ? ` · hopper ${e.hopper_pct}%` : ''}`))));
    if (!d.log.length) log.append(el('div', { class: 'muted' }, 'No entries yet'));
  }
  load().catch((e) => toast(e.message, true));
}
