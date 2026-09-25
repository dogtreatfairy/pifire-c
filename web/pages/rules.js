import { PF, el, api, patchSettings, toast, confirmDialog, dialog, pushScreen, degUnit, actionBtn, screenActions, iconBtn, segmented } from '../app.js';
import { icon as lucide } from '../icons.js';

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
const OP_LABEL = {
  '>': 'is above', '>=': 'is at or above', '<': 'is below', '<=': 'is at or below',
  '==': 'is', '!=': 'is not', between: 'is between', within: 'is within',
  is_one_of: 'is one of', is_none_of: 'is none of',
  is_on: 'is on', is_off: 'is off', is: 'is', is_not: 'is not',
  contains: 'contains', empty: 'is empty', not_empty: 'is not empty',
};
const titleCase = (s) => s.replace(/_/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase());

let CAT = null;
const catalogue = async () => (CAT ||= await api('/rules/entities'));
const domainOf = (id) => CAT?.domains.find((d) => d.id === id) || CAT?.domains[0];
const traitsOf = (id) => domainOf(id)?.traits || [];
const traitDef = (domain, trait) => traitsOf(domain).find((t) => t.id === trait);
// the daemon names its own traits; titleCase is only the fallback for one it has not labelled
const traitLabel = (domain, trait) => traitDef(domain, trait)?.label || titleCase(trait);
const isGroup = (n) => Array.isArray(n?.conditions);
const listOp = (op) => op === 'is_one_of' || op === 'is_none_of';

/** a one-line plain-English description of what a rule watches */
/* How long a node has to have been true, said the way a person would. */
const forLabel = (secs) => !secs ? '' : secs >= 3600 ? ` for ${(secs / 3600).toFixed(secs % 3600 ? 1 : 0)} h`
  : secs >= 60 ? ` for ${Math.round(secs / 60)} min` : ` for ${secs}s`;

/* One line saying what a node means, used both for the row in the list and for the header of each
   condition card when it is folded shut. A card you cannot read without opening it is a card that
   has to be opened. */
function describeNode(node, domain, top) {
  if (isGroup(node)) {
    const inner = (node.conditions || []).map((k) => describeNode(k, domain, false)).filter(Boolean);
    if (!inner.length) return '';
    const join = node.op === 'any' ? ' or ' : ' and ';
    const body = top || inner.length < 2 ? inner.join(join) : `(${inner.join(join)})`;
    return (node.op === 'not' ? `not ${inner.length > 1 ? `(${inner.join(' and ')})` : inner[0]}` : body) + forLabel(node.for_s);
  }
  if (!node?.trait) return '';
  const from = node.entity && node.entity !== 'this' ? node.entity : domain;
  const lhs = node.entity && node.entity !== 'this'
    ? `${titleCase(node.entity)} ${traitLabel(from, node.trait)}` : traitLabel(from, node.trait);
  const v = node.value;
  const vs = Array.isArray(v) ? ` ${v.join(' or ')}`
    : v && typeof v === 'object' && v.trait
      ? `${v.entity ? ` ${titleCase(v.entity)} ${traitLabel(v.entity, v.trait)}` : ` its ${traitLabel(from, v.trait)}`}${
          v.offset ? ` ${v.offset > 0 ? '+' : '\u2212'} ${Math.abs(v.offset)}` : ''}`
      : v === undefined ? '' : ` ${v}`;
  return `${lhs} ${OP_LABEL[node.op] || node.op}${vs}${node.value2 !== undefined ? ` \u00b1 ${node.value2}` : ''}${forLabel(node.for_s)}`;
}

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

