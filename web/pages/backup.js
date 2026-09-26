import { PF, el, api, patchSettings, toast, confirmDialog, dialog, pushScreen, itemRow, iconBtn, addRow, screenActions } from '../app.js';
import { fieldInput, readField } from './settings.js';
import { icon as lucide } from '../icons.js';

/* One file, the whole grill, to every place it is wanted.
 *
 * Four things on the page, in the order they are wanted: what the last backup was and one button
 * to make another; the LOCATIONS it goes to, a manager list like the probes and the pellet
 * profiles -- add several, each with its own kind, fields and switch, and every backup goes to all
 * of them, as Home Assistant's backup agents do; when; and what is out there, each row saying
 * which locations hold it and offering the way back. */

const KINDS = [
  { type: 'gdrive', name: 'Google Drive', sub: 'Sign in with Google from your phone', icon: 'cloud', color: '#34a853' },
  { type: 'onedrive', name: 'OneDrive', sub: 'Sign in with Microsoft from your phone', icon: 'cloud', color: '#0a84ff' },
  { type: 'smb', name: 'Network share', sub: 'A NAS or a computer on the network (SMB)', icon: 'network', color: '#bf5af2' },
  { type: 'folder', name: 'Folder on the grill', sub: 'A USB stick, or a share already mounted', icon: 'folder', color: '#ffd60a' },
];
const kindOf = (type) => KINDS.find((k) => k.type === type) || KINDS[3];
const SCHED = [['off', 'Never'], ['daily', 'Every day'], ['weekly', 'Every week'], ['monthly', 'Every month']];
const DAYS = [[0, 'Sunday'], [1, 'Monday'], [2, 'Tuesday'], [3, 'Wednesday'], [4, 'Thursday'], [5, 'Friday'], [6, 'Saturday']];
const HOURS = Array.from({ length: 24 }, (_, h) => [h, `${((h + 11) % 12) + 1}:00 ${h < 12 ? 'AM' : 'PM'}`]);

