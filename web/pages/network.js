import { PF, el, api, toast, dialog, confirmDialog } from '../app.js';

const bars = (s) => (s >= 70 ? '▂▄▆█' : s >= 50 ? '▂▄▆' : s >= 30 ? '▂▄' : '▂');

async function passwordDialog(ssid, secured) {
  return dialog((close) => {
    const inp = el('input', { type: 'password', autocomplete: 'off', placeholder: secured ? 'Wi-Fi password' : 'Open network — no password', disabled: !secured, minlength: secured ? 8 : 0, 'aria-label': 'Wi-Fi password' });
    const show = el('label', { class: 'row', style: 'font-size:.85rem' }, el('input', { type: 'checkbox', onchange: (e) => (inp.type = e.target.checked ? 'text' : 'password') }), 'Show password');
    const form = el('form', { onsubmit: (e) => { e.preventDefault(); close(secured ? inp.value : ''); } },
      el('h3', {}, `Join ${ssid}`),
      el('div', { class: 'field' }, inp), show,
      el('p', { class: 'muted', style: 'font-size:.82rem' }, 'While the grill switches networks its hotspot will drop. Reconnect your phone to your home Wi-Fi and open pifire.local, or come back to the hotspot if it reappears (wrong password).'),
      el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'), el('button', { class: 'btn primary', type: 'submit' }, 'Join')));
    if (secured) setTimeout(() => inp.focus(), 50);
    return form;
  });
}

export function renderNetwork(view, { captive = false } = {}) {
  const statusCard = el('div', { class: 'card' });
  const hotspotCard = el('div', { class: 'card' });
  const list = el('div', { class: 'list' });
  const scanBtn = el('button', { class: 'btn sm', onclick: () => scan(true) }, 'Rescan');
  view.append(...[
    captive ? el('div', { class: 'card' }, el('h3', {}, 'Welcome to PiFire'), el('p', { class: 'muted' }, 'Pick your home Wi-Fi network below. After it joins, open pifire.local from your phone or computer on that network.')) : null,
    el('h2', {}, 'Connection'), statusCard,
    captive ? null : el('h2', {}, 'Setup hotspot'), captive ? null : hotspotCard,
    el('div', { class: 'row between' }, el('h2', {}, 'Networks'), scanBtn),
    el('div', { class: 'card' }, list)].filter(Boolean));

  let timer;
  async function status() {
    let s;
    try { s = await api('/network/status'); } catch { return; }
    statusCard.innerHTML = '';
    const kv = el('div', { class: 'kv' });
    const stateLabel = { boot: 'Starting…', online: 'Connected', hotspot: 'Hotspot (setup mode)', connecting: 'Joining network…', offline: 'Not connected' }[s.state] || s.state;
    kv.append(el('div', {}, 'Status'), el('div', {}, stateLabel));
    if (s.ssid) kv.append(el('div', {}, 'Network'), el('div', {}, s.ssid));
    if (s.ip) kv.append(el('div', {}, 'Address'), el('div', {}, s.ip));
    if (s.signal) kv.append(el('div', {}, 'Signal'), el('div', {}, `${s.signal}% ${bars(s.signal)}`));
    kv.append(el('div', {}, 'Interface'), el('div', {}, s.iface));
    statusCard.append(kv);
    if (s.last_error) statusCard.append(el('div', { class: 'banner', style: 'margin:10px 0 0' }, s.last_error));
    if (s.ssid && s.state === 'online') statusCard.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn sm ghost', onclick: async () => { if (await confirmDialog(`Forget ${s.ssid}?`, 'The grill will disconnect and may start its setup hotspot.', 'Forget', true)) { await api('/network/forget', { body: { ssid: s.ssid } }); status(); } } }, 'Forget network')));

    hotspotCard.innerHTML = '';
    hotspotCard.append(el('div', { class: 'kv' }, el('div', {}, 'Hotspot name'), el('div', {}, s.hotspot.ssid), el('div', {}, 'Password'), el('div', {}, s.hotspot.password), el('div', {}, 'Address'), el('div', {}, '10.42.0.1')),
      el('p', { class: 'muted', style: 'font-size:.82rem' }, 'The hotspot starts automatically when no known network is found after boot. Change the password under Settings → Network.'),
      el('div', { class: 'form-actions' }, el('button', { class: 'btn sm' + (s.hotspot.active ? '' : ' primary'), onclick: async () => { if (s.hotspot.active || await confirmDialog('Start the setup hotspot?', 'Your current Wi-Fi connection will drop.', 'Start hotspot')) { await api('/network/hotspot', { body: { on: !s.hotspot.active } }); setTimeout(status, 1500); } } }, s.hotspot.active ? 'Stop hotspot' : 'Start hotspot')));
  }
  async function scan(rescan) {
    scanBtn.disabled = true;
    list.innerHTML = ''; list.append(el('div', { class: 'muted' }, 'Scanning…'));
    try {
      const nets = await api(`/network/scan?rescan=${rescan ? 1 : 0}`);
      list.innerHTML = '';
      for (const n of nets) {
        list.append(el('div', { class: 'item', style: 'cursor:pointer', onclick: async () => {
          if (n.active) return;
          const psk = await passwordDialog(n.ssid, !!n.security);
          if (psk === undefined) return;
          try { await api('/network/connect', { body: { ssid: n.ssid, psk } }); toast(`Joining ${n.ssid}…`); } catch (e) { toast(e.message, true); }
          status();
        } },
          el('div', {}, el('div', {}, n.ssid, n.active ? el('span', { class: 'pill', style: 'margin-left:8px' }, 'connected') : null), el('div', { class: 'meta' }, n.security || 'Open')),
          el('div', { class: 'muted' }, `${n.signal}% ${bars(n.signal)}`)));
      }
      if (!nets.length) list.append(el('div', { class: 'muted' }, 'No networks found'));
    } catch (e) { list.innerHTML = ''; list.append(el('div', { class: 'muted' }, 'Scan failed')); }
    scanBtn.disabled = false;
  }
  status(); scan(false);
  timer = setInterval(status, 5000);
  return () => clearInterval(timer);
}
