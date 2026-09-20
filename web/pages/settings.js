import { PF, el, api, patchSettings, toast, degUnit, confirmDialog } from '../app.js';
import { renderProbes } from './probes.js';
import { listGroup } from '../app.js';

// Field descriptors: path relative to the group, type: num|int|bool|select|text|temp|tempdelta
const T = (path, label, help, extra = {}) => ({ path, label, help, type: 'temp', ...extra });
const N = (path, label, help, extra = {}) => ({ path, label, help, type: 'num', ...extra });
const I = (path, label, help, extra = {}) => ({ path, label, help, type: 'int', ...extra });
const B = (path, label, help) => ({ path, label, help, type: 'bool' });
const S = (path, label, help, options, bool = false) => ({ path, label, help, type: 'select', options, bool });
const X = (path, label, help) => ({ path, label, help, type: 'text' });

// Settings pages. Each page holds one or more sections; a section maps to one settings group (its `id`)
// and saves independently, so a page can combine related groups (e.g. startup + shutdown).
const PAGES = [
  // ---- Grill
  { key: 'controller', title: 'Temperature control', sub: 'Control algorithm and tuning', section: 'Cooking', icon: 'gauge', color: '#ff8a1f', controller: true },
  { key: 'probes', title: 'Probes', sub: 'Wired and Bluetooth probes, profiles, tuner', section: 'Hardware', icon: 'thermometer', color: '#ff453a', custom: renderProbes },
  { key: 'auger', title: 'Auger & feed', sub: 'Cycle length, feed limits, P-mode', section: 'Cooking', icon: 'sliders-horizontal', color: '#ff9f0a', sections: [{ id: 'cycle_data', fields: [
    I('HoldCycleTime', 'Control cycle (s)', 'Length of one auger cycle while holding a temperature; the controller decides the feed once per cycle', { min: 5, max: 120 }),
    N('u_min', 'Minimum auger duty', 'Smallest fraction of each cycle the auger runs (0.1 = 10%). Keeps the fire alive at low set points', { step: 0.01, min: 0, max: 1 }),
    N('u_max', 'Maximum auger duty', 'Largest fraction of each cycle the auger runs (0.9 = 90%). Stops the pot from over-filling', { step: 0.01, min: 0, max: 1 }),
    I('SmokeOnCycleTime', 'Smoke: auger on (s)', 'Auger run time per cycle in Smoke and during startup', { min: 1 }),
    I('SmokeOffCycleTime', 'Smoke: auger off (s)', 'Base pause between runs in Smoke; each P-mode level adds 10 s', { min: 1 }),
    I('PMode', 'P-mode', 'Higher = longer pauses = less pellets and more smoke (0–9)', { min: 0, max: 9 }),
    B('FanPidEnabled', 'Modulate AC fan at minimum feed', 'When the auger is already at its minimum duty, pulse the fan to hold temperature (AC fans only)'),
  ] }] },
  { key: 'startup', title: 'Startup & shutdown', sub: 'Ignition, what happens after startup, cool-down', section: 'Cooking', icon: 'power', color: '#30d158', sections: [
    { id: 'startup', title: 'Startup', fields: [
      I('duration', 'Startup time (s)', 'Igniter and startup feed run for this long', { min: 60, max: 900 }),
      T('startup_exit_temp', 'End startup early at', 'Leave startup as soon as the pit reaches this temperature (0 = wait for the timer)', { allowZero: true }),
      { path: 'exit_rise', label: 'End startup after a rise of', help: 'Leave startup once the pit has climbed this much above where it was when you pressed start - proof the fire is lit (0 = off)', type: 'tempdelta' },
      S('start_to_mode.after_startup_mode', 'After startup go to', '', [['Smoke', 'Smoke'], ['Hold', 'Hold']]),
      T('start_to_mode.primary_setpoint', 'Default hold temperature', 'Used when starting into Hold without choosing a temperature'),
      I('prime_on_startup', 'Prime before startup (g)', 'Pellets pushed into the pot before igniting (0 = off)', { min: 0 }),
      I('pwm_duty_cycle', 'Fan speed during startup (%)', 'DC fan only', { min: 10, max: 100 }),
      B('smartstart.enabled', 'Smart Start', 'Choose the startup profile from how warm the pit already is'),
      T('smartstart.exit_temp', 'Smart Start exit temperature', ''),
    ] },
    { id: 'shutdown', title: 'Shutdown', fields: [
      I('shutdown_duration', 'Cool-down fan time (s)', 'The fan keeps running this long after the auger stops', { min: 30 }),
      B('auto_power_off', 'Power off the Pi after shutdown', ''),
    ] },
  ] },
  { key: 'smoke', title: 'Smoke & Smoke+', sub: 'Default smoke mode and fan cycling', section: 'Cooking', icon: 'cloud', color: '#8e8e93', sections: [{ id: 'smoke_plus', fields: [
    S('enabled', 'Default smoke mode', 'Which mode Smoke starts in; switch any time from the Home screen', [['false', 'Smoke'], ['true', 'Smoke+']], true),
    T('min_temp', 'Smoke+ works above', 'Below this the fan stays on continuously'),
    T('max_temp', 'Smoke+ works below', 'Above this the fan stays on continuously'),
    I('on_time', 'Fan on (s)', '', { min: 1 }), I('off_time', 'Fan off (s)', '', { min: 1 }),
    B('fan_ramp', 'Ramp fan speed', 'DC fan only: ramp up instead of switching'), I('duty_cycle', 'Ramp target speed (%)', 'DC fan only', { min: 10, max: 100 }),
  ] }] },
  { key: 'fan', title: 'DC fan', sub: 'PWM speed control', section: 'Hardware', icon: 'fan', color: '#64d2ff', dc: true, sections: [{ id: 'pwm', fields: [
    B('pwm_control', 'Vary fan speed with temperature', 'Default for new cooks; can be changed while cooking'),
    I('frequency', 'PWM frequency (Hz)', '25 000 Hz for 4-wire PC fans', { min: 100, max: 100000 }),
    I('min_duty_cycle', 'Minimum fan speed (%)', 'Some fans stall below this', { min: 0, max: 100 }),
    I('max_duty_cycle', 'Maximum fan speed (%)', '', { min: 10, max: 100 }),
    I('update_time', 'Speed update interval (s)', '', { min: 1 }),
  ] }] },
  { key: 'learning', title: 'Learning', sub: 'What the grill learns from every cook', section: 'Cooking', icon: 'brain', color: '#bf5af2', sections: [{ id: 'learning', fields: [
    B('enabled', 'Learn from cooks', 'Record the steady feed for each set point and ambient temperature, and the plant model from every startup'),
    B('auto_tune', 'Apply learned tuning automatically', 'Hand the measured plant model (and autotune results) to the controller as soon as they are known'),
    I('half_life_obs', 'Memory half-life (observations)', 'How quickly old cooks fade; ~12 observations per hour of Hold', { min: 5, max: 500 }),
  ] }] },
  { key: 'lid', title: 'Lid-open detection', sub: 'Pause the feed when the lid is opened', section: 'Cooking', icon: 'lock-open', color: '#ffd60a', sections: [{ id: 'cycle_data', fields: [
    B('LidOpenDetectEnabled', 'Detect an open lid', 'A sudden temperature drop pauses the auger so the pot does not overfill'),
    I('LidOpenThreshold', 'Drop that counts as open (%)', 'Percentage below the set point', { min: 1, max: 50 }),
    I('LidOpenPauseTime', 'Pause length (s)', '', { min: 10 }),
  ] }] },
  // ---- Safety
  { key: 'safety', title: 'Temperature limits', sub: 'High-temperature cutoff, flame-out detection', section: 'Safety', icon: 'shield-check', color: '#ff453a', sections: [{ id: 'safety', fields: [
    T('maxtemp', 'High-temperature cutoff', 'Above this in any mode everything shuts off and the grill goes to Error'),
    B('startup_check', 'Flame-out detection', 'Watch for the pit dropping below the flame-out floor in Smoke and Hold'),
    T('minstartuptemp', 'Flame-out floor (minimum)', 'Lowest floor used after a normal startup'),
    T('maxstartuptemp', 'Flame-out floor (maximum)', ''),
    I('reigniteretries', 'Re-ignite attempts', 'Tries to re-light after a flame-out before going to Error', { min: 0, max: 5 }),
    I('probe_fault_s', 'Pit probe timeout (s)', 'Seconds without a valid pit reading before Error', { min: 3 }),
    I('error_cooldown_fan_s', 'Fan run after an error (s)', 'Cools the pot when the grill errors while hot', { min: 0 }),
  ] }] },
  { key: 'coldstart', title: 'Cold-weather start', sub: 'Confirm ignition by temperature rise instead of a fixed floor', section: 'Safety', icon: 'snowflake', color: '#5ac8fa', sections: [{ id: 'safety', fields: [
    B('coldstart.enabled', 'Cold-weather start', 'Keep starting until the pit has risen from its cold baseline; for freezing conditions'),
    { path: 'coldstart.delta_rise', label: 'Rise that confirms ignition', help: 'Above the baseline measured in the first minute', type: 'tempdelta' },
    I('coldstart.timeout_s', 'Give up after (s)', '0 = same as the startup time', { min: 0 }),
    B('coldstart.exit_on_rise', 'End startup once the rise is confirmed', 'Otherwise the full startup time runs'),
  ] }] },
  { key: 'limits', title: 'Output limits & manual control', sub: 'Igniter and auger time caps, manual overrides', section: 'Safety', icon: 'zap', color: '#ff9f0a', sections: [{ id: 'safety', fields: [
    I('igniter_max_on_s', 'Igniter maximum on time (s)', 'The igniter is forced off after this', { min: 60 }),
    I('auger_max_on_s', 'Auger maximum continuous run (s)', 'Absolute cap, regardless of controller or manual control', { min: 5 }),
    B('allow_manual_changes', 'Allow manual outputs while cooking', 'Temporarily override outputs from More → Manual outputs'),
    I('manual_override_time', 'Manual override lasts (s)', '', { min: 5 }),
  ] }] },
  // ---- Cook
  { key: 'keepwarm', title: 'Keep warm', sub: 'After a probe reaches its target', section: 'Cooking', icon: 'flame', color: '#ff6b35', sections: [{ id: 'keep_warm', fields: [T('temp', 'Keep-warm temperature', ''), B('s_plus', 'Use Smoke+ while keeping warm', '')] }] },
  { key: 'pellets', title: 'Pellets & hopper', sub: 'Low-pellet warnings, hopper sensor calibration', section: 'Hardware', icon: 'package', color: '#ac8e68', sections: [{ id: 'pelletlevel', fields: [
    B('warning_enabled', 'Low-pellet warnings', ''), I('warning_level', 'Warn below (%)', '', { min: 1, max: 99 }), I('warning_time', 'Repeat every (min)', '', { min: 1 }),
    I('empty', 'Sensor reading when empty (cm)', 'Distance from the sensor to the bottom of the hopper', { min: 1 }), I('full', 'Sensor reading when full (cm)', '', { min: 0 }),
  ] }] },
  { key: 'history', title: 'History & cook files', sub: 'Chart sampling and retention', section: 'Data', icon: 'database', color: '#5e5ce6', sections: [{ id: 'history', fields: [
    I('sample_s', 'Sample every (s)', '', { min: 1, max: 60 }), I('retention_hours', 'Keep for (hours)', '', { min: 1 }), B('clear_on_startup', 'Clear the chart when a cook starts', ''),
  ] }] },
  // ---- Connectivity
  { key: 'integrations', title: 'Notifications & integrations', sub: 'MQTT, Home Assistant, webhooks', section: 'Connectivity', icon: 'bell', color: '#ff453a', sections: [{ id: 'notify', fields: [
    B('mqtt.enabled', 'MQTT', 'Publish state to a broker, with Home Assistant discovery'), X('mqtt.broker', 'Broker host', ''), I('mqtt.port', 'Broker port', '', { min: 1, max: 65535 }),
    X('mqtt.username', 'Username', ''), { path: 'mqtt.password', label: 'Password', type: 'password' }, X('mqtt.id', 'Device ID', 'Topic prefix'), I('mqtt.update_sec', 'Publish every (s)', '', { min: 5 }),
    B('webhook.enabled', 'Webhook', 'POST events as JSON to a URL'), X('webhook.url', 'Webhook URL', ''),
  ] }] },
  { key: 'hotspot', title: 'Setup hotspot', sub: 'Fallback access point when no Wi-Fi is known', section: 'Connectivity', icon: 'router', color: '#30d158', sections: [{ id: 'network', fields: [
    X('hotspot_ssid', 'Hotspot name', 'Blank = PiFire-XXXX from the Wi-Fi address'),
    { path: 'hotspot_password', label: 'Hotspot password', help: 'At least 8 characters', type: 'text' },
    I('setup_timeout_s', 'Start hotspot after (s)', 'If no network connects within this time after boot', { min: 10, max: 600 }),
    B('force_setup', 'Start the hotspot on next boot', 'One-shot: cleared automatically'),
  ] }] },
  { key: 'webserver', title: 'Web server', sub: 'Port', section: 'Connectivity', icon: 'network', color: '#8e8e93', sections: [{ id: 'web', fields: [I('port', 'Port', 'Restart required', { min: 1, max: 65535 })] }] },
  // ---- System
  { key: 'general', title: 'General', sub: 'Grill name, units, auger rate', section: 'System', icon: 'settings-2', color: '#8e8e93', sections: [{ id: 'globals', fields: [
    X('grill_name', 'Grill name', 'Shown in the header and in notifications'),
    S('units', 'Temperature units', 'All temperature settings convert automatically', [['F', 'Fahrenheit'], ['C', 'Celsius']]),
    N('augerrate', 'Auger rate (g/s)', 'Pellets delivered per second of auger run; used for priming and usage estimates', { step: 0.01, min: 0.01 }),
    B('prime_ignition', 'Igniter on while priming', ''),
    B('debug_mode', 'Debug logging', ''),
  ] }] },
  { key: 'appearance', title: 'Appearance', sub: 'Theme, optional features', section: 'System', icon: 'palette', color: '#bf5af2', sections: [{ id: 'globals', fields: [
    S('theme', 'Theme', '', [['dark', 'Dark'], ['light', 'Light'], ['auto', 'Follow system']]),
    B('show_recipes', 'Show recipes', 'Recipe programs on the Cook page'),
  ] }] },
  { key: 'updates', title: 'Software updates', sub: 'Release source and automatic checks', section: 'System', icon: 'refresh-cw', color: '#0a84ff', sections: [{ id: 'update', fields: [
    X('repo', 'GitHub repository', 'owner/name whose releases the updater installs'),
    B('auto_check', 'Check automatically', 'Shortly after boot and then periodically; a notice is logged when a newer release exists'),
    I('check_interval_h', 'Check every (hours)', '', { min: 1, max: 720 }),
    B('include_prerelease', 'Include pre-releases', 'Offer alpha/beta/rc builds as well as final releases'),
  ] }] },
];
// index order: what you cook with, the hardware, the safety net, connectivity, data, the app itself
const SECTIONS = ['Cooking', 'Hardware', 'Safety', 'Connectivity', 'Data', 'System'];
const LINKS = {
  Hardware: [{ href: '#/more/hardware', icon: 'cpu', color: '#64d2ff', title: 'Hardware setup', sub: 'Board, pins, display, hopper sensor' }],
  Connectivity: [{ href: '#/more/network', icon: 'wifi', color: '#0a84ff', title: 'Wi-Fi', sub: 'Networks and connection' }],
  Data: [{ href: '#/history', icon: 'chart-line', color: '#30d158', title: 'Cook files', sub: 'Saved cooks and analysis logs' }, { href: '#/more/learning', icon: 'brain', color: '#bf5af2', title: 'Learning data', sub: 'Feed-forward model, plant estimate, autotune' }],
  System: [{ href: '#/more/system', icon: 'monitor', color: '#8e8e93', title: 'System', sub: 'Health, restart, power' }, { href: '#/more/about', icon: 'info', color: '#8e8e93', title: 'About', sub: '' }],
};