function conditionRow(cond, domain, onChange) {
  const row = el('div', { class: 'cond' });
  const draw = () => {
    row.innerHTML = '';
    const from = cond.entity && cond.entity !== 'this' ? cond.entity : domain;
    const def = traitDef(from, cond.trait) || traitsOf(from)[0];
    if (def && !traitDef(from, cond.trait)) cond.trait = def.id;
    const ops = def?.operators || ['=='];
    if (!ops.includes(cond.op)) cond.op = ops[0];
    const needsValue = !['is_on', 'is_off', 'empty', 'not_empty'].includes(cond.op);
    const isList = listOp(cond.op);
    if (isList && !Array.isArray(cond.value)) cond.value = [];
    if (!isList && Array.isArray(cond.value)) cond.value = cond.value[0] ?? 0;
    const asTrait = !isList && cond.value && typeof cond.value === 'object';

    // the reading being tested: one of the watched entity's own, or any single-instance entity's,
    // which is how "only while the grill is in Hold" is added to a probe rule
    const cur = `${cond.entity && cond.entity !== 'this' ? cond.entity : 'this'}:${cond.trait}`;
    const groups = [el('optgroup', { label: `This ${titleCase(domain)}` },
      traitsOf(domain).map((t) => el('option', { value: `this:${t.id}`, selected: cur === `this:${t.id}` }, t.label || titleCase(t.id))))];
    for (const d of CAT.domains) {
      if (d.multi || d.id === domain) continue;
      groups.push(el('optgroup', { label: titleCase(d.id) },
        d.traits.map((t) => el('option', { value: `${d.id}:${t.id}`, selected: cur === `${d.id}:${t.id}` }, `${titleCase(d.id)} ${t.label || titleCase(t.id)}`))));
    }
    row.append(
      el('select', { class: 'c-trait', onchange: (e) => {
        const [ent, tr] = e.target.value.split(':');
        if (ent === 'this') delete cond.entity; else cond.entity = ent;
        cond.trait = tr;
        draw(); onChange();
      } }, groups),
      el('select', { class: 'c-op', onchange: (e) => { cond.op = e.target.value; draw(); onChange(); } },
        ops.map((o) => el('option', { value: o, selected: o === cond.op }, OP_LABEL[o] || o))));

    if (needsValue) {
      const pair = cond.op === 'between' || cond.op === 'within';
      let valueField;
      if (asTrait || (!isList && def?.type !== 'enum')) {
        /* What it is compared against: a number, or another reading.
         *
         * Home Assistant's numeric_state takes a number OR an entity in the same `above`/`below`
         * field, and that is right -- they are two kinds of answer to one question, not two modes
         * of the editor. There used to be a "Compare with" switch above this deciding which sort of
         * field you were about to get, which is a question about the interface rather than about
         * the grill. One list: "a number" first, then every reading it could mean.
         *
         * A reading can carry an offset, because a band around a moving number is the point of
         * comparing against one at all: "below the set point + 15" and "above the set point - 15"
         * is a rule that still means something after the set point changes, where a fixed pair of
         * numbers does not. */
        const cur = asTrait ? `${cond.value.entity || 'this'}:${cond.value.trait}` : 'num';
        const opts = [el('option', { value: 'num', selected: !asTrait }, 'a number'),
          el('optgroup', { label: `This ${titleCase(domain)}` },
            traitsOf(domain).map((t) => el('option', { value: `this:${t.id}`, selected: cur === `this:${t.id}` }, `its ${t.label || titleCase(t.id)}`)))];
        for (const d of CAT.domains) {
          if (d.multi || d.id === domain) continue;
          opts.push(el('optgroup', { label: titleCase(d.id) },
            d.traits.map((t) => el('option', { value: `${d.id}:${t.id}`, selected: cur === `${d.id}:${t.id}` }, `${titleCase(d.id)} ${t.label || titleCase(t.id)}`))));
        }
        const chooser = el('select', { class: 'c-val', onchange: (e) => {
          const v = e.target.value;
          if (v === 'num') cond.value = 0;
          else { const [ent, tr] = v.split(':'); cond.value = ent === 'this' ? { trait: tr } : { entity: ent, trait: tr }; }
          draw(); onChange();
        } }, opts);
        if (asTrait) {
          const unit = def?.type === 'temperature' ? degUnit() : '';
          valueField = el('div', { class: 'c-val c-operand' }, chooser,
            el('input', { type: 'text', inputmode: 'decimal', class: 'c-off',
              value: cond.value.offset ?? '', placeholder: `\u00b1 ${unit || '0'}`,
              title: 'Offset on that reading',
              onchange: (e) => { const n = parseFloat(e.target.value); if (n) cond.value.offset = n; else delete cond.value.offset; onChange(); } }));
        } else {
          const unit = def?.type === 'temperature' ? degUnit() : def?.type === 'duration' ? 'seconds' : def?.unit === '%' ? '%' : 'value';
          valueField = el('div', { class: 'c-val c-operand' }, chooser,
            el('input', { type: 'text', inputmode: 'decimal', class: 'c-num', value: cond.value ?? '', placeholder: unit,
              onchange: (e) => { cond.value = parseFloat(e.target.value) || 0; onChange(); } }));
        }
      } else if (isList) {
        // several accepted values as chips: "the mode is Hold or Smoke" stays one row
        const opts = def?.type === 'enum' ? (CAT.modes || []) : [];
        if (opts.length) {
          valueField = el('div', { class: 'c-val chips tight' }, opts.map((m) => {
            const on = cond.value.includes(m);
            return el('button', { class: `chip ${on ? 'on' : ''}`, type: 'button', onclick: () => {
              cond.value = on ? cond.value.filter((x) => x !== m) : [...cond.value, m];
              draw(); onChange();
            } }, m);
          }));
        } else {
          valueField = el('input', { class: 'c-val', type: 'text', value: cond.value.join(', '), placeholder: 'value, value',
            onchange: (e) => { cond.value = e.target.value.split(',').map((x) => x.trim()).filter(Boolean); onChange(); } });
        }
        valueField.style.gridColumn = '1 / -1';
      } else if (def?.type === 'enum') {
        valueField = el('select', { class: 'c-val', onchange: (e) => { cond.value = e.target.value; onChange(); } },
          (CAT.modes || []).map((m) => el('option', { value: m, selected: m === cond.value }, m)));
      }
      if (pair) valueField.style.gridColumn = '1';
      row.append(valueField);
      if (pair) row.append(el('input', { class: 'c-val2', type: 'text', inputmode: 'decimal', value: cond.value2 ?? '',
        placeholder: 'and', onchange: (e) => { cond.value2 = parseFloat(e.target.value) || 0; onChange(); } }));
    }
  };
  draw();
  return row;
}

