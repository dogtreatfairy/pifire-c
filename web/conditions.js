/* The condition builder: the cards, the rows and the picker that "Conditional Notifications" is
 * made of, and that anything else asking the same kind of question is made of too.
 *
 * It lived inside the notifications page, so when recipes needed to say how a step ends they got a
 * lookalike built out of segmented controls instead -- two things that ask the same question in
 * two different visual languages, which is the thing this project is least allowed to do. One
 * builder, one set of shapes: a fold per condition with its own summary in the header, AND / OR /
 * NOT as the marks &, \u2265 and \u2260, one "Add condition" that asks what kind, and a duration on
 * any node. See docs/design-language.md.
 *
 * Nothing here hardcodes what can be asked about: the daemon publishes the entity catalogue and
 * these dropdowns are built from it, so a trait added in C shows up on the next load. */
import { el, api, degUnit, dialog, actionBtn, iconBtn, segmented } from './app.js';

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
  /* the unit, so a summary says 250 F rather than a bare 250 that could be either */
  const d = traitDef(from, node.trait);
  const u = d?.type === 'temperature' ? degUnit() : d?.type === 'percent' ? '%' : '';
  const vs = Array.isArray(v) ? ` ${v.join(' or ')}`
    : v && typeof v === 'object' && v.trait
      ? `${v.entity ? ` ${titleCase(v.entity)} ${traitLabel(v.entity, v.trait)}` : ` its ${traitLabel(from, v.trait)}`}${
          v.offset ? ` ${v.offset > 0 ? '+' : '\u2212'} ${Math.abs(v.offset)}` : ''}`
      : v === undefined ? '' : ` ${v}${u}`;
  return `${lhs} ${OP_LABEL[node.op] || node.op}${vs}${node.value2 !== undefined ? ` \u00b1 ${node.value2}${u}` : ''}${forLabel(node.for_s)}`;
}

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
        /* The unit sits beside the number, and it is the grill's unit today -- the stored value is
           rewritten when the units change (see convert_rules_units in rules.c), so what is typed as
           15 C reads as 59 F afterwards rather than as a 15 that now means something else. */
        const suffix = def?.type === 'temperature' ? degUnit()
                     : def?.type === 'duration' ? 's' : def?.unit === '%' ? '%' : '';
        const withUnit = (input) => suffix
          ? el('div', { class: 'c-unit' }, input, el('span', {}, suffix)) : input;
        if (asTrait) {
          valueField = el('div', { class: 'c-val c-operand' }, chooser,
            withUnit(el('input', { type: 'text', inputmode: 'decimal', class: 'c-off',
              value: cond.value.offset ?? '', placeholder: '\u00b1 0',
              title: 'Offset on that reading',
              onchange: (e) => { const n = parseFloat(e.target.value); if (n) cond.value.offset = n; else delete cond.value.offset; onChange(); } })));
        } else {
          valueField = el('div', { class: 'c-val c-operand' }, chooser,
            withUnit(el('input', { type: 'text', inputmode: 'decimal', class: 'c-num', value: cond.value ?? '',
              placeholder: suffix || 'value',
              onchange: (e) => { cond.value = parseFloat(e.target.value) || 0; onChange(); } })));
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
/* Seconds are what is stored; the unit is only how the number is written. A whole number of
   minutes is written in minutes, anything else in seconds, so 90 reads as 90 sec and not 1.5 min. */
const forUnit = (node) => node._for_unit || (node.for_s && node.for_s % 60 === 0 ? 'min' : 's');

const forField = (node, onChange) => {
  const unit = forUnit(node);
  /* No duration reads as "0 min", not "immediately". A condition that answers the moment it is
     true is a condition held for zero time, and saying so keeps the field one kind of thing --
     a number with a unit -- instead of a number that sometimes turns into a word. */
  const input = el('input', {
    type: 'text', inputmode: 'decimal', style: 'flex:1 1 auto; min-width:0', placeholder: '0',
    value: String(unit === 'min' ? (node.for_s || 0) / 60 : (node.for_s || 0)),
    onchange: (e) => {
      const n = parseFloat(e.target.value);
      node.for_s = n > 0 ? Math.round(forUnit(node) === 'min' ? n * 60 : n) : 0;
      e.target.value = String(forUnit(node) === 'min' ? node.for_s / 60 : node.for_s);
      onChange();
    },
  });
  return el('div', { class: 'field' },
    el('label', {}, 'For'),
    el('div', { class: 'row', style: 'gap:var(--sp-2)' }, input,
      /* Switching the unit keeps the number written and reinterprets it: 5 min becomes 5 sec,
         which is what someone correcting the unit meant, rather than 0.08 min. */
      segmented([['min', 'min'], ['s', 'sec']], unit, (u) => {
        node._for_unit = u;
        node.for_s = Math.round((parseFloat(input.value) || 0) * (u === 'min' ? 60 : 1));
        onChange();
      }, { hug: true })));
};

/* What can be added, as a list of kinds -- the shape Home Assistant uses. A comparison first,
   because it is what most rules are made of, then the three ways of joining them. */
function addKind(node, domain, depth, done) {
  const t = traitsOf(domain)[0];
  const kinds = [
    { glyph: '123', name: 'Condition', sub: 'Compare a reading with a value',
      make: () => ({ trait: t?.id || 'temp', op: t?.operators?.[0] || '>=', value: 0 }) },
    ...(depth < 3 ? [
      { glyph: '&', name: 'AND', sub: 'True when all are true', make: () => ({ op: 'all', conditions: [] }) },
      { glyph: '\u2265', name: 'OR', sub: 'True when any is true', make: () => ({ op: 'any', conditions: [] }) },
      { glyph: '\u2260', name: 'NOT', sub: 'True when the inside is false', make: () => ({ op: 'not', conditions: [] }) },
    ] : []),
  ];
  return dialog((close) => el('div', {},
    el('h3', {}, 'Add condition'),
    el('div', { class: 'ios-list', style: 'margin-top:var(--sp-2)' },
      kinds.map((k) => el('button', { class: 'irow kind-row', type: 'button', onclick: () => { node.conditions.push(k.make()); close(); done(); } },
        el('span', { class: 'cc-glyph' }, k.glyph),
        el('span', { class: 'body' }, el('span', { class: 't' }, k.name), el('span', { class: 's' }, k.sub))))),
    el('div', { class: 'form-actions' }, actionBtn('cancel', 'Cancel', { size: '', onclick: () => close() }))));
}

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
      /* One button, and it asks what KIND of thing to add.
       *
       * Two buttons -- "Condition" and "Group" -- made the reader work out what a group was before
       * they could use one, and buried AND, OR and NOT inside whichever of the two happened to
       * mean them. Home Assistant offers a single Add condition that opens a list of the kinds,
       * with the logical ones sitting in the same list as the rest, and that is right: adding "or"
       * is adding a condition, it is just a condition made of other conditions. */
      body.append(el('div', { class: 'cc-add' },
        actionBtn('add', 'Add condition', { onclick: () => addKind(node, domain, depth, () => { draw(); retitle(); onChange(); }) })));
    } else {
      body.append(conditionRow(node, domain, () => { retitle(); onChange(); }));
    }
    body.append(forField(node, () => { retitle(); onChange(); }));
    retitle();
  };
  draw();
  return det;
}

export { OP_LABEL, GROUP_OPS, titleCase, isGroup, listOp, forLabel, describeNode,
         catalogue, domainOf, traitsOf, traitDef, traitLabel, conditionRow, forField, addKind, condNode };
/* The raw catalogue, for the few places that need more of it than the helpers expose. */
export const cat = () => CAT;