const fmtMB = (b) => b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`;
const fmtWhen = (ts) => {
  if (!ts) return '';
  const d = new Date(ts * 1000);
  return d.toLocaleString([], { weekday: 'short', day: 'numeric', month: 'short', hour: 'numeric', minute: '2-digit' });
};
const newId = () => `loc-${Date.now().toString(36)}${Math.random().toString(36).slice(2, 5)}`;

export function renderBackup(view) {
  const cfg = () => PF.settings?.backup || {};
  const locs = () => cfg().locations || [];
  let st = null;
  const locState = (id) => (st?.locations || []).find((l) => l.id === id) || {};

  /* ---- what happened last, and the button ---- */
  const status = el('div', { class: 'card' });
  const drawStatus = () => {
    status.innerHTML = '';
    const last = st?.last || {};
    const kv = el('div', { class: 'kv' });
    kv.append(el('div', {}, 'Last backup'), el('div', {}, last.ts ? `${fmtWhen(last.ts)} · ${fmtMB(last.size || 0)}` : 'None yet'));
    if (last.ts) {
      const res = last.results || {};
      const said = locs().filter((l) => res[l.id]).map((l) => res[l.id].ok ? l.name : `${l.name} failed`);
      if (said.length) kv.append(el('div', {}, 'Sent to'), el('div', { class: last.ok ? '' : 'warn-ink' }, said.join(', ')));
      for (const l of locs()) if (res[l.id] && !res[l.id].ok) kv.append(el('div', {}, l.name), el('div', { class: 'warn-ink' }, res[l.id].message));
    }
    kv.append(el('div', {}, 'Next'), el('div', {}, st?.next_ts ? fmtWhen(st.next_ts) : !locs().some((l) => l.enabled !== false) ? 'Add a location first' : 'No schedule'));
    status.append(kv);
    if (st?.message && (st.busy || st.error)) status.append(el('p', { class: `help${st.error ? ' warn-ink' : ''}`, style: 'margin-top:8px' }, st.message));
    status.append(el('div', { class: 'form-actions' },
      el('button', { class: 'btn primary', type: 'button', disabled: !!st?.busy || !locs().some((l) => l.enabled !== false),
        onclick: async () => { try { await api('/backup/run', { body: {} }); poll(); } catch (e) { toast(e.message, true); } } },
        lucide('archive', 'ic btn-ic'), el('span', {}, st?.busy ? 'Working…' : 'Back Up Now'))));
  };

  /* ---- the locations ---- */
  const locList = el('div', { class: 'ios-list' });
  const saveLocs = async (next) => {
    try { await patchSettings('backup', { locations: next }); }
    catch (e) { toast(e.message, true); return false; }
    try { st = await api('/backup'); } catch { /* keep what we had */ }
    return true;
  };
  const describe = (l) => {
    const k = kindOf(l.type);
    const ls = locState(l.id);
    if (l.type === 'gdrive' || l.type === 'onedrive') return `${k.name} · ${ls.connected ? 'Connected' : 'Not connected'}`;
    if (l.type === 'smb') return `${k.name} · ${l.host && l.share ? `//${l.host}/${l.share}/${l.path || 'PiFire'}` : 'not set up'}`;
    return `${k.name} · ${l.folder || 'not set up'}`;
  };
  const drawLocs = () => {
    locList.innerHTML = '';
    for (const l of locs()) {
      const k = kindOf(l.type);
      const sw = el('label', { class: 'switch', onclick: (e) => e.stopPropagation() },
        el('input', { type: 'checkbox', checked: l.enabled !== false, onchange: async (e) => {
          const next = locs().map((x) => (x.id === l.id ? { ...x, enabled: e.target.checked } : x));
          if (await saveLocs(next)) drawAll();
        } }), el('span'));
      locList.append(itemRow({
        icon: k.icon, color: k.color, title: l.name || k.name, meta: describe(l),
        onclick: () => editLoc(l, false),
        actions: [sw],
      }));
    }
    if (!locs().length) locList.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'Nowhere yet. Every backup goes to every location switched on here.'));
    locList.append(addRow('Add Location', pickKind));
  };

  /* what kind: the same picker as Add condition, one row per kind with a line saying what it needs */
  const pickKind = () => dialog((close) => el('div', { class: 'sheet' },
    el('div', { class: 'sheet-head' }, el('h3', {}, 'Add location')),
    el('div', { class: 'sheet-body pick-list' },
      el('div', { class: 'ios-list' }, KINDS.map((k) => el('button', { class: 'irow kind-row', type: 'button',
        onclick: () => { close(); editLoc({ id: newId(), type: k.type, name: k.name, enabled: true }, true); } },
        el('span', { class: 'cc-glyph' }, lucide(k.icon)),
        el('span', { class: 'body' }, el('span', { class: 't' }, k.name), el('span', { class: 's' }, k.sub)))))),
    el('div', { class: 'form-actions' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close() }, 'Cancel'))));

  /* one location: its fields over a footer that saves or deletes it alone */
  const editLoc = (loc0, isNew) => {
    const loc = { ...loc0 };
    const k = kindOf(loc.type);
    return pushScreen((close) => {
      const wrap = el('div', { class: 'sheet' });
      const body = el('div');
      const form = el('form', { onsubmit: (e) => e.preventDefault() });
      let ready = false;
      const touched = () => { if (ready) wrap.dispatchEvent(new CustomEvent('pf-dirty', { bubbles: true })); };
      form.oninput = touched; form.onchange = touched;
      const fields = [{ path: 'name', label: 'Name', help: 'How this location is listed', type: 'text' }];
      if (loc.type === 'gdrive' || loc.type === 'onedrive') {
        /* With the project's own client shipped, connecting is one tap and a code. The fields
           for a client of your own are there for the few who want them, behind a fold, and
           are the only way when a build ships without one. */
        const builtin = !!st?.clients?.[loc.type];
        const own = [];
        if (loc.type === 'gdrive') own.push({ path: 'client_id', label: 'Client ID', type: 'text' }, { path: 'client_secret', label: 'Client secret', type: 'password' });
        else own.push({ path: 'client_id', label: 'Application (client) ID', type: 'text' });
        if (loc.type === 'gdrive') own.push({ path: 'cloud_folder', label: 'Folder in Drive', help: 'Made if it is not there', type: 'text' });
        if (builtin) {
          const adv = el('div', { class: 'fold-body' });
          for (const f of own) adv.append(fieldInput(f, loc[f.path] ?? (f.path === 'cloud_folder' ? 'PiFire Backups' : '')));
          adv.prepend(el('p', { class: 'help', style: 'padding:6px 0' }, 'Leave the client blank to use PiFire\u2019s own. Only fill this in if you have registered a client of your own.'));
          form.append(el('details', { class: 'fold', open: !!loc.client_id }, el('summary', {}, el('span', {}, 'Advanced')), adv));
          fields.push(...own);
        } else {
          body.append(el('p', { class: 'help', style: 'padding:6px 0' }, loc.type === 'gdrive'
            ? 'This build ships without a Google client, so one of your own is needed: at console.cloud.google.com create a project, turn on the Google Drive API, and under Credentials add an OAuth client of type "TVs and Limited Input devices".'
            : 'This build ships without a Microsoft client, so one of your own is needed: at portal.azure.com register an application for personal Microsoft accounts, allow public client flows, and copy its Application (client) ID.'));
          fields.push(...own);
        }
      } else if (loc.type === 'smb') {
        if (st && st.smbclient === false) body.append(el('div', { class: 'notice warn' }, el('span', {}, 'smbclient is not installed on the grill. Run: sudo apt install smbclient')));
        fields.push({ path: 'host', label: 'Host', help: 'Name or address of the NAS or computer. A Tailscale name or 100.x address works too', type: 'text' },
          { path: 'share', label: 'Share', type: 'text' },
          { path: 'path', label: 'Folder in the share', help: 'Made if it is not there', type: 'text' },
          { path: 'user', label: 'User', type: 'text' },
          { path: 'password', label: 'Password', type: 'password' });
      } else {
        fields.push({ path: 'folder', label: 'Folder', help: 'A path on the grill: a USB stick, or a share already mounted there', type: 'text' });
      }
      for (const f of fields) {
        if (form.querySelector(`[name="${f.path}"]`)) continue;   /* already placed, under Advanced */
        const node = fieldInput(f, loc[f.path] ?? (f.path === 'cloud_folder' ? 'PiFire Backups' : f.path === 'path' ? 'PiFire' : ''));
        const fold = form.querySelector('details.fold');
        if (fold) form.insertBefore(node, fold); else form.append(node);
      }
      body.append(form);
      /* the machines on the tailnet, offered under the host field: the NAS is reachable by its
         Tailscale name from anywhere the grill is, not only from the kitchen */
      if (loc.type === 'smb') {
        api('/network/tailscale').then((ts) => {
          const peers = (ts.peers || []).filter((p) => p.os !== 'iOS' && p.os !== 'android');
          const host = form.querySelector('[name="host"]');
          if (!peers.length || !host) return;
          const dl = el('datalist', { id: `hosts-${loc.id}` }, ...peers.flatMap((p) => [
            el('option', { value: p.name, label: `${p.name} \u00b7 Tailscale${p.online ? '' : ' (offline)'}` }),
            p.ip ? el('option', { value: p.ip, label: `${p.name} \u00b7 ${p.ip}` }) : null].filter(Boolean)));
          host.setAttribute('list', dl.id);
          host.after(dl);
        }).catch(() => {});
      }
      const read = () => { const out = { ...loc }; for (const f of fields) out[f.path] = readField(f, form); return out; };
      const persist = async (next) => {
        const all = locs();
        const list = all.some((x) => x.id === next.id) ? all.map((x) => (x.id === next.id ? next : x)) : [...all, next];
        return saveLocs(list);
      };
      /* the cloud sign-in and the connection test act on the saved location, so they save first */
      if (loc.type === 'gdrive' || loc.type === 'onedrive') {
        const ls = locState(loc.id);
        body.append(el('div', { class: 'field inline' },
          el('div', {}, el('label', {}, loc.type === 'gdrive' ? 'Google account' : 'Microsoft account'), el('div', { class: 'help' }, ls.connected ? 'Connected' : st?.pending?.loc === loc.id ? 'Waiting for the code to be entered' : 'Not connected')),
          ls.connected
            ? el('button', { class: 'btn sm ghost', type: 'button', onclick: async () => {
                if (!await confirmDialog(`Disconnect ${loc.name || k.name}?`, 'Backups stop going there until it is connected again. Nothing there is removed.', 'Disconnect', true)) return;
                try { st = await api('/backup/disconnect', { body: { id: loc.id } }); close(undefined); drawAll(); } catch (e) { toast(e.message, true); } } }, 'Disconnect')
            : el('button', { class: 'btn sm primary', type: 'button', onclick: async () => {
                let next; try { next = read(); } catch (e) { toast(e.message, true); return; }
                if (!await persist(next)) return;
                await connectCloud(next);
                close(undefined); drawAll();
              } }, st?.pending?.loc === loc.id ? 'Show code' : loc.type === 'gdrive' ? 'Sign in with Google' : 'Sign in with Microsoft')));
      } else {
        body.append(el('div', { class: 'field inline' },
          el('div', {}, el('label', {}, 'Browse'), el('div', { class: 'help' }, loc.type === 'smb' ? 'Pick the share and folder from the host' : 'Pick the folder on the grill')),
          el('button', { class: 'btn sm', type: 'button', onclick: async () => {
            let cur; try { cur = read(); } catch (e) { toast(e.message, true); return; }
            if (loc.type === 'smb' && !cur.host) { toast('Enter the host first', true); return; }
            const picked = await browse(loc.type, loc.type === 'smb' ? { host: cur.host, share: cur.share, user: cur.user, password: cur.password } : {}, loc.type === 'smb' ? cur.path : (cur.folder || '/'));
            if (!picked) return;
            if (loc.type === 'smb') { form.querySelector('[name="share"]').value = picked.share; form.querySelector('[name="path"]').value = picked.path; }
            else form.querySelector('[name="folder"]').value = picked.path;
            touched();
          } }, 'Browse\u2026')));
        body.append(el('div', { class: 'field inline' },
          el('div', {}, el('label', {}, 'Connection'), el('div', { class: 'help' }, 'Saves, then checks the grill can reach it')),
          el('button', { class: 'btn sm', type: 'button', onclick: async (e) => {
            const b = e.currentTarget; b.disabled = true;
            try { const next = read(); if (await persist(next)) { const r = await api('/backup/test', { body: { id: next.id } }); toast(r.message); } }
            catch (err) { toast(err.message, true); }
            b.disabled = false;
          } }, 'Test')));
      }
      wrap.append(el('div', { class: 'sheet-body' }, body),
        screenActions({
          onDelete: isNew ? null : async () => {
            if (!await confirmDialog(`Remove ${loc.name || k.name}?`, 'Backups already there are left where they are.', 'Remove', true)) return;
            if (await saveLocs(locs().filter((x) => x.id !== loc.id))) { close(undefined); drawAll(); }
          },
          deleteTitle: 'Remove location',
          onCancel: () => close(undefined),
          onSave: async () => {
            let next; try { next = read(); } catch (e) { toast(e.message, true); return; }
            if (await persist(next)) { toast('Saved'); close(next); drawAll(); }
          },
          dirty: isNew,
        }));
      setTimeout(() => { ready = true; }, 0);
      return wrap;
    }, { title: isNew ? `New ${k.name}` : (loc0.name || k.name), back: 'Backup' });
  };

  /* a cloud sign-in: a code to type into the service's device page on the phone */
  const connectCloud = async (loc) => {
    try { st = await api('/backup/connect', { body: { id: loc.id } }); } catch (e) { toast(e.message, true); return; }
    const p = st.pending;
    if (!p || p.loc !== loc.id) return;
    let timer = null;
    await dialog((close) => {
      const code = el('div', { class: 'gcode' }, p.user_code);
      const state = el('p', { class: 'help' }, 'Waiting for you to enter it…');
      const tick = async () => {
        try { st = await api('/backup'); } catch { return; }
        if (locState(loc.id).connected) { toast(`${loc.name} connected`); close(); return; }
        if (!st.pending || st.pending.loc !== loc.id) { state.textContent = st.message || 'The code was not used'; return; }
        timer = setTimeout(tick, 3000);
      };
      timer = setTimeout(tick, 3000);
      return el('div', {},
        el('h3', {}, `Connect ${loc.name}`),
        el('p', { class: 'muted' }, 'On your phone, open the link and enter this code.'),
        code,
        el('a', { class: 'btn primary block', href: p.url, target: '_blank', rel: 'noopener' }, p.url.replace(/^https?:\/\/(www\.)?/, '')),
        state,
        el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close() }, 'Close')));
    });
    clearTimeout(timer);
  };

  /* ---- when ---- */
  const whenCard = el('div', { class: 'card' });
  const drawWhen = () => {
    whenCard.innerHTML = '';
    const form = el('form');
    const sched = cfg().schedule ?? 'weekly';
    const fields = [{ path: 'schedule', label: 'Back up', type: 'select', options: SCHED }];
    if (sched === 'weekly') fields.push({ path: 'weekday', label: 'On', type: 'select', options: DAYS });
    if (sched === 'monthly') fields.push({ path: 'monthday', label: 'On day', help: '1 to 28', type: 'int', min: 1, max: 28 });
    if (sched !== 'off') fields.push({ path: 'hour', label: 'At', type: 'select', options: HOURS });
    fields.push({ path: 'keep', label: 'Keep the newest', help: 'Older backups at each location are removed', type: 'int', min: 1, max: 100 });
    for (const f of fields) {
      const node = fieldInput(f, cfg()[f.path] ?? (f.path === 'keep' ? 8 : f.path === 'hour' ? 3 : f.path === 'monthday' ? 1 : 0));
      form.append(node);
      if (f.path === 'schedule') node.querySelector('select').onchange = async (e) => { await save({ schedule: e.target.value }); };
    }
    form.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
    form.onsubmit = async (e) => {
      e.preventDefault();
      const patch = {};
      try { for (const f of fields) patch[f.path] = readField(f, form); } catch (err) { toast(err.message, true); return; }
      await save(patch);
    };
    whenCard.append(form);
  };

  /* ---- what is out there ---- */
  const listHead = el('h2', {}, 'Backups');
  const listCard = el('div', { class: 'ios-list' });
  const drawList = async () => {
    const any = locs().some((l) => l.enabled !== false);
    listHead.hidden = !any; listCard.hidden = !any;
    listCard.innerHTML = '';
    if (!any) return;
    listCard.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'Looking…'));
    let files = [], warning = '';
    try { const r = await api('/backup/list'); files = r.files || []; warning = r.warning || ''; listCard.innerHTML = ''; }
    catch (e) { listCard.innerHTML = ''; listCard.append(el('p', { class: 'help warn-ink', style: 'padding:var(--sp-3)' }, e.message)); }
    if (warning) listCard.append(el('p', { class: 'help warn-ink', style: 'padding:var(--sp-3) var(--sp-3) 0' }, warning));
    for (const f of files) {
      const where = (f.locations || []).map((id) => locs().find((l) => l.id === id)?.name || id).join(', ');
      listCard.append(itemRow({
        icon: 'archive', color: '#30d158',
        title: fmtWhen(f.ts) || f.name,
        meta: `${fmtMB(f.size || 0)} · ${where}`,
        chevron: false,
        onclick: () => restoreNamed(f),
        actions: [iconBtn('rotate-ccw', `Restore ${f.name}`, { onclick: (e) => { e.stopPropagation(); restoreNamed(f); } })],
      }));
    }
    if (!files.length && !warning) listCard.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'Nothing there yet.'));
    listCard.append(el('button', { class: 'btn ghost block', type: 'button', onclick: restoreFile }, lucide('upload', 'ic btn-ic'), el('span', {}, 'Restore from a file')));
  };

  const restoreWarn = 'Replaces the settings, tuning library, learning, recipes, pellets, notification rules and saved cooks on this grill with the backup’s. The grill restarts. The grill must be stopped.';
  const restoreNamed = async (f) => {
    if (!await confirmDialog(`Restore ${fmtWhen(f.ts) || f.name}?`, restoreWarn, 'Restore', true)) return;
    try { await api('/backup/restore', { body: { name: f.name } }); toast('Restoring — the grill is restarting'); poll(); }
    catch (e) { toast(e.message, true); }
  };
  const restoreFile = () => {
    const inp = el('input', { type: 'file', accept: '.gz,.tgz,application/gzip,application/x-gzip' });
    inp.onchange = async () => {
      const file = inp.files?.[0];
      if (!file) return;
      if (!await confirmDialog(`Restore ${file.name}?`, restoreWarn, 'Restore', true)) return;
      try {
        const r = await fetch('/api/v1/backup/restore', { method: 'POST', headers: { 'Content-Type': 'application/gzip' }, body: file });
        const j = await r.json().catch(() => ({}));
        if (!r.ok) throw new Error(j.error || `HTTP ${r.status}`);
        toast('Restoring — the grill is restarting'); poll();
      } catch (e) { toast(e.message, true); }
    };
    inp.click();
  };

  /* Pick a folder rather than type it: a list of what is there, a row up, and one button that
     takes the folder you are in. For a share with no share named yet, the shares of the host
     come first. Credentials travel with the request; nothing is saved until Save. */
  const browse = (kind, creds, start) => dialog((close) => {
    const head = el('div', { class: 'browse-path' });
    const list = el('div', { class: 'ios-list' });
    const sub = el('input', { type: 'text', placeholder: 'New subfolder (optional)' });
    let path = start || '';
    let share = creds.share || '';
    let atShares = kind === 'smb' && !share;
    const load = async () => {
      list.innerHTML = '';
      list.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'Looking\u2026'));
      try {
        const r = await api('/backup/browse', { body: { type: kind, path, ...creds, share } });
        list.innerHTML = '';
        if (atShares) {
          head.textContent = `//${creds.host}`;
          for (const sname of r.shares || []) list.append(row('network', sname, () => { share = sname; atShares = false; path = ''; load(); }));
          if (!(r.shares || []).length) list.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'No shares listed. The host may not publish them; type the share name instead.'));
          return;
        }
        path = r.path || '';
        head.textContent = kind === 'smb' ? `//${creds.host}/${share}/${path}` : path;
        if (r.parent !== undefined) list.append(row('chevron-left', 'Up', () => { path = r.parent; load(); }));
        else if (kind === 'smb') list.append(row('chevron-left', 'Shares', () => { atShares = true; share = ''; load(); }));
        for (const d of r.dirs || []) list.append(row('folder', d, () => { path = kind === 'smb' ? (path ? `${path}/${d}` : d) : (path === '/' ? `/${d}` : `${path}/${d}`); load(); }));
        if (!(r.dirs || []).length) list.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'No folders here.'));
      } catch (e) { list.innerHTML = ''; list.append(el('p', { class: 'help warn-ink', style: 'padding:var(--sp-3)' }, e.message)); }
    };
    const row = (ic, text, onclick) => el('button', { class: 'irow kind-row', type: 'button', onclick },
      el('span', { class: 'cc-glyph' }, lucide(ic)), el('span', { class: 'body' }, el('span', { class: 't' }, text)));
    load();
    return el('div', { class: 'sheet' },
      el('div', { class: 'sheet-head' }, el('h3', {}, kind === 'smb' ? 'Browse the share' : 'Browse the grill')),
      el('div', { class: 'sheet-body pick-list' }, head, list, el('div', { class: 'field' }, sub)),
      el('div', { class: 'form-actions' },
        el('button', { class: 'btn ghost', type: 'button', onclick: () => close(undefined) }, 'Cancel'),
        el('button', { class: 'btn primary', type: 'button', disabled: false, onclick: () => {
          if (atShares) { toast('Pick a share first', true); return; }
          const extra = sub.value.trim().replace(/^\/+|\/+$/g, '');
          const full = extra ? (path && path !== '/' ? `${path}/${extra}` : (kind === 'smb' ? extra : `/${extra}`)) : path;
          close({ share, path: full });
        } }, 'Use this folder')));
  });

  const save = async (patch) => {
    try { await patchSettings('backup', patch); toast('Saved'); }
    catch (e) { toast(e.message, true); return; }
    try { st = await api('/backup'); } catch { /* keep what we had */ }
    drawAll();
  };
  const drawAll = () => { drawStatus(); drawLocs(); drawWhen(); drawList(); };

  let pollT = null;
  const poll = async () => {
    clearTimeout(pollT);
    try { st = await api('/backup'); } catch { return; }
    drawStatus();
    if (st.busy) pollT = setTimeout(poll, 1500); else drawList();
  };

  view.append(status, el('h2', {}, 'Locations'), locList, el('h2', {}, 'When'), whenCard, listHead, listCard);
  api('/backup').then((s) => { st = s; drawAll(); if (s.busy) poll(); }).catch((e) => toast(e.message, true));
  return () => clearTimeout(pollT);
}