const get = (obj, path) => path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);
const setDeep = (obj, path, v) => { const ks = path.split('.'); let o = obj; for (const k of ks.slice(0, -1)) o = o[k] ??= {}; o[ks.at(-1)] = v; };

export function fieldInput(f, value) {
  const id = 'f_' + f.path.replace(/\W/g, '_') + '_' + Math.random().toString(36).slice(2, 6);
  let input;
  switch (f.type) {
    case 'bool': input = el('input', { type: 'checkbox', id, name: f.path, checked: !!value }); break;
    case 'select': input = el('select', { id, name: f.path }, (f.options || []).map(([v, l]) => el('option', { value: v, selected: String(v) === String(value) }, l))); break;
    case 'password': input = el('input', { type: 'password', id, name: f.path, value: value ?? '', autocomplete: 'off' }); break;
    case 'text': input = el('input', { type: 'text', id, name: f.path, value: value ?? '' }); break;
    default: input = el('input', { type: 'text', inputmode: 'decimal', id, name: f.path, value: value ?? '', pattern: '-?[0-9]*[.,]?[0-9]*' });
  }
  const unit = f.type === 'temp' || f.type === 'tempdelta' ? ` (${degUnit()})` : '';
  if (f.type === 'bool') {
    return el('label', { class: 'toggle', for: id }, el('div', {}, el('div', {}, f.label), f.help ? el('div', { class: 'help muted', style: 'font-size:.76rem' }, f.help) : null), el('span', { class: 'switch' }, input, el('span')));
  }
  return el('div', { class: 'field inline' }, el('div', {}, el('label', { for: id }, f.label + unit), f.help ? el('div', { class: 'help' }, f.help) : null), input);
}

