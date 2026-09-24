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

/* `slots.hopper` is where the hopper's own block goes: what the sensor reads, the two buttons that
   teach it the ends of its scale, and nothing about pellet brands. It used to sit inside the Loaded
   Pellets card, which is about which pellets are in the grill and how many have been burned -- a
   different subject that happens to be on the same page. A control belongs to the thing it acts on. */
export function renderPellets(view, slots = {}) {
  const current = el('div', { class: 'card' });
  const hopper = slots.hopper || el('div', { class: 'card' });
  const list = el('div', { class: 'list' });
  const log = el('div', { class: 'list' });
  if (!slots.hopper) view.append(el('h2', {}, 'Hopper'), hopper);
  view.append(el('h2', {}, 'Loaded Pellets'), current,
    el('div', { class: 'row between' }, el('h2', {}, 'Pellet Profiles'), el('button', { class: 'btn sm', onclick: async () => { const r = await profileDialog(); if (r) { await api('/pellets/profile', { body: r }).catch((e) => toast(e.message, true)); load(); } } }, 'Add')),
    el('div', { class: 'card' }, list), el('h2', {}, 'Log'), el('div', { class: 'card' }, log));

  async function load() {
    const d = await api('/pellets');
    current.innerHTML = '';
    const h = d.hopper;
    current.append(el('div', { class: 'row between' },
      el('div', {}, el('div', { style: 'font-size:1.2rem;font-weight:600' }, d.current.brand ? `${d.current.brand} ${d.current.wood}` : 'None selected'),
        el('div', { class: 'muted', style: 'font-size:.8rem' }, `≈ ${(d.current.est_usage_g / 453.6).toFixed(2)} lb (${d.current.est_usage_g.toFixed(0)} g) used since loading`))));

    hopper.innerHTML = '';
    /* A scale needs two distinct ends. Set both to the same distance, or set empty nearer than
       full, and the level stops reading altogether -- which is honest, but the page used to say
       only "—" and leave the reason in the log. */
    const fullCm = Number(PF.settings?.pelletlevel?.full), emptyCm = Number(PF.settings?.pelletlevel?.empty);
    const scale = emptyCm > fullCm;
    if (h.enabled && !scale) {
      hopper.append(el('div', { class: 'notice warn' },
        `Empty (${emptyCm} cm) has to be further from the sensor than full (${fullCm} cm), or there is no scale to read the level against. Measure the other end, or type the two distances below.`));
    }
    if (h.enabled) {
      const cal = async (as) => {
        const what = as === 'full' ? 'full' : 'empty';
        if (!await confirmDialog(`Call this ${what}?`,
          `The sensor takes a reading now and that distance becomes ${what === 'full' ? 'the top' : 'the bottom'} of the scale. Do it with the hopper actually ${what === 'full' ? 'filled' : 'empty'}: the number depends on where the sensor sits and how the pellets heap up, which is why it is measured rather than typed.`,
          `Set ${what}`)) return;
        try {
          await api('/pellets/calibrate', { body: { as } });
          toast(`Measuring ${what}…`);
          setTimeout(async () => { await slots.onCalibrated?.(); load(); }, 3000);
        } catch (e) { toast(e.message, true); }
      };
      hopper.append(
        el('div', { class: 'row between' },
          /* With no scale the percentage is the last one worked out against a scale that no longer
             exists, so it is not shown: a number contradicting the warning above it is worse than
             no number, and the reading in centimetres is still true. */
          el('div', { style: 'font-size:1.6rem;font-weight:600' }, scale && h.pct >= 0 ? `${h.pct}%` : '—'),
          el('div', { class: 'muted', style: 'font-size:.78rem;text-align:right' },
            h.cm > 0 ? `${h.cm.toFixed(1)} cm to the pellets` : 'no reading',
            el('div', {}, `full ${PF.settings?.pelletlevel?.full ?? '—'} cm · empty ${PF.settings?.pelletlevel?.empty ?? '—'} cm`))),
        el('div', { class: 'progress' }, el('div', { style: `width:${scale ? Math.max(0, h.pct) : 0}%` })),
        /* Two measurements, each taken with the hopper in the state being named, in the order you
           would do them: fill it and say so, run it out and say so. */
        el('div', { class: 'form-actions' },
          el('button', { class: 'btn sm ghost', onclick: async () => { await api('/pellets/check', { body: {} }); toast('Checking hopper…'); setTimeout(load, 2500); } }, 'Check Level Now'),
          el('button', { class: 'btn sm ghost', onclick: () => cal('full') }, 'Set Current As Full'),
          el('button', { class: 'btn sm ghost', onclick: () => cal('empty') }, 'Set Current As Empty')));
    }
    else hopper.append(el('p', { class: 'muted', style: 'font-size:.8rem;margin:0' }, 'No hopper sensor configured (Hardware setup \u2192 distance sensor).'));

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
