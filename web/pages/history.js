import { PF, el, api, onStatus, degUnit, segmented, toast, confirmDialog } from '../app.js';

const COLORS = ['#ff8a1f', '#5ac8fa', '#4cd964', '#ff2d55', '#af52de', '#ffcc00', '#34aadc'];

export function renderHistory(view) {
  let minutes = Number(localStorage.getItem('pf.hist.minutes') || 15);
  let live = true, plot = null, timer = null;
  const chartEl = el('div', { class: 'chart' });
  const header = el('div', { class: 'row between' },
    segmented([[15, '15m'], [60, '1h'], [180, '3h'], [720, '12h'], [1440, '24h']], minutes, (v) => { minutes = v; localStorage.setItem('pf.hist.minutes', v); load(); }),
    el('label', { class: 'row' }, el('input', { type: 'checkbox', checked: live, onchange: (e) => (live = e.target.checked) }), 'Live'));
  const stats = el('div', { class: 'grid2' });
  view.append(el('div', { class: 'card' }, header, chartEl), stats,
    el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', onclick: async () => { if (await confirmDialog('Clear history?', 'Removes all stored samples.', 'Clear', true)) { await api('/history/clear', { body: {} }); load(); } } }, 'Clear history')));

  async function load() {
    let h;
    try { h = await api(`/history?minutes=${minutes}`); } catch (e) { toast('History unavailable', true); return; }
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
      stats.append(el('div', { class: 'card tight stat' }, el('div', { class: 'v' }, `${arr[arr.length - 1].toFixed(0)}${degUnit()}`), el('div', { class: 'l' }, `${pr?.name || label} · min ${Math.min(...arr).toFixed(0)} · max ${Math.max(...arr).toFixed(0)}`)));
    }
  }
  load();
  const ro = new ResizeObserver(() => { if (plot) plot.setSize({ width: chartEl.clientWidth, height: plot.height }); });
  ro.observe(chartEl);
  timer = setInterval(() => { if (live && document.visibilityState === 'visible') load(); }, minutes <= 60 ? 10000 : 60000);
  return () => { clearInterval(timer); ro.disconnect(); if (plot) plot.destroy(); };
}