export function readField(f, form) {
  const input = form.querySelector(`[name="${CSS.escape(f.path)}"]`);
  if (!input) return undefined;
  if (f.type === 'bool') return input.checked;
  if (f.type === 'select') return f.bool ? input.value === 'true' : input.value;
  if (f.type === 'text' || f.type === 'password') return input.value;
  const v = parseFloat(String(input.value).replace(',', '.'));
  if (Number.isNaN(v)) throw new Error(`${f.label}: enter a number`);
  if (f.min != null && v < f.min) throw new Error(`${f.label}: minimum is ${f.min}`);
  if (f.max != null && v > f.max) throw new Error(`${f.label}: maximum is ${f.max}`);
  return f.type === 'int' ? Math.round(v) : v;
}

function pageCard(pg) {
  const wrap = el('div');
  for (const sec of pg.sections) {
    const data = PF.settings[sec.id] || {};
    const form = el('form', { onsubmit: async (e) => {
      e.preventDefault();
      const patch = {};
      try { for (const f of sec.fields) { const v = readField(f, form); if (v !== undefined) setDeep(patch, f.path, v); } }
      catch (err) { toast(err.message, true); return; }
      const btn = form.querySelector('button[type=submit]'); btn.disabled = true;
      try { await patchSettings(sec.id, patch); toast('Saved'); if (sec.id === 'globals' && 'units' in patch) location.reload(); } catch (err) { toast(err.message, true); }
      btn.disabled = false;
    } });
    form.append(el('h2', {}, sec.title || pg.title));
    const card = el('div', { class: 'card' });
    for (const f of sec.fields) card.append(fieldInput(f, get(data, f.path)));
    card.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
    form.append(card);
    wrap.append(form);
  }
  return wrap;
}

