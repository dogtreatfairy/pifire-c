import { PF, el, api, patchSettings, toast, degUnit, confirmDialog } from '../app.js';

// Field descriptors: path relative to the group, type: num|int|bool|select|text|temp|tempdelta
const T = (path, label, help, extra = {}) => ({ path, label, help, type: 'temp', ...extra });
const N = (path, label, help, extra = {}) => ({ path, label, help, type: 'num', ...extra });
const I = (path, label, help, extra = {}) => ({ path, label, help, type: 'int', ...extra });
const B = (path, label, help) => ({ path, label, help, type: 'bool' });
const S = (path, label, help, options) => ({ path, label, help, type: 'select', options });
const X = (path, label, help) => ({ path, label, help, type: 'text' });

const GROUPS = [
  { id: 'globals', title: 'General', fields: [
    X('grill_name', 'Grill name', 'Shown in the header and in notifications'),
    S('units', 'Temperature units', 'All temperature settings convert automatically', [['F', 'Fahrenheit'], ['C', 'Celsius']]),
    S('theme', 'Theme', '', [['dark', 'Dark'], ['light', 'Light'], ['auto', 'Follow system']]),
    N('augerrate', 'Auger rate (g/s)', 'Pellets delivered per second of auger run; used for priming and usage estimates', { step: 0.01, min: 0.01 }),
    B('prime_ignition', 'Igniter during prime', 'Turn the igniter on while priming before a startup'),
    B('debug_mode', 'Debug logging', ''),
  ] },
  { id: 'cycle_data', title: 'Cycle', fields: [
    I('HoldCycleTime', 'Hold cycle time (s)', 'Length of one auger cycle in Hold. The controller runs once per cycle.', { min: 5, max: 120 }),
    N('u_min', 'Minimum feed ratio', 'Auger on-fraction floor per cycle, prevents flame-out (0.1 = 10%)', { step: 0.01, min: 0, max: 1 }),
    N('u_max', 'Maximum feed ratio', 'Auger on-fraction ceiling per cycle so the pot can keep up', { step: 0.01, min: 0, max: 1 }),
    I('SmokeOnCycleTime', 'Smoke auger on (s)', 'Auger on time per cycle in Smoke and Startup', { min: 1 }),
    I('SmokeOffCycleTime', 'Smoke auger off (s)', 'Base auger off time in Smoke and Startup; P-mode adds 10 s per level', { min: 1 }),
    I('PMode', 'P-mode', 'Adds 10 s of auger-off time per level in Smoke', { min: 0, max: 9 }),
    B('LidOpenDetectEnabled', 'Lid-open detection', 'Pause the feed when the pit temperature drops sharply'),
    I('LidOpenThreshold', 'Lid-open threshold (%)', 'Drop below this percentage of the set point triggers a pause', { min: 1, max: 50 }),
    I('LidOpenPauseTime', 'Lid-open pause (s)', '', { min: 10 }),
    B('FanPidEnabled', 'Fan PID (AC fans)', 'Modulate the fan when the feed is already at its minimum'),
  ] },
  { id: 'safety', title: 'Safety', fields: [
    T('maxtemp', 'Maximum pit temperature', 'Above this in any mode the grill goes to Error and shuts off'),
    T('minstartuptemp', 'Minimum startup floor', 'Lowest flame-out floor when cold-start is off'),
    T('maxstartuptemp', 'Maximum startup floor', ''),
    I('reigniteretries', 'Re-ignite retries', 'Attempts to re-light after a suspected flame-out before Error', { min: 0, max: 5 }),
    B('startup_check', 'Flame-out detection', 'Watch for the pit dropping below the startup floor in Smoke/Hold'),
    B('coldstart.enabled', 'Cold-start mode', 'For freezing weather: keep starting until the pit has risen from its baseline instead of a fixed floor'),
    { path: 'coldstart.delta_rise', label: 'Cold-start rise', help: 'Temperature rise above the startup baseline that confirms ignition', type: 'tempdelta' },
    I('coldstart.timeout_s', 'Cold-start timeout (s)', '0 = same as startup duration', { min: 0 }),
    B('coldstart.exit_on_rise', 'End startup once the rise is confirmed', 'Leave startup early when cold-start sees the pit rising (and above the minimum startup temperature) instead of running the full timer'),
    I('igniter_max_on_s', 'Igniter max on (s)', 'Igniter is forced off after this long', { min: 60 }),
    I('auger_max_on_s', 'Auger max continuous on (s)', 'Absolute cap regardless of controller or manual control', { min: 5 }),
    I('probe_fault_s', 'Probe fault timeout (s)', 'Seconds without a valid pit reading before Error', { min: 3 }),
    I('error_cooldown_fan_s', 'Error cooldown fan (s)', 'Fan run time after an error when the pit is hot', { min: 0 }),
    B('allow_manual_changes', 'Allow manual output changes while cooking', 'Temporarily override outputs from the Manual page'),
    I('manual_override_time', 'Manual override time (s)', '', { min: 5 }),
  ] },
  { id: 'startup', title: 'Startup', fields: [
    I('duration', 'Startup duration (s)', 'Igniter and startup feed run for this long', { min: 60, max: 900 }),
    T('startup_exit_temp', 'Startup exit temperature', 'Leave startup early when the pit reaches this (0 = disabled)', { allowZero: true }),
    S('start_to_mode.after_startup_mode', 'After startup', '', [['Smoke', 'Smoke'], ['Hold', 'Hold']]),
    T('start_to_mode.primary_setpoint', 'Default hold temperature', ''),
    I('prime_on_startup', 'Prime on startup (g)', '0 = disabled', { min: 0 }),
    I('pwm_duty_cycle', 'Startup fan (%)', 'DC fan speed during startup', { min: 10, max: 100 }),
    B('smartstart.enabled', 'Smart Start', 'Pick a startup profile from the initial pit temperature'),
    T('smartstart.exit_temp', 'Smart Start exit temperature', ''),
  ] },
  { id: 'shutdown', title: 'Shutdown', fields: [
    I('shutdown_duration', 'Shutdown fan time (s)', '', { min: 30 }),
    B('auto_power_off', 'Power off the Pi after shutdown', ''),
  ] },
  { id: 'smoke_plus', title: 'Smoke+', fields: [
    B('enabled', 'Enable by default', ''),
    T('min_temp', 'Minimum temperature', 'Below this the fan stays on'),
    T('max_temp', 'Maximum temperature', 'Above this the fan stays on'),
    I('on_time', 'Fan on (s)', '', { min: 1 }), I('off_time', 'Fan off (s)', '', { min: 1 }),
    I('duty_cycle', 'Ramp target (%)', 'DC fans only', { min: 10, max: 100 }), B('fan_ramp', 'Ramp fan speed', 'DC fans only'),
  ] },
  { id: 'pwm', title: 'DC fan (PWM)', dc: true, fields: [
    B('pwm_control', 'Temperature-based fan speed by default', ''),
    I('frequency', 'PWM frequency (Hz)', '25 kHz for 4-wire PC fans', { min: 100, max: 100000 }),
    I('min_duty_cycle', 'Minimum fan (%)', 'Some fans stall below this', { min: 0, max: 100 }),
    I('max_duty_cycle', 'Maximum fan (%)', '', { min: 10, max: 100 }),
    I('update_time', 'Update interval (s)', '', { min: 1 }),
  ] },
  { id: 'keep_warm', title: 'Keep warm', fields: [T('temp', 'Keep-warm temperature', ''), B('s_plus', 'Smoke+ while keeping warm', '')] },
  { id: 'pelletlevel', title: 'Pellets', fields: [
    B('warning_enabled', 'Low pellet warnings', ''), I('warning_level', 'Warn below (%)', '', { min: 1, max: 99 }),
    I('warning_time', 'Warning interval (min)', '', { min: 1 }), I('empty', 'Empty distance (cm)', 'Sensor reading when the hopper is empty', { min: 1 }), I('full', 'Full distance (cm)', '', { min: 0 }),
  ] },
  { id: 'notify', title: 'Notifications', fields: [
    B('mqtt.enabled', 'MQTT', 'Publish state to a broker with Home Assistant discovery'), X('mqtt.broker', 'Broker host', ''), I('mqtt.port', 'Broker port', '', { min: 1, max: 65535 }),
    X('mqtt.username', 'Username', ''), { path: 'mqtt.password', label: 'Password', type: 'password' }, X('mqtt.id', 'Device ID', 'Topic prefix'), I('mqtt.update_sec', 'Publish interval (s)', '', { min: 5 }),
    B('webhook.enabled', 'Webhook', 'POST events as JSON to a URL'), X('webhook.url', 'Webhook URL', ''),
  ] },
  { id: 'network', title: 'Network', fields: [
    X('hotspot_ssid', 'Hotspot name', 'Blank = PiFire-XXXX from the Wi-Fi address'),
    { path: 'hotspot_password', label: 'Hotspot password', help: 'At least 8 characters', type: 'text' },
    I('setup_timeout_s', 'Boot wait before hotspot (s)', 'Start the setup hotspot if no network connects within this time', { min: 10, max: 600 }),
    B('force_setup', 'Start hotspot on next boot', 'One-shot: cleared automatically'),
  ] },
  { id: 'update', title: 'Software updates', fields: [
    X('repo', 'GitHub repository', 'owner/name whose Releases the updater checks (assets pifire-<ver>-<arch>.tar.gz + SHA256SUMS)'),
    B('auto_check', 'Check automatically', 'Two minutes after boot and then periodically; a notice is logged when a newer release exists'),
    I('check_interval_h', 'Check every (hours)', '', { min: 1, max: 720 }),
  ] },
  { id: 'history', title: 'History', fields: [
    I('sample_s', 'Sample interval (s)', '', { min: 1, max: 60 }), I('retention_hours', 'Keep history (hours)', '', { min: 1 }), B('clear_on_startup', 'Clear history on startup', ''),
  ] },
  { id: 'web', title: 'Web server', fields: [I('port', 'Port', 'Restart required', { min: 1, max: 65535 })] },
];

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
  if (f.type === 'select' || f.type === 'text' || f.type === 'password') return input.value;
  const v = parseFloat(String(input.value).replace(',', '.'));
  if (Number.isNaN(v)) throw new Error(`${f.label}: enter a number`);
  if (f.min != null && v < f.min) throw new Error(`${f.label}: minimum is ${f.min}`);
  if (f.max != null && v > f.max) throw new Error(`${f.label}: maximum is ${f.max}`);
  return f.type === 'int' ? Math.round(v) : v;
}

function groupCard(g) {
  const data = PF.settings[g.id] || {};
  const form = el('form', { onsubmit: async (e) => {
    e.preventDefault();
    const patch = {};
    try { for (const f of g.fields) { const v = readField(f, form); if (v !== undefined) setDeep(patch, f.path, v); } }
    catch (err) { toast(err.message, true); return; }
    const btn = form.querySelector('button[type=submit]'); btn.disabled = true;
    try { await patchSettings(g.id, patch); toast('Saved'); if (g.id === 'globals') location.reload(); } catch (err) { toast(err.message, true); }
    btn.disabled = false;
  } });
  form.append(el('h2', {}, g.title));
  const card = el('div', { class: 'card' });
  for (const f of g.fields) card.append(fieldInput(f, get(data, f.path)));
  card.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save changes')));
  form.append(card);
  return form;
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
  controllerCard().then((c) => view.prepend(c)).catch(() => {});
  for (const g of GROUPS) if (!g.dc || dc) view.append(groupCard(g));
}
