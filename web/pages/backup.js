import { PF, el, api, patchSettings, toast, confirmDialog, dialog, itemRow, iconBtn } from '../app.js';
import { fieldInput, readField } from './settings.js';
import { icon as lucide } from '../icons.js';

/* One file, the whole grill, somewhere else.
 *
 * Three things on the page, in the order they are wanted: what the last backup was and one button
 * to make another; where they go and when; and what is at the destination, each row a way back.
 * The destination's own fields appear under its choice rather than every destination's fields at
 * once, because a grill backs up to one place. */

const DESTS = [['off', 'Off'], ['gdrive', 'Google Drive'], ['smb', 'Network share (SMB)'], ['folder', 'Folder on the grill']];
const SCHED = [['off', 'Never'], ['daily', 'Every day'], ['weekly', 'Every week'], ['monthly', 'Every month']];
const DAYS = [[0, 'Sunday'], [1, 'Monday'], [2, 'Tuesday'], [3, 'Wednesday'], [4, 'Thursday'], [5, 'Friday'], [6, 'Saturday']];
const HOURS = Array.from({ length: 24 }, (_, h) => [h, `${((h + 11) % 12) + 1}:00 ${h < 12 ? 'AM' : 'PM'}`]);
const DEST_SAID = { gdrive: 'Google Drive', smb: 'the network share', folder: 'the folder' };

const fmtMB = (b) => b >= 1048576 ? `${(b / 1048576).toFixed(1)} MB` : b >= 1024 ? `${Math.round(b / 1024)} KB` : `${b} B`;
const fmtWhen = (ts) => {
  if (!ts) return '';
  const d = new Date(ts * 1000);
  return d.toLocaleString([], { weekday: 'short', day: 'numeric', month: 'short', hour: 'numeric', minute: '2-digit' });
};