/* The three ways conditions join, with the mark each one is written with. */
const GROUP_OPS = [['all', 'AND', '&'], ['any', 'OR', '\u2265'], ['not', 'NOT', '\u2260']];

/* A duration on a single condition: "in Hold for half an hour", "above 200 for five minutes".
   It belongs to the condition rather than to the whole rule, so one arm of an AND can wait while
   the others answer at once -- which is what makes "in Hold, within 5 degrees of the set point,
   for ten minutes" a single rule. */
const forField = (node, onChange) => el('div', { class: 'field' },
  el('label', {}, 'For'),
  el('div', { class: 'row', style: 'gap:var(--sp-2)' },
    el('input', {
      type: 'text', inputmode: 'decimal', style: 'flex:1 1 auto; min-width:0',
      value: node.for_s ? (node.for_s >= 60 ? node.for_s / 60 : node.for_s) : '',
      placeholder: 'immediately',
      onchange: (e) => {
        const n = parseFloat(e.target.value);
        const mins = (node._for_unit || 'min') === 'min';
        if (!(n > 0)) delete node.for_s; else node.for_s = Math.round(mins ? n * 60 : n);
        onChange();
      },
    }),
    segmented([['min', 'min'], ['s', 'sec']], node._for_unit || 'min', (u) => {
      const was = node._for_unit || 'min';
      node._for_unit = u;
      if (node.for_s && was !== u) node.for_s = u === 'min' ? node.for_s : node.for_s;
      onChange();
    })));

