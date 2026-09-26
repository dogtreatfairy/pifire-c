import { PF, el, api, patchSettings, addRow, toast, confirmDialog, dialog, pushScreen, degUnit, actionBtn, screenActions, iconBtn, segmented } from '../app.js';
import { icon as lucide } from '../icons.js';
/* The condition cards, rows and picker are shared: recipes ask the same kind of question about
   when a step ends, and must ask it in the same shapes. See web/conditions.js. */
import { titleCase, describeNode, catalogue, condNode, cat, domainOf, hoistFor } from '../conditions.js';

// Conditional Notifications: a table of rules, and an editor that builds them out of the entity
// catalogue the daemon publishes. Nothing here hardcodes what the grill can be asked about, so a
// trait added in C shows up in these dropdowns on the next load.

const LEVELS = [['info', 'Info'], ['normal', 'Normal'], ['high', 'High'], ['critical', 'Critical']];
// The list reads worst first, then alphabetically inside each level: the thing most worth knowing
// about is at the top, and anything else is where its name says it will be rather than where it
// happened to be added.
const RANK = { critical: 0, high: 1, normal: 2, info: 3 };
const byUrgencyThenName = (a, b) => (RANK[a.level] ?? 2) - (RANK[b.level] ?? 2) ||
  String(a.name || a.id).localeCompare(String(b.name || b.id), undefined, { sensitivity: 'base', numeric: true });
const SINKS = [['app', 'In App'], ['webpush', 'This Device'], ['pushover', 'Pushover'], ['ntfy', 'ntfy']];
const ROLES = [['any', 'Any Probe'], ['Food', 'Food Probes'], ['Primary', 'The Pit Probe'], ['Aux', 'Aux Probes']];
const LINKS = [['any', 'Wired & Bluetooth'], ['bluetooth', 'Bluetooth Only'], ['wired', 'Wired Only']];

function summarise(r) {
  const sel = r.select || {};
  let who = titleCase(sel.domain || 'grill');
  if (sel.domain === 'probe') {
    const role = ROLES.find(([v]) => v === (sel.role || 'any'))?.[1] || 'Any Probe';
    who = (sel.match === 'every' ? 'Every ' : 'Any ') + role.replace(/^Any /, '').replace(/^The /, '');
    if (sel.link === 'bluetooth') who += ', Bluetooth';
    if (sel.link === 'wired') who += ', wired';
    if (sel.include?.length) who = sel.include.join(', ');
    if (sel.exclude?.length) who += ` (not ${sel.exclude.join(', ')})`;
  }
  const what = describeNode(r.when, r.select?.domain || 'grill', true);
  const held = r.for_s ? ` for ${r.for_s >= 60 ? `${Math.round(r.for_s / 60)} min` : `${r.for_s}s`}` : '';
  return `${who} · ${what || 'nothing yet'}${held}`;
}

const blankRule = () => ({
  id: `r${Date.now().toString(36)}`, name: 'New Notification', enabled: true,
  select: { domain: 'probe', role: 'Food', link: 'any', match: 'any', include: [], exclude: [] },
  when: { op: 'all', conditions: [{ trait: 'temp', op: '>=', value: 0 }] },
  title: '{probe} reached {target}', body: '{probe} is at {temp}.',
  level: 'normal', sinks: ['app', 'pushover'], for_s: 0, cooldown_s: 600, repeat_s: 0, only_while_cooking: true,
});

// ---- editor -------------------------------------------------------------

