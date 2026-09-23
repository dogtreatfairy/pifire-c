import { PF, el, api, patchSettings, toast, confirmDialog, dialog, degUnit } from '../app.js';
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
  const describe = (node, top) => {
    if (isGroup(node)) {
      const inner = node.conditions.map((k) => describe(k, false)).filter(Boolean);
      if (!inner.length) return '';
      const join = node.op === 'any' ? ' or ' : ' and ';
      // only a nested group needs brackets; the outermost one reads better without them
      return top || inner.length < 2 ? inner.join(join) : `(${inner.join(join)})`;
    }
    if (!node?.trait) return '';
    const from = node.entity && node.entity !== 'this' ? node.entity : (r.select?.domain || 'grill');
    const lhs = node.entity && node.entity !== 'this'
      ? `${titleCase(node.entity)} ${traitLabel(from, node.trait)}` : traitLabel(from, node.trait);
    const v = node.value;
    const vs = Array.isArray(v) ? ` ${v.join(' or ')}`
      : v && typeof v === 'object' && v.trait
        ? (v.entity ? ` ${titleCase(v.entity)} ${traitLabel(v.entity, v.trait)}` : ` its ${traitLabel(from, v.trait)}`)
        : v === undefined ? '' : ` ${v}`;
    return `${lhs} ${OP_LABEL[node.op] || node.op}${vs}${node.value2 !== undefined ? ` \u00b1 ${node.value2}` : ''}`;
  };
  const what = describe(r.when, true);
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

function conditionRow(cond, domain, onChange, onRemove) {
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
        ops.map((o) => el('option', { value: o, selected: o === cond.op }, OP_LABEL[o] || o))),
      el('button', { class: 'btn xs ghost c-del', type: 'button', 'aria-label': 'Remove this condition', onclick: onRemove }, '×'));

    if (needsValue) {
      const pair = cond.op === 'between' || cond.op === 'within';
      let valueField;
      if (asTrait) {
        // "its own target" for the probe being watched, or any reading of a single-instance entity
        // such as the grill's set point, which is how "pit reached its set point" is written
        const cur = `${cond.value.entity || 'this'}:${cond.value.trait}`;
        const opts = [el('optgroup', { label: 'This ' + titleCase(domain) },
          traitsOf(domain).map((t) => el('option', { value: `this:${t.id}`, selected: cur === `this:${t.id}` }, `its ${titleCase(t.id)}`)))];
        for (const d of CAT.domains) {
          if (d.multi || d.id === domain) continue;
          opts.push(el('optgroup', { label: titleCase(d.id) },
            d.traits.map((t) => el('option', { value: `${d.id}:${t.id}`, selected: cur === `${d.id}:${t.id}` }, `${titleCase(d.id)} ${titleCase(t.id)}`))));
        }
        valueField = el('select', { class: 'c-val', onchange: (e) => {
          const [ent, tr] = e.target.value.split(':');
          cond.value = ent === 'this' ? { trait: tr } : { entity: ent, trait: tr };
          onChange();
        } }, opts);
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
      } else {
        const unit = def?.type === 'temperature' ? degUnit() : def?.type === 'duration' ? 'seconds' : def?.unit === '%' ? '%' : 'value';
        valueField = el('input', {
          class: 'c-val', type: 'text', inputmode: 'decimal', value: cond.value ?? '',
          placeholder: unit, onchange: (e) => { cond.value = parseFloat(e.target.value) || 0; onChange(); },
        });
      }
      if (pair) valueField.style.gridColumn = '1';
      row.append(valueField);
      if (pair) row.append(el('input', { class: 'c-val2', type: 'text', inputmode: 'decimal', value: cond.value2 ?? '',
        placeholder: 'and', onchange: (e) => { cond.value2 = parseFloat(e.target.value) || 0; onChange(); } }));
      // comparing against another reading is what lets one rule cover every probe
      if (!isList) row.append(el('button', {
        class: `btn xs c-kind ${asTrait ? 'primary' : 'ghost'}`, type: 'button', title: 'Compare with another reading',
        onclick: () => { cond.value = asTrait ? 0 : { trait: 'target' }; draw(); onChange(); },
      }, asTrait ? 'trait' : '123'));
    }
  };
  draw();
  return row;
}

/* A condition is either a comparison or a group of them with its own all/any. Nesting is what lets
   one rule say "the mode is Hold or Smoke, and the pit is within 15 of the set point": the or has
   to bind tighter than the and, and a single flat list cannot express that. */
function condNode(node, domain, onChange, onRemove, depth) {
  if (!isGroup(node)) return conditionRow(node, domain, onChange, onRemove);
  const box = el('div', { class: `cond-group d${Math.min(depth, 3)}` });
  const draw = () => {
    box.innerHTML = '';
    box.append(el('div', { class: 'cg-head' },
      el('select', { class: 'cg-op', onchange: (e) => { node.op = e.target.value; onChange(); } },
        [['all', 'All Of These'], ['any', 'Any Of These']].map(([v, l]) => el('option', { value: v, selected: v === node.op }, l))),
      onRemove ? el('button', { class: 'btn xs ghost c-del', type: 'button', 'aria-label': 'Remove this group', onclick: onRemove }, '×') : null));
    node.conditions.forEach((k, i) => box.append(condNode(k, domain, onChange,
      () => { node.conditions.splice(i, 1); draw(); onChange(); }, depth + 1)));
    const t = traitsOf(domain)[0];
    box.append(el('div', { class: 'cg-add' },
      el('button', { class: 'btn xs ghost', type: 'button', onclick: () => {
        node.conditions.push({ trait: t?.id || 'temp', op: t?.operators?.[0] || '>=', value: 0 });
        draw(); onChange();
      } }, '+ Condition'),
      depth < 3 ? el('button', { class: 'btn xs ghost', type: 'button', onclick: () => {
        node.conditions.push({ op: 'any', conditions: [] });
        draw(); onChange();
      } }, '+ Group') : null));
  };
  draw();
  return box;
}