/* A condition, and a group of conditions, are both a card: a header you can read with it shut and
   a body you open to change it. Nesting is what lets one rule say "the mode is Hold or Smoke, and
   the pit is within 15 of the set point": the OR has to bind tighter than the AND, and a flat list
   cannot express that. The card is the app's own fold -- the same shape every collapsible section
   uses -- rather than a second collapsible invented for this screen. */
function condNode(node, domain, onChange, onRemove, depth) {
  const group = isGroup(node);
  const det = el('details', { class: `fold cond-card${group ? ' is-group' : ''} d${Math.min(depth, 3)}`, open: depth < 1 || !group });
  const title = el('span', { class: 'cc-title' });
  const glyph = el('span', { class: 'cc-glyph' });
  const head = el('summary', { class: 'cc-head' }, glyph, title,
    onRemove ? iconBtn('trash-2', group ? 'Remove this group' : 'Remove this condition', {
      class: 'danger cc-del',
      onclick: (e) => { e.preventDefault(); e.stopPropagation(); onRemove(); },
    }) : null);
  const body = el('div', { class: 'cc-body' });
  det.append(head, body);

  const retitle = () => {
    const g = GROUP_OPS.find(([v]) => v === (node.op || 'all'));
    glyph.textContent = group ? (g?.[2] || '&') : '123';
    const said = describeNode(node, domain, false);
    title.textContent = group ? `${g?.[1] || 'AND'}${node.conditions?.length ? ` \u00b7 ${node.conditions.length}` : ''}`
                              : (said || 'New condition');
  };

  const draw = () => {
    body.innerHTML = '';
    if (group) {
      /* The operator is a choice of three, so it is three buttons rather than a dropdown you have
         to open to find out what the options were. */
      body.append(segmented(GROUP_OPS.map(([v, l]) => [v, l]), node.op || 'all', (v) => { node.op = v; retitle(); onChange(); }));
      node.conditions.forEach((k, i) => body.append(condNode(k, domain, () => { retitle(); onChange(); },
        () => { node.conditions.splice(i, 1); draw(); retitle(); onChange(); }, depth + 1)));
      const t = traitsOf(domain)[0];
      body.append(el('div', { class: 'cc-add' },
        actionBtn('add', 'Condition', { onclick: () => {
          node.conditions.push({ trait: t?.id || 'temp', op: t?.operators?.[0] || '>=', value: 0 });
          draw(); retitle(); onChange();
        } }),
        depth < 3 ? actionBtn('add', 'Group', { onclick: () => {
          node.conditions.push({ op: 'any', conditions: [] });
          draw(); retitle(); onChange();
        } }) : null));
    } else {
      body.append(conditionRow(node, domain, () => { retitle(); onChange(); }));
    }
    body.append(forField(node, () => { retitle(); onChange(); }));
    retitle();
  };
  draw();
  return det;
}

