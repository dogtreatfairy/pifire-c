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
import { icon } from './icons.js';

/* How an operator reads in a card's header, where there is one line and the rest of the row is
   folded away: a symbol, so "Hottest Food Probe \u2265 160\u00b0F" fits where "is at or above" did not. */
const OP_SYM = {
  '>': '>', '>=': '\u2265', '<': '<', '<=': '\u2264', '==': '=', '!=': '\u2260',
  between: 'between', within: 'within \u00b1', is_one_of: 'is one of', is_none_of: 'is none of',
  is_on: 'is on', is_off: 'is off', is: 'is', is_not: 'is not', contains: 'contains', empty: 'is empty', not_empty: 'is not empty',
};
/* A duration the way a cook says it: 3 h, 90 min, 45 s. */
const fmtSecs = (v) => (!(v > 0) ? `${v} s` : v % 3600 === 0 ? `${v / 3600} h` : v >= 3600 ? `${Math.floor(v / 3600)} h ${Math.round((v % 3600) / 60)} min`
  : v % 60 === 0 ? `${v / 60} min` : `${v} s`);
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
  const num = (x) => (d?.type === 'duration' ? fmtSecs(x) : `${x}${u}`);
  const vs = Array.isArray(v) ? ` ${v.join(' or ')}`
    : v && typeof v === 'object' && v.trait
      ? `${v.entity ? ` ${titleCase(v.entity)} ${traitLabel(v.entity, v.trait)}` : ` its ${traitLabel(from, v.trait)}`}${
          v.offset ? ` ${v.offset > 0 ? '+' : '\u2212'} ${Math.abs(v.offset)}` : ''}`
      : v === undefined ? '' : ` ${num(v)}`;
  const isOn = node.op === 'is_on' || node.op === 'is_off';
  /* "Lid Opened" rather than "Lid Opened is on": a yes-or-no thing said once */
  if (isOn && !top) return `${node.op === 'is_off' ? 'not ' : ''}${lhs}${forLabel(node.for_s)}`;
  return `${lhs} ${OP_SYM[node.op] || node.op}${vs}${node.value2 !== undefined ? ` ${node.op === 'within' ? '' : 'and '}${num(node.value2)}` : ''}${forLabel(node.for_s)}`;
}

/* Every kind of condition has a mark, and it is the same mark wherever that kind appears: on the
   condition's card, in the Add condition list, on a recipe's rail and in a recipe row's summary.
   A thermometer is a temperature, a stopwatch is time on the clock, an hourglass is time still to
   run, a hand is the cook, an open door is the lid. Chosen by what is being read before by what
   type of number it is, so a probe's battery is a battery and not a gauge. */