async function controllerCard() {
  const controllers = await api('/controllers');
  const sel = PF.settings.controller.selected;
  const wrap = el('div');
  const render = (id) => {
    const c = controllers.find((x) => x.id === id) || controllers[0];
    const cfg = PF.settings.controller.config?.[c.id] || {};
    wrap.innerHTML = '';
    const form = el('form', { onsubmit: async (e) => {
      e.preventDefault();
      const patch = { selected: c.id, config: { [c.id]: {} } };
      try {
        for (const o of c.config) {
          const f = { path: o.option_name, label: o.option_friendly_name, type: o.option_type === 'bool' ? 'bool' : 'num' };
          patch.config[c.id][o.option_name] = readField(f, form);
        }
      } catch (err) { toast(err.message, true); return; }
      try { await patchSettings('controller', patch); toast('Controller saved'); } catch (err) { toast(err.message, true); }
    } });
    const card = el('div', { class: 'card' },
      el('div', { class: 'field' }, el('label', {}, 'Controller'), el('select', { onchange: (e) => render(e.target.value) }, controllers.map((x) => el('option', { value: x.id, selected: x.id === c.id }, x.name)))),
      el('p', { class: 'muted', style: 'font-size:.85rem' }, c.description),
      el('p', { class: 'muted', style: 'font-size:.78rem' }, `Recommended cycle: ${c.recommend.cycle_time}s, feed ${c.recommend.u_min}–${c.recommend.u_max}`));
    for (const o of c.config) {
      const f = { path: o.option_name, label: o.option_friendly_name, help: o.option_description, type: o.option_type === 'bool' ? 'bool' : o.units === 'temp_delta' ? 'tempdelta' : 'num' };
      card.append(fieldInput(f, cfg[o.option_name] ?? o.option_default));
    }
    card.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save controller')));
    form.append(el('h2', {}, 'Controller'), card);
    wrap.append(form);
  };
  render(sel);
  return wrap;
}

