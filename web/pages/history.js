import { PF, el, api, onStatus, degUnit, segmented, toast, confirmDialog } from '../app.js';

const COLORS = ['#ff8a1f', '#5ac8fa', '#4cd964', '#ff2d55', '#af52de', '#ffcc00', '#34aadc'];

export function renderHistory(view) {
  let minutes = 15; try { minutes = Number(localStorage.getItem('pf.hist.minutes') || 15); } catch {}
  let live = true, plot = null, timer = null;
  const chartEl = el('div', { class: 'chart' });
  const header = el('div', { class: 'row between' },
    segmented([[15, '15m'], [60, '1h'], [180, '3h'], [720, '12h'], [1440, '24h']], minutes, (v) => { minutes = v; viewing = null; title.textContent = ''; try { localStorage.setItem('pf.hist.minutes', v); } catch {} load(); }),
    el('label', { class: 'row' }, el('input', { type: 'checkbox', checked: live, onchange: (e) => (live = e.target.checked) }), 'Live'));
  const stats = el('div', { class: 'grid2' });
  const cooks = el('div', { class: 'list' });
  let viewing = null; // cook file being viewed, or null for live
  const title = el('div', { class: 'muted', style: 'font-size:.85rem;margin:6px 0' });
  view.append(el('div', { class: 'row between', style: 'margin-bottom:8px' }, el('h2', { style: 'margin:0' }, 'History'), el('a', { class: 'btn sm', href: '/api/v1/cooklog', download: 'pifire-cooklog.json', title: 'Running cook, else the last cook: samples with controller terms, settings and learning state' }, 'Export analysis log')), el('div', { class: 'card' }, header, title, chartEl), stats,
    el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', onclick: async () => { if (await confirmDialog('Clear history?', 'Removes all stored samples.', 'Clear', true)) { await api('/history/clear', { body: {} }); load(); } } }, 'Clear history')),
    el('h2', {}, 'Cook files'), el('div', { class: 'card' }, cooks));

  async function loadCooks() {
    const list = await api('/cookfiles').catch(() => []);
    cooks.innerHTML = '';
    for (const c of list) {
      const m = c.metrics || {};
      cooks.append(el('div', { class: 'item' },
        el('div', { style: 'cursor:pointer', onclick: async () => { viewing = await api(`/cookfiles/${c.id}`); title.textContent = `Viewing ${viewing.name}`; render(viewing.history); } },
          el('div', {}, c.name), el('div', { class: 'meta' }, `${(m.duration_s / 3600).toFixed(1)} h · max ${Math.round(m.max_pit || 0)}${degUnit()} · ≈${((m.pellets_g || 0) / 453.6).toFixed(1)} lb`)),
        el('div', { class: 'btnrow' },
          el('a', { class: 'btn sm ghost', href: `/api/v1/cookfiles/${c.id}`, download: `${c.name.replace(/[^\w.-]+/g, '_')}.json` }, 'Download'),
          el('a', { class: 'btn sm ghost', href: `/api/v1/cookfiles/${c.id}/log`, download: `cooklog_${c.name.replace(/[^\w.-]+/g, '_')}.json`, title: 'Full analysis log: samples with controller terms, settings, learning state' }, 'Analysis log'),
          el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog('Delete cook file?', c.name, 'Delete', true)) { await api(`/cookfiles/${c.id}/delete`, { body: {} }); loadCooks(); } } }, 'Delete'))));
    }
    if (!list.length) cooks.append(el('div', { class: 'muted' }, 'Cook files are saved automatically when a cook ends.'));
  }
  loadCooks();

  async function load() {
    if (viewing) return;
    let h;
    try { h = await api(`/history?minutes=${minutes}`); } catch (e) { toast('History unavailable', true); return; }
    render(h);
  }
  function render(h) {
    const t = h.t;
    const series = [{ label: 'Time' }];
    const data = [t];
    const sp = h.setpoint.map((v) => (v > 0 ? v : null));
    let ci = 0;
    const names = Object.keys(h.probes);
    const primary = PF.status?.probes.find((p) => p.role === 'Primary')?.label;
    names.sort((a, b) => (a === primary ? -1 : b === primary ? 1 : 0));
    for (const label of names) {
      const color = COLORS[ci++ % COLORS.length];
      const pr = PF.status?.probes.find((p) => p.label === label);
      series.push({ label: pr?.name || label, stroke: color, width: label === primary ? 2.5 : 1.5, spanGaps: false, value: (u, v) => (v == null ? '—' : `${v.toFixed(0)}${degUnit()}`) });
      data.push(h.probes[label].temp);
      const tg = h.probes[label].target.map((v) => (v > 0 ? v : null));
      if (tg.some((v) => v != null)) { series.push({ label: `${pr?.name || label} target`, stroke: color, dash: [4, 4], width: 1, value: (u, v) => (v == null ? '—' : `${v.toFixed(0)}`) }); data.push(tg); }
    }
    if (sp.some((v) => v != null)) { series.push({ label: 'Set point', stroke: '#f4f4f5', dash: [6, 4], width: 1.2, value: (u, v) => (v == null ? '—' : `${v.toFixed(0)}`) }); data.push(sp); }

    const opts = {
      width: chartEl.clientWidth || 340, height: Math.max(240, Math.min(420, window.innerHeight * 0.45)),
      series, cursor: { drag: { x: true, y: false } },
      axes: [
        { stroke: '#9b9ca3', grid: { stroke: 'rgba(128,128,128,.15)' }, values: (u, vals) => vals.map((v) => new Date(v * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })) },
        { stroke: '#9b9ca3', grid: { stroke: 'rgba(128,128,128,.15)' }, size: 46, values: (u, vals) => vals.map((v) => `${v}°`) },
      ],
      scales: { x: { time: true }, y: { range: (u, min, max) => [Math.max(0, Math.floor((min - 10) / 25) * 25), Math.ceil((max + 10) / 25) * 25] } },
      legend: { live: true },
    };
    if (plot) plot.destroy();
    plot = new uPlot(opts, data, chartEl);
    stats.innerHTML = '';
    for (const label of names) {
      const arr = h.probes[label].temp.filter((v) => v != null);
      if (!arr.length) continue;
      const pr = PF.status?.probes.find((p) => p.label === label);
      /* A 48 h range is tens of thousands of samples per probe, and spreading that many arguments
         into Math.min blows the call stack on some engines. Walk it instead. */
      let lo = arr[0], hi = arr[0];
      for (const v of arr) { if (v < lo) lo = v; if (v > hi) hi = v; }
      stats.append(el('div', { class: 'card tight stat' }, el('div', { class: 'v' }, `${arr[arr.length - 1].toFixed(0)}${degUnit()}`), el('div', { class: 'l' }, `${pr?.name || label} · min ${lo.toFixed(0)} · max ${hi.toFixed(0)}`)));
    }
  }
  load();
  const ro = new ResizeObserver(() => { if (plot) plot.setSize({ width: chartEl.clientWidth, height: plot.height }); });
  ro.observe(chartEl);
  timer = setInterval(() => { if (live && document.visibilityState === 'visible') load(); }, minutes <= 60 ? 10000 : 60000);

  /* Coming back to the app must redraw at once. The interval skips every tick spent in the
     background, and iOS suspends timers in a backgrounded Home Screen app anyway, so without this
     the chart sits on whatever it last drew until a tick happens to land, which on the longer
     ranges is a minute away and after a suspend may be longer still. */
  const resume = () => { if (live && !viewing && document.visibilityState === 'visible') load(); };
  document.addEventListener('visibilitychange', resume);
  window.addEventListener('pageshow', resume);
  window.addEventListener('focus', resume);

  return () => {
    clearInterval(timer);
    document.removeEventListener('visibilitychange', resume);
    window.removeEventListener('pageshow', resume);
    window.removeEventListener('focus', resume);
    ro.disconnect();
    if (plot) plot.destroy();
  };
}