export function renderBackup(view) {
  const get = (path, dflt) => path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), PF.settings?.backup) ?? dflt;

  /* ---- what happened last, and the button ---- */
  const status = el('div', { class: 'card' });
  let st = null;
  const drawStatus = () => {
    status.innerHTML = '';
    const last = st?.last || {};
    const kv = el('div', { class: 'kv' });
    kv.append(el('div', {}, 'Last backup'), el('div', {}, last.ts ? `${fmtWhen(last.ts)} · ${fmtMB(last.size || 0)}` : 'None yet'));
    if (last.ts && !last.ok) kv.append(el('div', {}, 'Result'), el('div', { class: 'warn-ink' }, last.message || 'failed'));
    kv.append(el('div', {}, 'Next'), el('div', {}, st?.next_ts ? fmtWhen(st.next_ts) : st?.destination === 'off' ? 'Nowhere to send it yet' : 'No schedule'));
    status.append(kv);
    if (st?.message && (st.busy || st.error)) status.append(el('p', { class: `help${st.error ? ' warn-ink' : ''}`, style: 'margin-top:8px' }, st.message));
    status.append(el('div', { class: 'form-actions' },
      el('button', { class: 'btn primary', type: 'button', disabled: !!st?.busy || st?.destination === 'off',
        onclick: async () => { try { await api('/backup/run', { body: {} }); poll(); } catch (e) { toast(e.message, true); } } },
        lucide('archive', 'ic btn-ic'), el('span', {}, st?.busy ? 'Working…' : 'Back Up Now'))));
  };

  /* ---- where ---- */
  const whereCard = el('div', { class: 'card' });
  const drawWhere = () => {
    whereCard.innerHTML = '';
    const form = el('form');
    const dest = get('destination', 'off');
    const destField = { path: 'destination', label: 'Send backups to', type: 'select', options: DESTS };
    const destInput = fieldInput(destField, dest);
    destInput.querySelector('select').onchange = async (e) => { await save({ destination: e.target.value }); };
    form.append(destInput);
    const fields = [];
    if (dest === 'gdrive') {
      const gd = st?.gdrive || {};
      form.append(el('p', { class: 'help', style: 'padding:6px 0' },
        'Needs your own Google API client (5 minutes, once): at console.cloud.google.com create a project, turn on the Google Drive API, and under Credentials add an OAuth client of type "TVs and Limited Input devices". PiFire only ever sees the files it made.'));
      fields.push({ path: 'gdrive.client_id', label: 'Client ID', type: 'text' },
        { path: 'gdrive.client_secret', label: 'Client secret', type: 'password' },
        { path: 'gdrive.folder', label: 'Folder in Drive', help: 'Made if it is not there', type: 'text' });
      for (const f of fields) form.append(fieldInput(f, get(f.path, '')));
      const pending = gd.pending;
      form.append(el('div', { class: 'field inline' },
        el('div', {}, el('label', {}, 'Google account'), el('div', { class: 'help' }, gd.connected ? 'Connected' : pending ? 'Waiting for the code to be entered' : 'Not connected')),
        gd.connected
          ? el('button', { class: 'btn sm ghost', type: 'button', onclick: async () => {
              if (!await confirmDialog('Disconnect Google Drive?', 'Backups stop going there until it is connected again. Nothing in Drive is removed.', 'Disconnect', true)) return;
              try { st = await api('/backup/gdrive/disconnect', { body: {} }); drawAll(); } catch (e) { toast(e.message, true); } } }, 'Disconnect')
          : el('button', { class: 'btn sm primary', type: 'button', onclick: connectGoogle }, pending ? 'Show code' : 'Connect')));
    } else if (dest === 'smb') {
      if (st && st.smbclient === false) form.append(el('div', { class: 'notice warn' }, el('span', {}, 'smbclient is not installed on the grill. Run: sudo apt install smbclient')));
      fields.push({ path: 'smb.host', label: 'Host', help: 'Name or address of the NAS or computer', type: 'text' },
        { path: 'smb.share', label: 'Share', type: 'text' },
        { path: 'smb.path', label: 'Folder in the share', help: 'Made if it is not there', type: 'text' },
        { path: 'smb.user', label: 'User', type: 'text' },
        { path: 'smb.password', label: 'Password', type: 'password' });
      for (const f of fields) form.append(fieldInput(f, get(f.path, '')));
      form.append(testRow());
    } else if (dest === 'folder') {
      fields.push({ path: 'folder.path', label: 'Folder', help: 'A path on the grill: a USB stick, or a share already mounted there', type: 'text' });
      for (const f of fields) form.append(fieldInput(f, get(f.path, '')));
      form.append(testRow());
    }
    if (fields.length) {
      form.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
      form.onsubmit = async (e) => {
        e.preventDefault();
        const patch = {};
        try { for (const f of fields) setDeep(patch, f.path, readField(f, form)); } catch (err) { toast(err.message, true); return; }
        await save(patch);
      };
    }
    whereCard.append(form);
  };
  const testRow = () => el('div', { class: 'field inline' },
    el('div', {}, el('label', {}, 'Connection'), el('div', { class: 'help' }, 'Save first, then check the grill can reach it')),
    el('button', { class: 'btn sm', type: 'button', onclick: async (e) => {
      const b = e.currentTarget; b.disabled = true;
      try { const r = await api('/backup/test', { body: {} }); toast(r.message); } catch (err) { toast(err.message, true); }
      b.disabled = false;
    } }, 'Test'));

  /* ---- when ---- */
  const whenCard = el('div', { class: 'card' });
  const drawWhen = () => {
    whenCard.innerHTML = '';
    const form = el('form');
    const sched = get('schedule', 'weekly');
    const fields = [{ path: 'schedule', label: 'Back up', type: 'select', options: SCHED }];
    if (sched === 'weekly') fields.push({ path: 'weekday', label: 'On', type: 'select', options: DAYS });
    if (sched === 'monthly') fields.push({ path: 'monthday', label: 'On day', help: '1 to 28', type: 'int', min: 1, max: 28 });
    if (sched !== 'off') fields.push({ path: 'hour', label: 'At', type: 'select', options: HOURS });
    fields.push({ path: 'keep', label: 'Keep the newest', help: 'Older backups at the destination are removed', type: 'int', min: 1, max: 100 });
    for (const f of fields) {
      const node = fieldInput(f, get(f.path, f.path === 'keep' ? 8 : f.path === 'hour' ? 3 : f.path === 'monthday' ? 1 : 0));
      form.append(node);
      if (f.path === 'schedule') node.querySelector('select').onchange = async (e) => { await save({ schedule: e.target.value }); };
    }
    form.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
    form.onsubmit = async (e) => {
      e.preventDefault();
      const patch = {};
      try { for (const f of fields) setDeep(patch, f.path, readField(f, form)); } catch (err) { toast(err.message, true); return; }
      await save(patch);
    };
    whenCard.append(form);
  };

  /* ---- what is there ---- */
  const listHead = el('h2', {}, 'Backups');
  const listCard = el('div', { class: 'ios-list' });
  const drawList = async () => {
    const dest = get('destination', 'off');
    listHead.hidden = dest === 'off';
    listCard.innerHTML = '';
    if (dest === 'off') { listCard.hidden = true; return; }
    listCard.hidden = false;
    listHead.textContent = `Backups at ${DEST_SAID[dest]}`;
    listCard.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'Looking…'));
    let files = [];
    try { files = (await api('/backup/list')).files || []; listCard.innerHTML = ''; }
    catch (e) { listCard.innerHTML = ''; listCard.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, e.message)); }
    for (const f of files) {
      listCard.append(itemRow({
        icon: 'archive', color: '#30d158',
        title: fmtWhen(f.ts) || f.name,
        meta: `${fmtMB(f.size || 0)} · ${f.name}`,
        chevron: false,
        onclick: () => restoreNamed(f),
        actions: [iconBtn('rotate-ccw', `Restore ${f.name}`, { onclick: (e) => { e.stopPropagation(); restoreNamed(f); } })],
      }));
    }
    if (!files.length) listCard.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'Nothing there yet.'));
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

  /* ---- Google's sign-in: a code to type into google.com/device on the phone ---- */
  const connectGoogle = async () => {
    try { st = await api('/backup/gdrive/connect', { body: {} }); } catch (e) { toast(e.message, true); return; }
    const p = st.gdrive?.pending;
    if (!p) { drawAll(); return; }
    let timer = null;
    await dialog((close) => {
      const code = el('div', { class: 'gcode' }, p.user_code);
      const state = el('p', { class: 'help' }, 'Waiting for you to enter it…');
      const tick = async () => {
        try { st = await api('/backup'); } catch { return; }
        if (st.gdrive?.connected) { toast('Google Drive connected'); close(); return; }
        if (!st.gdrive?.pending) { state.textContent = st.message || 'The code was not used'; return; }
        timer = setTimeout(tick, 3000);
      };
      timer = setTimeout(tick, 3000);
      return el('div', {},
        el('h3', {}, 'Connect Google Drive'),
        el('p', { class: 'muted' }, 'On your phone, open the link and enter this code.'),
        code,
        el('a', { class: 'btn primary block', href: p.url, target: '_blank', rel: 'noopener' }, p.url.replace(/^https?:\/\/(www\.)?/, '')),
        state,
        el('div', { class: 'btnrow' }, el('button', { class: 'btn ghost', type: 'button', onclick: () => close() }, 'Close')));
    });
    clearTimeout(timer);
    drawAll();
  };

  const save = async (patch) => {
    try { await patchSettings('backup', patch); toast('Saved'); }
    catch (e) { toast(e.message, true); return; }
    try { st = await api('/backup'); } catch { /* keep what we had */ }
    drawAll();
  };
  const drawAll = () => { drawStatus(); drawWhere(); drawWhen(); drawList(); };

  let pollT = null;
  const poll = async () => {
    clearTimeout(pollT);
    try { st = await api('/backup'); } catch { return; }
    drawStatus();
    if (st.busy) pollT = setTimeout(poll, 1500);
  };

  view.append(status, el('h2', {}, 'Where'), whereCard, el('h2', {}, 'When'), whenCard, listHead, listCard);
  api('/backup').then((s) => { st = s; drawAll(); if (s.busy) poll(); }).catch((e) => toast(e.message, true));
  return () => clearTimeout(pollT);
}

function setDeep(obj, path, value) {
  const keys = path.split('.');
  let o = obj;
  for (let i = 0; i < keys.length - 1; i++) o = o[keys[i]] ||= {};
  o[keys[keys.length - 1]] = value;
}