export function renderSettings(view, rest) {
  if (!PF.settings) { view.append(el('div', { class: 'card muted' }, 'Loading settings…')); return; }
  const dc = !!PF.settings.platform?.dc_fan;
  const pages = PAGES.filter((p) => !p.dc || dc);
  const page = rest?.[0];
  if (page) {
    view.append(el('button', { class: 'btn ghost sm', onclick: () => (location.hash = '#/settings') }, '‹ Settings'));
    const pg = pages.find((x) => x.key === page);
    if (!pg) { view.append(el('div', { class: 'card muted' }, 'No such settings page')); return; }
    if (pg.controller) { controllerCard().then((c) => view.append(c)).catch((e) => toast(e.message, true)); return; }
    if (pg.custom) { Promise.resolve(pg.custom(view)).catch((e) => toast(e.message, true)); return; }
    view.append(pageCard(pg));
    return;
  }
  // index: iOS-style grouped lists, one row per page
  for (const sec of SECTIONS) {
    const rows = pages.filter((p) => p.section === sec).map((p) => ({ href: `#/settings/${p.key}`, icon: p.icon, color: p.color, title: p.title, sub: p.sub }));
    for (const l of LINKS[sec] || []) rows.push(l);
    if (rows.length) view.append(listGroup(sec, rows));
  }
}