function ruleEditor(rule, isNew, allRules) {
  const r = structuredClone(rule);
  r.select ||= { domain: 'probe', role: 'any', link: 'any', match: 'any' };
  r.when ||= { op: 'all', conditions: [] };
  r.when.conditions ||= [];
  /* An older rule kept its hold time beside the condition rather than on it; fold it in so the
     one control shown is the one that runs. */
  if (r.for_s > 0 && !r.when.for_s) r.when.for_s = r.for_s;
  r.for_s = 0;
  hoistFor(r.when);   /* a For belongs to a condition, not to the group round it */

  return pushScreen((close) => {
    const wrap = el('div', { class: 'rule-edit sheet' });
    /* Adding a condition, ticking a chip or switching AND to OR is not an <input> event, so the
       builder says for itself that something moved and the pinned Save lights up. */
    let ready = false;   /* the first draw is not a change the person made */
    const touched = () => { if (ready) wrap.dispatchEvent(new CustomEvent('pf-dirty', { bubbles: true })); };
    const preview = el('div', { class: 'rule-preview' }, el('div', { class: 'muted' }, 'Preview…'));
    let previewTimer = null;
    const refreshPreview = () => {
      clearTimeout(previewTimer);
      previewTimer = setTimeout(async () => {
        try {
          const p = await api('/rules/preview', { body: r });
          preview.innerHTML = '';
          preview.append(
            el('div', { class: 'pv-title' }, p.title || '(no title)'),
            p.body ? el('div', { class: 'pv-body' }, p.body) : null,
            el('div', { class: 'meta' }, `${p.selected} watched · ${p.matching} matching right now`));
        } catch { /* leave the last preview */ }
      }, 350);
    };

    const body = el('div');
    const draw = () => {
      body.innerHTML = '';
      const sel = r.select;
      const isProbe = sel.domain === 'probe';
      const instances = domainOf(sel.domain)?.instances || [];

      // ---- what it watches
      const watch = el('div', { class: 'card tight' },
        el('div', { class: 'field inline' }, el('label', {}, 'Watch'),
          el('select', { onchange: (e) => { sel.domain = e.target.value; r.when.conditions = []; draw(); refreshPreview(); } },
            cat().domains.map((d) => el('option', { value: d.id, selected: d.id === sel.domain }, titleCase(d.id))))));
      if (isProbe) {
        watch.append(
          el('div', { class: 'field inline' }, el('label', {}, 'Which'),
            el('select', { onchange: (e) => { sel.role = e.target.value; draw(); refreshPreview(); } },
              ROLES.map(([v, l]) => el('option', { value: v, selected: v === (sel.role || 'any') }, l)))),
          el('div', { class: 'field inline' }, el('label', {}, 'Connection'),
            el('select', { onchange: (e) => { sel.link = e.target.value; draw(); refreshPreview(); } },
              LINKS.map(([v, l]) => el('option', { value: v, selected: v === (sel.link || 'any') }, l)))),
          el('div', { class: 'field inline' }, el('label', {}, 'Fire'),
            el('select', { onchange: (e) => { sel.match = e.target.value; refreshPreview(); } },
              [['any', 'Once Per Matching Probe'], ['every', 'Once When All Match']].map(([v, l]) => el('option', { value: v, selected: v === (sel.match || 'any') }, l)))));
        // include / exclude as tappable chips, so one rule can cover all but a few probes
        const chips = (key, label) => {
          sel[key] ||= [];
          const box = el('div', { class: 'chips' });
          for (const p of instances) {
            const on = sel[key].includes(p.name) || sel[key].includes(p.label);
            box.append(el('button', { class: `chip ${on ? 'on' : ''}`, type: 'button', onclick: () => {
              sel[key] = on ? sel[key].filter((x) => x !== p.name && x !== p.label) : [...sel[key], p.name];
              draw(); refreshPreview();
            } }, p.name));
          }
          if (!instances.length) box.append(el('span', { class: 'muted' }, 'No probes configured'));
          return el('div', { class: 'field' }, el('label', {}, label), box);
        };
        watch.append(chips('include', 'Only These (blank = all that match above)'), chips('exclude', 'Except'));
      }

      // ---- the condition
      /* One duration control, not two. "Hold For" used to sit under the condition saying exactly
         what the root group's own "For" says, and the two could disagree. */
      const conds = el('div', { class: 'card tight' },
        el('div', { class: 'field' }, el('label', {}, 'When'),
          condNode(r.when, sel.domain, () => { touched(); refreshPreview(); }, null, 0)));

      // ---- the message
      const tokenChips = el('div', { class: 'chips' });
      let lastFocused = null;
      const track = (input) => { input.addEventListener('focus', () => (lastFocused = input)); return input; };
      const titleIn = track(el('input', { type: 'text', value: r.title || '', onchange: (e) => { r.title = e.target.value; refreshPreview(); }, oninput: (e) => { r.title = e.target.value; refreshPreview(); } }));
      const bodyIn = track(el('textarea', { rows: 2, oninput: (e) => { r.body = e.target.value; refreshPreview(); } }));
      bodyIn.value = r.body || '';
      for (const t of cat().tokens) {
        tokenChips.append(el('button', { class: 'chip', type: 'button', onclick: () => {
          const input = lastFocused || titleIn;
          const at = input.selectionStart ?? input.value.length;
          input.value = `${input.value.slice(0, at)}{${t}}${input.value.slice(at)}`;
          if (input === titleIn) r.title = input.value; else r.body = input.value;
          input.focus();
          input.selectionStart = input.selectionEnd = at + t.length + 2;
          refreshPreview();
        } }, `{${t}}`));
      }
      const msg = el('div', { class: 'card tight' },
        el('div', { class: 'field' }, el('label', {}, 'Title'), titleIn),
        el('div', { class: 'field' }, el('label', {}, 'Message'), bodyIn),
        el('div', { class: 'field' }, el('label', {}, 'Insert'), tokenChips),
        preview);

      // ---- how loudly
      const seg = el('div', { class: 'seg' });
      for (const [v, l] of LEVELS) {
        seg.append(el('button', { class: `seg-btn ${r.level === v ? 'on' : ''} lvl-${v}`, type: 'button',
          onclick: () => { r.level = v; draw(); } }, l));
      }
      const sinkBox = el('div', { class: 'chips' });
      r.sinks ||= ['app'];
      for (const [v, l] of SINKS) {
        const on = r.sinks.includes(v);
        sinkBox.append(el('button', { class: `chip ${on ? 'on' : ''}`, type: 'button', onclick: () => {
          r.sinks = on ? r.sinks.filter((x) => x !== v) : [...r.sinks, v];
          draw();
        } }, l));
      }
      /* How loud it is comes first, before the name. It is the decision that changes what every
         other answer on this screen means -- a critical alert and a quiet one are not the same rule
         with a different word on it -- and it is the one you are surest about when you open this. */
      const urgency = el('div', { class: 'card tight' },
        el('div', { class: 'field' }, el('label', {}, 'Urgency'), seg),
        r.level === 'critical' ? el('p', { class: 'help' },
          'Sends the highest priority each service offers (Pushover Emergency repeats until you acknowledge it). Whether it breaks through a Focus mode depends on how you allow the Pushover or ntfy app in your phone\'s notification settings.') : null);
      const alert = el('div', { class: 'card tight' },
        el('div', { class: 'field' }, el('label', {}, 'Send To'), sinkBox),
        /* Test sends the real message to the real services, so it belongs with the choice of where
           it goes. It is not a commit action and never went on the bar; it was down beside the
           message preview, which is a long way from where anyone looks for it. */
        el('div', { class: 'form-actions' },
          actionBtn('test', 'Send a test', { size: '', onclick: async () => {
            try { await api('/rules/test', { body: r }); toast('Sent \u2014 check your phone'); } catch (e) { toast(e.message, true); }
          } }, 'send')));

      // ---- advanced
      const adv = el('details', { class: 'fold' }, el('summary', {}, el('span', {}, 'Advanced')),
        el('div', { class: 'card tight' },
          el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Cooldown'), el('div', { class: 'help' }, 'Seconds before this can send again')),
            el('input', { type: 'text', inputmode: 'numeric', value: r.cooldown_s ?? 600, onchange: (e) => (r.cooldown_s = parseInt(e.target.value, 10) || 0) })),
          el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Repeat Every'), el('div', { class: 'help' }, 'Seconds; 0 = send once until it goes false')),
            el('input', { type: 'text', inputmode: 'numeric', value: r.repeat_s ?? 0, onchange: (e) => (r.repeat_s = parseInt(e.target.value, 10) || 0) })),
          el('label', { class: 'toggle' }, el('div', {}, el('div', {}, 'Only While Cooking'), el('div', { class: 'help' }, 'Off means it can also fire while the grill is stopped')),
            el('span', { class: 'switch' }, el('input', { type: 'checkbox', checked: r.only_while_cooking !== false, onchange: (e) => (r.only_while_cooking = e.target.checked) }), el('span'))),
          /* One situation, one alarm: while this one stands, the ones it names are silenced. It is
             how "Hopper Critical" keeps "Hopper Low" from sounding beside it about the same hopper. */
          el('div', { class: 'field' }, el('label', {}, 'Stands In For'),
            el('div', { class: 'help' }, 'While this fires, these stay quiet'),
            el('div', { class: 'chips' }, (allRules || []).filter((o) => o.id !== r.id).map((o) => {
              const on = (r.supersedes || []).includes(o.id);
              return el('button', { class: `chip ${on ? 'on' : ''}`, type: 'button', onclick: () => {
                r.supersedes = on ? (r.supersedes || []).filter((x) => x !== o.id) : [...(r.supersedes || []), o.id];
                touched(); draw();
              } }, o.name || o.id);
            })))));

      body.append(
        urgency,
        el('div', { class: 'field' }, el('label', {}, 'Name'),
          el('input', { type: 'text', value: r.name || '', onchange: (e) => (r.name = e.target.value) })),
        watch, conds, msg, alert, adv);
      refreshPreview();
    };
    draw();

    /* A sheet, like every other: a header that stays, a body that scrolls, a footer that stays.
       This was one long block inside a dialog that hid its overflow, so on a phone the form was
       taller than the screen, Save and Cancel were below the fold, and there was nothing to scroll
       -- the editor simply sat there with no way out of it. */
    const dismiss = async () => {
      if (JSON.stringify(r) !== JSON.stringify(rule) && !await confirmDialog('Discard changes?', r.name || '', 'Discard', true)) return;
      close(undefined);
    };
    wrap.append(
      el('div', { class: 'sheet-body' }, body),
      screenActions({
        onDelete: isNew ? null : () => close('delete'),
        deleteTitle: 'Delete notification',
        onCancel: dismiss,
        onSave: () => close(r),
        /* A new rule is born changed and is ready to save; an existing one lights Save only once
           something has actually moved. */
        dirty: isNew,
      }));
    setTimeout(() => { ready = true; }, 0);
    return wrap;
  }, { title: isNew ? 'New Notification' : rule.name, back: 'Notifications' });
}