const TRAIT_ICON = {
  elapsed: 'timer', cook_elapsed: 'timer', mode_remaining: 'timer', aiming_s: 'timer', remaining: 'timer', running: 'timer',
  eta: 'hourglass', food_eta: 'hourglass', battery: 'battery', food_battery: 'battery',
  prompt: 'hand', lid: 'door-open', lid_open: 'door-open',
  mode: 'circle-gauge', error: 'triangle-alert', level: 'package',
  signal: 'wifi', rssi: 'wifi', wifi_signal: 'wifi', connected: 'bluetooth', wireless: 'bluetooth', tailscale_online: 'network',
  state: 'zap', percent: 'zap', duty: 'sliders-horizontal', feedforward: 'sliders-horizontal',
};
const TYPE_ICON = { temperature: 'thermometer', duration: 'timer', percent: 'gauge', bool: 'circle-check', enum: 'list' };
function traitIcon(domain, t) {
  if (domain === 'weather') return 'cloud-sun';
  return TRAIT_ICON[t?.id] || TYPE_ICON[t?.type] || 'activity';
}
function condIcon(node, domain) {
  const from = node.entity && node.entity !== 'this' ? node.entity : domain;
  return traitIcon(from, traitDef(from, node.trait));
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
    /* An entity naming the watched domain itself is "this": the shipped grill rules say
       entity "grill" on a grill rule, and without this the select fell back to its first option and
       showed "Mode" over a condition about the error code. */
    if (cond.entity === domain) delete cond.entity;
    const cur = `${cond.entity && cond.entity !== 'this' ? cond.entity : 'this'}:${cond.trait}`;
    const groups = [el('optgroup', { label: `This ${titleCase(domain)}` },
      traitsOf(domain).map((t) => el('option', { value: `this:${t.id}`, selected: cur === `this:${t.id}` }, t.label || titleCase(t.id))))];
    for (const d of CAT.domains) {
      if (d.multi || d.id === domain) continue;
      groups.push(el('optgroup', { label: titleCase(d.id) },
        d.traits.map((t) => el('option', { value: `${d.id}:${t.id}`, selected: cur === `${d.id}:${t.id}` }, `${titleCase(d.id)} ${t.label || titleCase(t.id)}`))));
    }
    /* The reading on a line of its own, full width, so "Hottest Food Probe, Rested" is readable;
       then how it is compared and with what, side by side. Two selects sharing one line cut every
       label in half at phone width. */
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
      if (!isList && def?.type === 'duration' && !asTrait) {
        /* A time, written the way the For field is written: a number and min or sec. Nobody says
           "10800 s" for three hours, and a row that made them work that out was one nobody read. */
        const unit = cond._unit || ((cond.value || 0) % 60 === 0 && (cond.value || 0) >= 60 ? 'min' : 's');
        const shown = unit === 'min' ? (cond.value || 0) / 60 : (cond.value || 0);
        const input = el('input', { type: 'text', inputmode: 'decimal', class: 'c-num', value: String(shown), placeholder: '0',
          onchange: (e) => { const n = parseFloat(e.target.value) || 0; cond.value = Math.round((cond._unit || unit) === 'min' ? n * 60 : n); onChange(); } });
        valueField = el('div', { class: 'c-val c-dur' }, input,
          segmented([['min', 'min'], ['s', 'sec']], unit, (u) => { cond._unit = u; cond.value = Math.round((parseFloat(input.value) || 0) * (u === 'min' ? 60 : 1)); onChange(); }, { hug: true }));
      } else if (asTrait || (!isList && def?.type !== 'enum')) {
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
        /* Comparing with another reading -- "below the set point + 15" -- is worth having and
           rarely wanted, so it is one tap away rather than a select reading "a number" in every
           row, which asked a question about the editor instead of about the grill. */
        const chooser = !asTrait ? null : el('select', { class: 'c-val', onchange: (e) => {
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
        const canReading = def?.type === 'temperature';
        const alt = (label, onclick) => el('button', { class: 'btn xs ghost c-alt', type: 'button', onclick }, label);
        if (asTrait) {
          valueField = el('div', { class: 'c-val c-operand wide' }, chooser,
            withUnit(el('input', { type: 'text', inputmode: 'decimal', class: 'c-off',
              value: cond.value.offset ?? '', placeholder: '\u00b1 0',
              title: 'Offset on that reading',
              onchange: (e) => { const n = parseFloat(e.target.value); if (n) cond.value.offset = n; else delete cond.value.offset; onChange(); } })),
            alt('Use a number', () => { cond.value = 0; draw(); onChange(); }));
        } else {
          valueField = el('div', { class: 'c-val' },
            withUnit(el('input', { type: 'text', inputmode: 'decimal', class: 'c-num', value: cond.value ?? '',
              placeholder: suffix || 'value',
              onchange: (e) => { cond.value = parseFloat(e.target.value) || 0; onChange(); } })));
        }
        /* after the value, not before it: the number belongs beside its operator */
        const altBtn = !asTrait && canReading ? alt('Compare with a reading instead', () => {
          const t = traitsOf(domain).find((x) => x.type === 'temperature' && x.id !== cond.trait) || traitsOf(domain)[0];
          cond.value = { trait: t.id }; draw(); onChange();
        }) : null;
        if (altBtn) queueMicrotask(() => row.append(altBtn));
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
/* What can be added, listed by what it IS -- Hottest Food Probe, Time In This Step, Pit
 * Temperature -- under the heading it belongs to, the way Home Assistant's condition picker lists
 * the things it can test. Picking one adds a row already aimed at that reading, so the reader
 * never sees a row that says "Temperature" and has to work out whose.
 *
 * This replaced a picker of KINDS -- "Condition, AND, OR, NOT" -- which asked the reader to decide
 * what shape of thing they wanted before they could say what they wanted to test. The groups are
 * still there for an editor that allows nesting, but at the end, under their own heading, and
 * NOT is gone: every operator has its opposite, and "is not connected" reads better than a NOT
 * around "is connected". */
function addKind(node, domain, depth, done, opts = {}) {
  const exclude = new Set(opts.exclude || []);
  const sections = [];
  const push = (heading, entries) => { if (entries.length) sections.push({ heading, entries }); };
  const rowFor = (entity, t) => ({
    icon: traitIcon(entity || domain, t),
    name: t.label || titleCase(t.id),
    sub: t.type === 'temperature' ? `Temperature, ${degUnit()}` : t.type === 'duration' ? 'Time'
       : t.type === 'percent' ? 'Percent' : t.type === 'bool' ? 'Yes or no' : t.type === 'enum' ? 'One of a list' : '',
    make: () => ({ ...(entity ? { entity } : {}), trait: t.id, op: t.operators?.[0] || '>=', value: t.type === 'bool' || t.type === 'enum' ? undefined : 0 }),
  });
  /* the watched thing's own readings first, under whatever headings the catalogue gives them */
  const own = traitsOf(domain).filter((t) => !exclude.has(t.id));
  const byCat = new Map();
  for (const t of own) { const c = t.category || `This ${titleCase(domain)}`; (byCat.get(c) || byCat.set(c, []).get(c)).push(rowFor(null, t)); }
  for (const [heading, entries] of byCat) push(heading, entries);
  /* then everything else that has one instance: the grill, the hopper, the weather */
  for (const d of cat()?.domains || []) {
    if (d.multi || d.id === domain || d.id === 'step') continue;
    push(titleCase(d.id), d.traits.map((t) => rowFor(d.id, t)));
  }
  if (opts.groups !== false && depth < 3) {
    push('Groups', [
      { glyph: '&', name: 'AND', sub: 'True when all of the conditions inside it are', make: () => ({ op: 'all', conditions: [] }) },
      { glyph: '≥', name: 'OR', sub: 'True when any one of them is', make: () => ({ op: 'any', conditions: [] }) },
    ]);
  }
  return dialog((close) => el('div', { class: 'sheet' },
    el('div', { class: 'sheet-head' }, el('h3', {}, 'Add condition')),
    el('div', { class: 'sheet-body pick-list' },
      sections.map((sec) => el('div', { class: 'pick-sec' },
        el('h2', {}, sec.heading),
        el('div', { class: 'ios-list' }, sec.entries.map((k) => el('button', { class: 'irow kind-row', type: 'button',
          onclick: () => { node.conditions.push(k.make()); close(); done(); } },
          el('span', { class: 'cc-glyph' }, k.glyph || icon(k.icon)),
          el('span', { class: 'body' }, el('span', { class: 't' }, k.name), k.sub ? el('span', { class: 's' }, k.sub) : null))))))),
    el('div', { class: 'form-actions' }, actionBtn('cancel', 'Cancel', { size: '', onclick: () => close() }))));
}

/* A condition, and a group of conditions, are both a card: a header you can read with it shut and
   a body you open to change it. Nesting is what lets one rule say "the mode is Hold or Smoke, and
   the pit is within 15 of the set point": the OR has to bind tighter than the AND, and a flat list
   cannot express that. The card is the app's own fold -- the same shape every collapsible section
   uses -- rather than a second collapsible invented for this screen. */
/* opts.flat: one level only -- a list joined by AND or OR, nothing nested and no NOT. It is what a
   recipe step wants: "three hours, or the meat is at 160" is a list, and a list is all most rules
   are too. opts.exclude: traits the picker must not offer here. */
function condNode(node, domain, onChange, onRemove, depth, opts = {}) {
  const group = isGroup(node);
  /* The root of a flat list is not a card: it is the list. Wrapping it in its own fold put a
     header reading "OR - 2" over rows that already said what they were, one more border inside
     the step's, and that nesting is what made the whole thing hard to read. */
  if (opts.flat && depth === 0 && group) {
    const wrap = el('div', { class: 'cond-flat' });
    const draw = () => {
      wrap.innerHTML = '';
      if (node.op === 'not') node.op = 'all';
      const ops = GROUP_OPS.filter(([v]) => v !== 'not').map(([v, l]) => [v, l]);
      if (node.conditions.length > 1) wrap.append(segmented(ops, node.op || 'all', (v) => { node.op = v; onChange(); }));
      node.conditions.forEach((k, i) => wrap.append(condNode(k, domain, onChange,
        () => { node.conditions.splice(i, 1); draw(); onChange(); }, 1, { ...opts, ix: i })));
      wrap.append(el('div', { class: 'cc-add' },
        actionBtn('add', 'Add condition', { onclick: () => addKind(node, domain, 0, () => { draw(); onChange(); }, { groups: false, exclude: opts.exclude }) })));
    };
    draw();
    return wrap;
  }
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
    if (group) glyph.textContent = g?.[2] || '&';
    else glyph.replaceChildren(icon(condIcon(node, domain)));
    const said = describeNode(node, domain, false);
    title.textContent = group ? `${g?.[1] || 'AND'}${node.conditions?.length ? ` \u00b7 ${node.conditions.length}` : ''}`
                              : (said || 'New condition');
  };

  const draw = () => {
    body.innerHTML = '';
    if (group) {
      /* The operator is a choice of three, so it is three buttons rather than a dropdown you have
         to open to find out what the options were. */
      const ops = (opts.flat ? GROUP_OPS.filter(([v]) => v !== 'not') : GROUP_OPS).map(([v, l]) => [v, l]);
      if (opts.flat && node.op === 'not') node.op = 'all';
      body.append(segmented(ops, node.op || 'all', (v) => { node.op = v; retitle(); onChange(); }));
      node.conditions.forEach((k, i) => body.append(condNode(k, domain, () => { retitle(); onChange(); },
        () => { node.conditions.splice(i, 1); draw(); retitle(); onChange(); }, depth + 1, { ...opts, ix: i })));
      /* One button: it opens the list of things that can be tested, by name and by heading. */
      body.append(el('div', { class: 'cc-add' },
        actionBtn('add', 'Add condition', { onclick: () => addKind(node, domain, depth, () => { draw(); retitle(); onChange(); }, { groups: !opts.flat, exclude: opts.exclude }) })));
    } else {
      body.append(conditionRow(node, domain, () => { retitle(); onChange(); }));
    }
    /* "For ten minutes" on a time already measured in minutes, or on a yes-or-no, means nothing;
       on a temperature it is the difference between a blip and a stall. */
    /* "For" is a property of a condition, as it is in Home Assistant: the hopper below 10% for
       thirty seconds. A second For on the group -- "and all of that, for how long?" -- was a
       second answer to the same question, and a rule read "for 0 minutes" in one place and "for
       30 seconds" in another. A group carries none; an older rule that put one on its group has it
       moved onto the conditions inside when the rule is opened. */
    const def = group ? null : traitDef(node.entity && node.entity !== 'this' ? node.entity : domain, node.trait);
    if (!group && def && def.type !== 'duration' && def.type !== 'bool') body.append(forField(node, () => { retitle(); onChange(); }));
    retitle();
  };
  draw();
  return det;
}

/* An older rule's group-level For, moved onto the conditions it contains: each of them held for
   that long says what the group held for that long said. */
export function hoistFor(node) {
  if (!Array.isArray(node?.conditions)) return;
  const secs = node.for_s || 0;
  delete node.for_s;
  for (const k of node.conditions) {
    if (Array.isArray(k.conditions)) { if (secs && !k.for_s) k.for_s = secs; hoistFor(k); }
    else if (secs && !k.for_s) k.for_s = secs;
  }
}
export { traitIcon, condIcon, OP_LABEL, OP_SYM, fmtSecs, GROUP_OPS, titleCase, isGroup, listOp, forLabel, describeNode,
         catalogue, domainOf, traitsOf, traitDef, traitLabel, conditionRow, forField, addKind, condNode };
/* The raw catalogue, for the few places that need more of it than the helpers expose. */
export const cat = () => CAT;
