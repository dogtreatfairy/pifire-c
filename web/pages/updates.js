import { PF, el, api, toast, confirmDialog, dialog, actionBar, actionBtn, iconBtn, patchSettings, fmtTime, onStatus } from '../app.js';
import { icon as lucide, tileStyle } from '../icons.js';

// Software Updates: PiFire and the system packages under it, in one list of things to install, each
// with a box to tick -- the shape of Linux Mint's Update Manager. Ticked installs, unticked stays.
// Whatever runs is shown on the console page, which is also where an install that restarts PiFire
// picks its story back up.

const CONSOLE = '#/settings/updates/console';
const idle = () => ['Stop', 'Monitor', 'Error'].includes(PF.status?.mode || 'Stop');
const bare = (v) => String(v || '').replace(/^v/, '');

/* Every line is the one row shape the Settings lists use: a mark at the left (a box to tick, or an
   icon tile), the name, and the value on the right. One line each, nothing under it. */
const tile = (icon, color) => el('span', { class: 'tile', style: tileStyle(color) }, lucide(icon));
function row({ lead, title, value, onclick, chevron, cls = '' }) {
  const kids = [lead, el('span', { class: 'body' }, el('span', { class: 't' }, title)), value != null ? el('span', { class: 'v' }, value) : null,
    chevron ? lucide('chevron-right', 'ic chev') : null];
  return onclick ? el('button', { class: `row ${cls}`, type: 'button', onclick }, kids) : el('div', { class: `row static ${cls}` }, kids);
}
function checkRow({ checked, title, value, onchange, disabled }) {
  const box = el('input', { type: 'checkbox', class: 'row-check', checked, disabled, onchange: (e) => onchange(e.target.checked) });
  return el('label', { class: `row ${disabled ? 'off' : ''}` }, box, el('span', { class: 'body' }, el('span', { class: 't' }, title)), value != null ? el('span', { class: 'v' }, value) : null);
}
const group = (title, rows) => el('section', { class: 'ios-group' }, el('h2', {}, title), el('div', { class: 'ios-list' }, rows.filter(Boolean)));
const notesOf = (u) => String(u.notes || '').split(/\r?\n/).map((l) => l.replace(/^\s*[-*]\s+/, '').trim()).filter((l) => l && !/^#/.test(l));

export function renderUpdates(view) {
  let u = null, pifireOn = null, busyCheck = '', pickTag = null;
  const sel = new Set();          // system packages ticked
  const known = new Set();        // every name ever listed, so a new one arrives ticked
  const slot = el('div');
  const install = actionBtn('download', 'Install', { size: '', class: 'primary', onclick: () => go() });
  const bar = actionBar([iconBtn('terminal', 'Console', { onclick: () => { location.hash = CONSOLE; } })], [install]);
  view.append(slot);
  /* The pinned bar belongs to the page, not to this block: this block sits above the settings on
     the same page, and a bar inside it scrolled under them. */
  (view.closest('main') || view).append(bar);

  const check = async (what) => {
    busyCheck = what;
    paint();
    try { await api('/update/check', { body: what === 'pifire' ? { pifire: true } : { system: true } }); poll(); }
    catch (e) { toast(e.message, true); busyCheck = ''; paint(); }
  };
  const checkedAt = (ts, what) => busyCheck === what ? 'Checking…' : ts ? fmtTime(ts) : 'Never';

  function paint() {
    if (!u) return;
    const busy = u.busy || !!busyCheck;
    /* ---- PiFire ---- */
    const branches = [...new Set(['main', ...(u.branches || []), u.branch, u.current_branch].filter(Boolean))];
    const pick = el('select', { 'aria-label': 'Branch', disabled: busy, onchange: async (e) => {
      pifireOn = null; pickTag = null;
      try { await patchSettings('update', { branch: e.target.value }); check('pifire'); } catch (err) { toast(err.message, true); }
    } }, branches.map((b) => el('option', { value: b, selected: b === u.branch }, b)));
    /* The release to install: the newest by default, any other on the list by choice. The box
       beside it says whether to install it at all; picking another release ticks it, picking the
       one already installed unticks it. */
    const rels = u.releases || [];
    if (!rels.some((r) => r.tag === pickTag)) pickTag = rels[0]?.tag || null;
    const chosen = rels.find((r) => r.tag === pickTag);
    const isCurrent = (r) => r && bare(r.version) === bare(u.current) && !u.switching;
    const installable = (r) => r && r.url && !isCurrent(r);
    /* Ignoring the newest release quiets it -- the header, the notification rule, this box -- and
       only it: the next release is news again. */
    const newest = rels[0];
    const ignored = !!(newest && u.ignored && bare(u.ignored) === bare(newest.tag));
    if (pifireOn == null && chosen) pifireOn = !isCurrent(chosen) && (u.available || u.switching) && !ignored;
    const relPick = el('select', { 'aria-label': 'Release', disabled: busy || !rels.length, onchange: (e) => {
      pickTag = e.target.value;
      pifireOn = !isCurrent(rels.find((r) => r.tag === pickTag));
      paint();
    } }, rels.length ? rels.map((r, i) => el('option', { value: r.tag, selected: r.tag === pickTag },
      `${bare(r.version)}${i === 0 ? ' · newest' : ''}${isCurrent(r) ? ' · installed' : ''}${r.url ? '' : ' · no build'}`)) : el('option', {}, 'Check first'));
    const box = el('input', { type: 'checkbox', class: 'row-check', 'aria-label': 'Install this release', checked: !!(pifireOn && installable(chosen)),
      disabled: !installable(chosen) || busy, onchange: (e) => { pifireOn = e.target.checked; paintBar(); } });
    const notes = chosen ? notesOf(chosen) : [];
    const older = chosen && bare(chosen.version) !== bare(u.current) && u.branch === 'main' && !u.switching && rels.findIndex((r) => r.tag === chosen.tag) > rels.findIndex((r) => isCurrent(r)) && rels.some(isCurrent);
    const pi = [
      row({ lead: tile('package', '#ff8a1f'), title: 'Installed', value: bare(u.current) + (u.current_branch !== 'main' ? ` · ${u.current_branch}` : '') }),
      el('div', { class: 'row static' }, tile('git-branch', '#636366'), el('span', { class: 'body' }, el('span', { class: 't' }, 'Branch')), pick),
      el('div', { class: 'row static' }, box, el('span', { class: 'body' }, el('span', { class: 't' }, older ? 'Release (older)' : 'Release')), relPick),
      u.state === 'error' && !busyCheck ? row({ lead: tile('triangle-alert', '#ff453a'), title: 'Check failed', value: u.message }) : null,
      notes.length ? row({ lead: tile('scroll-text', '#8e8e93'), title: 'Changelog', value: `${notes.length}`, chevron: true, onclick: () => showNotes(chosen, notes) }) : null,
      u.available && newest && !isCurrent(newest)
        ? row({ lead: tile(ignored ? 'eye' : 'eye-off', '#8e8e93'), title: ignored ? 'Ignored' : 'Ignore This Release', value: bare(newest.version),
            onclick: async () => {
              try { await patchSettings('update', { ignored: ignored ? '' : newest.tag }); if (!ignored) pifireOn = false; poll(); }
              catch (err) { toast(err.message, true); }
            } })
        : null,
      row({ lead: tile('refresh-cw', '#0a84ff'), title: 'Check for Updates', value: checkedAt(u.checked_at, 'pifire'), onclick: busy ? null : () => check('pifire'), cls: busy ? 'off' : '' }),
    ];
    /* ---- System ---- */
    const pk = u.system?.packages || [];
    for (const p of pk) if (!known.has(p.name)) { known.add(p.name); sel.add(p.name); }
    for (const n of [...sel]) if (!pk.some((p) => p.name === n)) sel.delete(n);
    const all = pk.length > 0 && pk.every((p) => sel.has(p.name));
    const sys = [
      pk.length > 1 ? checkRow({ checked: all, title: 'All', value: `${sel.size} of ${pk.length}`, onchange: (v) => { if (v) pk.forEach((p) => sel.add(p.name)); else sel.clear(); paint(); } }) : null,
      ...pk.map((p) => checkRow({ checked: sel.has(p.name), title: p.name, value: p.to, onchange: (v) => { if (v) sel.add(p.name); else sel.delete(p.name); paint(); } })),
      pk.length ? null : row({ lead: tile('circle-check', '#30d158'), title: u.system?.checked_at ? 'Up to date' : 'Not checked' }),
      u.system?.reboot_required ? row({ lead: tile('power', '#ff453a'), title: 'Reboot to Finish', chevron: true, onclick: idle() ? reboot : null, cls: 'danger' }) : null,
      row({ lead: tile('refresh-cw', '#0a84ff'), title: 'Check for Updates', value: checkedAt(u.system?.checked_at, 'system'), onclick: busy ? null : () => check('system'), cls: busy ? 'off' : '' }),
    ];
    slot.replaceChildren(group('PiFire', pi), group('System', sys));
    paintBar();
  }
  const chosenRel = () => (u?.releases || []).find((r) => r.tag === pickTag);
  const piReady = () => { const r = chosenRel(); return !!(pifireOn && r && r.url && (bare(r.version) !== bare(u.current) || u.switching)); };
  function count() { return (piReady() ? 1 : 0) + sel.size; }
  function paintBar() {
    const n = count();
    const running = u?.busy && !busyCheck && ['downloading', 'verifying', 'installing', 'upgrading'].includes(u.state);
    install.querySelector('span').textContent = running ? 'View Progress' : n ? `Install ${n}` : 'Install';
    install.disabled = !running && (!n || u?.busy);
    install.onclick = running ? () => { location.hash = CONSOLE; } : () => go();
  }
  async function go() {
    const withPi = piReady();
    const rel = chosenRel();
    const packages = [...sel];
    if (packages.length && !idle()) { toast('Stop the grill before system updates', true); return; }
    if (withPi && !idle() && !PF.settings?.update?.hot_update) { toast('Stop the grill first, or turn on Update while cooking', true); return; }
    const what = [withPi ? `PiFire ${bare(rel.version)}` : null, packages.length ? `${packages.length} system package${packages.length === 1 ? '' : 's'}` : null].filter(Boolean).join(' and ');
    const rels = u.releases || [], cur = rels.findIndex((r) => bare(r.version) === bare(u.current));
    const down = withPi && u.branch === 'main' && !u.switching && cur >= 0 && rels.indexOf(rel) > cur;
    if (!await confirmDialog(`Install ${what}?`, down ? 'Older than the installed version. Settings from newer versions may not carry back. PiFire restarts at the end.' : withPi ? 'PiFire restarts at the end.' : 'PiFire keeps running.', 'Install')) return;
    try { await api('/update/install', { body: { pifire: !!withPi, tag: withPi ? rel.tag : '', packages } }); location.hash = CONSOLE; }
    catch (e) { toast(e.message, true); }
  }

  let t = null;
  async function poll() {
    clearTimeout(t);
    try {
      u = await api('/update');
      if (!u.busy) busyCheck = '';
      paint();
    } catch { /* offline: keep what is shown */ }
    t = setTimeout(poll, u?.busy ? 1000 : 10000);
  }
  poll();
  const off = onStatus(() => paintBar());
  return () => { clearTimeout(t); off(); };
}

function showNotes(r, notes) {
  return dialog((close) => el('div', {},
    el('h3', {}, `PiFire ${bare(r.version)}`),
    el('ul', { class: 'changelog' }, notes.map((l) => el('li', {}, l))),
    el('div', { class: 'btnrow' }, el('button', { class: 'btn primary', type: 'button', onclick: () => close(true) }, 'Close'))));
}

async function reboot() {
  if (!await confirmDialog('Reboot?', 'The grill must be stopped.', 'Reboot')) return;
  api('/admin/reboot', { body: {} }).then(() => toast('Rebooting…')).catch((e) => toast(e.message, true));
}

/* The console: what the updater is doing, line by line, as the tools print it. It keeps reading
   through PiFire's own restart -- the daemon comes back holding the whole story -- and the app
   reloads into this page when the new version answers. */
export function renderConsole(view) {
  const stage = el('div', { class: 'ios-list con-head' });
  const bar = el('div', { class: 'progress', hidden: true }, el('div'));
  const box = el('div', { class: 'console', role: 'log', 'aria-live': 'polite' });
  const done = actionBtn('check', 'Done', { size: '', class: 'primary', onclick: () => { location.hash = '#/settings/updates'; } });
  const rb = actionBtn('power', 'Reboot', { size: '', hidden: true, onclick: reboot });
  view.append(stage, bar, box, actionBar([], [rb, done]));
  let id = null, seq = 0, t = null, lost = 0, bootVersion = null, announced = false;

  const setStage = (text, spin, value) => {
    const bad = text.startsWith('Failed');
    stage.replaceChildren(row({ lead: spin ? el('span', { class: 'tile spin-tile' }, el('span', { class: 'spin', 'aria-hidden': 'true' })) : tile(bad ? 'triangle-alert' : 'circle-check', bad ? '#ff453a' : '#30d158'),
      title: bad ? 'Failed' : text, value: bad ? text.replace(/^Failed:\s*/, '') : value }));
  };
  const WORDS = { checking: 'Checking', downloading: 'Downloading', verifying: 'Verifying', installing: 'Installing', upgrading: 'Upgrading system packages' };

  async function poll() {
    clearTimeout(t);
    let r;
    try { r = await api(`/update/log?since=${seq}`, { timeout: 4000 }); lost = 0; }
    catch {
      /* the daemon is away: while an install was running, that is PiFire restarting */
      lost++;
      setStage(lost > 300 ? 'Failed: PiFire has not come back' : 'Restarting PiFire', lost <= 300);
      t = setTimeout(poll, 1000);
      return;
    }
    if (bootVersion == null) bootVersion = r.version;
    if (r.id !== id) { id = r.id; box.replaceChildren(); if (r.from > 0) { seq = 0; return poll(); } }
    /* the page is what scrolls: follow the end while the reader is at it, leave them be if not */
    const main = view.closest('main') || document.scrollingElement;
    const atEnd = main.scrollTop + main.clientHeight >= main.scrollHeight - 24;
    for (const l of r.lines) box.append(el('div', { class: `con-line ${/^(Error|E:|W:)/.test(l) ? 'bad' : /^==/.test(l) ? 'head' : ''}` }, l));
    seq = r.seq;
    if (atEnd && r.lines.length) main.scrollTop = main.scrollHeight;
    if (r.busy) setStage(WORDS[r.state] || 'Working', true, r.state === 'downloading' ? `${Math.round((r.progress || 0) * 100)}%` : null);
    else if (r.state === 'error') setStage(`Failed: ${r.message}`, false);
    else setStage(r.lines.length || seq ? 'Finished' : 'Nothing running', false);
    bar.hidden = !(r.busy && r.state === 'downloading');
    bar.firstChild.style.width = `${Math.round((r.progress || 0) * 100)}%`;
    rb.hidden = !r.reboot_required;
    /* back on a new version: the announcement is this page, so it is not shown again elsewhere */
    if (!announced && r.version !== bootVersion) announced = true;
    if (!announced) { announced = true; api('/update').then((u) => { if (u?.installed?.ts) api('/update/seen', { body: {} }).catch(() => {}); }).catch(() => {}); }
    t = setTimeout(poll, r.busy ? 700 : 3000);
  }
  poll();
  return () => clearTimeout(t);
}