// ---- the table ----------------------------------------------------------

export async function renderRules(view) {
  await catalogue();
  const data = await api('/rules');
  let rules = data.rules || [];

  const list = el('div', { class: 'ios-list' });
  const save = async () => {
    try {
      await patchSettings('notify', { rules });
      // read back what was actually stored, so the table can never show something that was not saved
      rules = (await api('/rules')).rules || [];
      toast('Saved');
      draw();
    } catch (e) { toast(e.message, true); draw(); }
  };
  const edit = async (rule, isNew) => {
    const r = await ruleEditor(rule, isNew, rules);
    if (!r) return;
    if (r === 'delete') {
      if (!await confirmDialog('Delete notification?', rule.name, 'Delete', true)) return;
      rules = rules.filter((x) => x.id !== rule.id);
    } else if (isNew) rules.push(r);
    else rules = rules.map((x) => (x.id === r.id ? r : x));
    await save();
  };

  const draw = () => {
    list.innerHTML = '';
    for (const r of [...rules].sort(byUrgencyThenName)) {
      const sw = el('label', { class: 'switch', onclick: (e) => e.stopPropagation() },
        el('input', { type: 'checkbox', checked: r.enabled !== false, onchange: async (e) => { r.enabled = e.target.checked; await save(); } }), el('span'));
      /* The same row every list in the app uses: what it is called and what it watches, its
         urgency, and the one control you actually touch -- on or off. The conditions are behind the
         row, where they are edited once and then left alone. */
      list.append(el('div', { class: `prow rule-row ${r.enabled === false ? 'off' : ''}` },
        el('div', { class: 'row' },
          el('button', { class: 'rule-main', type: 'button', onclick: () => edit(r, false) },
            el('span', { class: 'body' },
              el('span', { class: 't' }, r.name || r.id,
                el('span', { class: `pill sm lvl-${r.level || 'normal'}` }, (r.level || 'normal').toUpperCase())),
              el('span', { class: 's' }, summarise(r)))),
          sw)));
    }
    if (!rules.length) list.append(el('p', { class: 'help', style: 'padding:var(--sp-3)' }, 'No conditional notifications yet.'));
    list.append(addRow('Add Notification', () => edit(blankRule(), true)));
  };
  draw();

  /* The way to add one is the same as on every other list: a full-width button at its foot, where
     the eye ends up after reading what is already there. A "+ Add" in the heading was the one
     place in the app that put it somewhere else. */
  view.append(
    el('h2', {}, 'Conditional Notifications'),
    /* The list is a settings list and nothing more: no paragraph explaining what a rule is -- the
       rows say it -- and no card wrapped round a list that draws its own border, which is where the
       double outline came from. Every other section on the page appends its list directly. */
    list);
}