function ruleEditor(rule, isNew) {
  const r = structuredClone(rule);
  r.select ||= { domain: 'probe', role: 'any', link: 'any', match: 'any' };
  r.when ||= { op: 'all', conditions: [] };
  r.when.conditions ||= [];

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
            CAT.domains.map((d) => el('option', { value: d.id, selected: d.id === sel.domain }, titleCase(d.id))))));
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
      const conds = el('div', { class: 'card tight' },
        el('div', { class: 'field' }, el('label', {}, 'When'),
          condNode(r.when, sel.domain, () => { touched(); refreshPreview(); }, null, 0)),
        el('div', { class: 'field inline' },
          el('div', {}, el('label', {}, 'Hold For'),
            el('div', { class: 'help' }, 'Seconds it must stay true before sending. 180 is three minutes.')),
          el('input', { type: 'text', inputmode: 'numeric', value: r.for_s ?? 0,
            onchange: (e) => { r.for_s = parseInt(e.target.value, 10) || 0; refreshPreview(); } })));

      // ---- the message
      const tokenChips = el('div', { class: 'chips' });
      let lastFocused = null;
      const track = (input) => { input.addEventListener('focus', () => (lastFocused = input)); return input; };
      const titleIn = track(el('input', { type: 'text', value: r.title || '', onchange: (e) => { r.title = e.target.value; refreshPreview(); }, oninput: (e) => { r.title = e.target.value; refreshPreview(); } }));
      const bodyIn = track(el('textarea', { rows: 2, oninput: (e) => { r.body = e.target.value; refreshPreview(); } }));
      bodyIn.value = r.body || '';
      for (const t of CAT.tokens) {
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
        preview,
        /* Test sends the message so you can see it land on your phone. It belongs beside the
           message it sends, not on the commit bar: it changes nothing, and a third verb up there
           pushed Cancel under the Home button. */
        el('div', { class: 'form-actions' },
          actionBtn('test', 'Send a test', { size: '', onclick: async () => {
            try { await api('/rules/test', { body: r }); toast('Sent \u2014 check your phone'); } catch (e) { toast(e.message, true); }
          } }, 'send')));

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
      const alert = el('div', { class: 'card tight' },
        el('div', { class: 'field' }, el('label', {}, 'Urgency'), seg),
        r.level === 'critical' ? el('p', { class: 'help' },
          'Sends the highest priority each service offers (Pushover Emergency repeats until you acknowledge it). Whether it breaks through a Focus mode depends on how you allow the Pushover or ntfy app in your phone\'s notification settings.') : null,
        el('div', { class: 'field' }, el('label', {}, 'Send To'), sinkBox));

      // ---- advanced
      const adv = el('details', { class: 'fold' }, el('summary', {}, el('span', {}, 'Advanced')),
        el('div', { class: 'card tight' },
          el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Cooldown'), el('div', { class: 'help' }, 'Seconds before this can send again')),
            el('input', { type: 'text', inputmode: 'numeric', value: r.cooldown_s ?? 600, onchange: (e) => (r.cooldown_s = parseInt(e.target.value, 10) || 0) })),
          el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Repeat Every'), el('div', { class: 'help' }, 'Seconds; 0 = send once until it goes false')),
            el('input', { type: 'text', inputmode: 'numeric', value: r.repeat_s ?? 0, onchange: (e) => (r.repeat_s = parseInt(e.target.value, 10) || 0) })),
          el('label', { class: 'toggle' }, el('div', {}, el('div', {}, 'Only While Cooking'), el('div', { class: 'help' }, 'Off means it can also fire while the grill is stopped')),
            el('span', { class: 'switch' }, el('input', { type: 'checkbox', checked: r.only_while_cooking !== false, onchange: (e) => (r.only_while_cooking = e.target.checked) }), el('span')))));

      body.append(
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
      /* The screen's own bar already says which notification this is, so the head carries what the
         bar cannot: how loud it is. Saying the name twice is what a dialog inside a page does. */
      el('div', { class: 'sheet-head' },
        el('div', {}, el('div', { class: 'help' }, isNew ? 'Sends when its condition becomes true' : 'Urgency')),
        el('span', { class: `pill sm lvl-${r.level || 'normal'}` }, (r.level || 'normal').toUpperCase())),
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
    const r = await ruleEditor(rule, isNew);
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
  };
  draw();

  view.append(
    el('div', { class: 'row between' }, el('h2', {}, 'Conditional Notifications'),
      el('button', { class: 'btn sm', type: 'button', onclick: () => edit(blankRule(), true) }, '+ Add')),
    /* The list is a settings list and nothing more: no paragraph explaining what a rule is -- the
       rows say it -- and no card wrapped round a list that draws its own border, which is where the
       double outline came from. Every other section on the page appends its list directly. */
    list);
}