function ruleEditor(rule, isNew) {
  const r = structuredClone(rule);
  r.select ||= { domain: 'probe', role: 'any', link: 'any', match: 'any' };
  r.when ||= { op: 'all', conditions: [] };
  r.when.conditions ||= [];

  return dialog((close) => {
    const wrap = el('div', { class: 'rule-edit' });
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
          condNode(r.when, sel.domain, refreshPreview, null, 0)),
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
      const alert = el('div', { class: 'card tight' },
        el('div', { class: 'field' }, el('label', {}, 'Urgency'), seg),
        r.level === 'critical' ? el('p', { class: 'muted', style: 'font-size:.78rem' },
          'Sends the highest priority each service offers (Pushover Emergency repeats until you acknowledge it). Whether it breaks through a Focus mode depends on how you allow the Pushover or ntfy app in your phone\'s notification settings.') : null,
        el('div', { class: 'field' }, el('label', {}, 'Send To'), sinkBox));

      // ---- advanced
      const adv = el('details', { class: 'fold' }, el('summary', {}, el('span', {}, 'Advanced')),
        el('div', { class: 'card tight' },
          el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Cooldown'), el('div', { class: 'help' }, 'Seconds before this can send again')),
            el('input', { type: 'text', inputmode: 'numeric', value: r.cooldown_s ?? 600, onchange: (e) => (r.cooldown_s = parseInt(e.target.value, 10) || 0) })),
          el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Repeat Every'), el('div', { class: 'help' }, 'Seconds; 0 = send once until it goes false')),
            el('input', { type: 'text', inputmode: 'numeric', value: r.repeat_s ?? 0, onchange: (e) => (r.repeat_s = parseInt(e.target.value, 10) || 0) })),
          el('label', { class: 'toggle' }, el('div', {}, el('div', {}, 'Only While Cooking'), el('div', { class: 'help muted', style: 'font-size:.76rem' }, 'Off means it can also fire while the grill is stopped')),
            el('span', { class: 'switch' }, el('input', { type: 'checkbox', checked: r.only_while_cooking !== false, onchange: (e) => (r.only_while_cooking = e.target.checked) }), el('span')))));

      body.append(
        el('div', { class: 'field' }, el('label', {}, 'Name'),
          el('input', { type: 'text', value: r.name || '', onchange: (e) => (r.name = e.target.value) })),
        watch, conds, msg, alert, adv);
      refreshPreview();
    };
    draw();

    wrap.append(el('h3', {}, isNew ? 'New Notification' : r.name), body,
      el('div', { class: 'btnrow', style: 'margin-top:12px' },
        el('button', { class: 'btn', type: 'button', onclick: async () => {
          try { await api('/rules/test', { body: r }); toast('Sent — check your phone'); } catch (e) { toast(e.message, true); }
        } }, 'Send Test'),
        el('button', { class: 'btn primary', type: 'button', onclick: () => close(r) }, 'Save')),
      el('div', { class: 'btnrow', style: 'margin-top:8px' },
        isNew ? null : el('button', { class: 'btn ghost', type: 'button', onclick: () => close('delete') }, 'Delete'),
        el('button', { class: 'btn ghost', type: 'button', onclick: async () => {
          if (JSON.stringify(r) !== JSON.stringify(rule) && !await confirmDialog('Discard changes?', r.name || '', 'Discard', true)) return;
          close(undefined);
        } }, 'Cancel')));
    return wrap;
  });
}

// ---- the table ----------------------------------------------------------

export async function renderRules(view) {
  await catalogue();
  const data = await api('/rules');
  let rules = data.rules || [];

  const list = el('div', { class: 'list' });
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
      list.append(el('div', { class: `item rule-row ${r.enabled === false ? 'off' : ''}` },
        el('button', { class: 'rule-main', type: 'button', onclick: () => edit(r, false) },
          el('div', { class: 'row', style: 'gap:8px' }, el('strong', {}, r.name || r.id),
            el('span', { class: `pill sm lvl-${r.level || 'normal'}` }, (r.level || 'normal').toUpperCase())),
          el('div', { class: 'meta' }, summarise(r))),
        sw));
    }
    if (!rules.length) list.append(el('div', { class: 'muted', style: 'padding:12px' }, 'No conditional notifications yet.'));
  };
  draw();

  view.append(
    el('div', { class: 'row between' }, el('h2', {}, 'Conditional Notifications'),
      el('button', { class: 'btn sm', type: 'button', onclick: () => edit(blankRule(), true) }, '+ Add')),
    el('p', { class: 'muted', style: 'font-size:.85rem' },
      'Each notification watches something on the grill and sends a message when what it describes becomes true. One rule can cover every probe: compare a reading with another reading, like "temperature is at or above its target", and the message names whichever probe matched.'),
    el('div', { class: 'card' }, list));
}
