import { PF, el, api, patchSettings, toast, degUnit, confirmDialog, alertSupport, requestAlertPermission, showSystemNotification } from '../app.js';
import { renderProbes } from './probes.js';
import { renderRules } from './rules.js';
import { renderLearning } from './learning.js';
import { renderPellets } from './pellets.js';
import { renderNetwork } from './network.js';
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
  { key: 'controller', title: 'Temperature Control & Learning', sub: 'Controller, what it learns from every cook, local weather', section: 'Cooking', icon: 'gauge', color: '#ff8a1f', custom: controllerPage },
  { key: 'hardware', title: 'Grill Hardware', sub: 'Board, pins, display, hopper sensor', section: 'Hardware', icon: 'cpu', color: '#64d2ff', custom: (v) => import('./more.js').then((m) => m.hardware(v)) },
  { key: 'probes', title: 'Probes', sub: 'Wired and Bluetooth probes, profiles, tuner', section: 'Hardware', icon: 'thermometer', color: '#ff453a', custom: renderProbes },
  { key: 'auger', title: 'Auger & Feed', sub: 'Cycle length, feed limits, P-mode', section: 'Cooking', icon: 'sliders-horizontal', color: '#ff9f0a', sections: [{ id: 'cycle_data', fields: [
    I('HoldCycleTime', 'Control cycle (s)', 'Length of one auger cycle while holding a temperature; the controller decides the feed once per cycle', { min: 5, max: 120 }),
    N('u_min', 'Minimum auger duty', 'Smallest fraction of each cycle the auger runs (0.1 = 10%). Keeps the fire alive at low set points', { step: 0.01, min: 0, max: 1 }),
    N('u_max', 'Maximum auger duty', 'Largest fraction of each cycle the auger runs (0.9 = 90%). Stops the pot from over-filling', { step: 0.01, min: 0, max: 1 }),
    I('SmokeOnCycleTime', 'Smoke: auger on (s)', 'Auger run time per cycle in Smoke and during startup', { min: 1 }),
    I('SmokeOffCycleTime', 'Smoke: auger off (s)', 'Base pause between runs in Smoke; each P-mode level adds 10 s', { min: 1 }),
    I('PMode', 'P-mode', 'Higher = longer pauses = less pellets and more smoke (0–9)', { min: 0, max: 9 }),
    B('FanPidEnabled', 'Modulate AC fan at minimum feed', 'When the auger is already at its minimum duty, pulse the fan to hold temperature (AC fans only)'),
  ] }] },
  { key: 'startup', title: 'Startup & Shutdown', sub: 'Ignition, what happens after startup, cool-down', section: 'Cooking', icon: 'power', color: '#30d158', sections: [
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
  { key: 'fan', title: 'DC Fan', sub: 'PWM speed control', section: 'Hardware', icon: 'fan', color: '#64d2ff', dc: true, sections: [{ id: 'pwm', fields: [
    B('pwm_control', 'Vary fan speed with temperature', 'Default for new cooks; can be changed while cooking'),
    I('frequency', 'PWM frequency (Hz)', '25 000 Hz for 4-wire PC fans', { min: 100, max: 100000 }),
    I('min_duty_cycle', 'Minimum fan speed (%)', 'Some fans stall below this', { min: 0, max: 100 }),
    I('max_duty_cycle', 'Maximum fan speed (%)', '', { min: 10, max: 100 }),
    I('update_time', 'Speed update interval (s)', '', { min: 1 }),
  ] }] },
  { key: 'lid', title: 'Lid-Open Detection', sub: 'Pause the feed when the lid is opened', section: 'Cooking', icon: 'lock-open', color: '#ffd60a', sections: [{ id: 'cycle_data', fields: [
    B('LidOpenDetectEnabled', 'Detect an open lid', 'A sudden temperature drop pauses the auger so the pot does not overfill'),
    I('LidOpenThreshold', 'Drop that counts as open (%)', 'Percentage below the set point', { min: 1, max: 50 }),
    I('LidOpenPauseTime', 'Pause length (s)', '', { min: 10 }),
  ] }] },
  // ---- Safety
  { key: 'safety', title: 'Temperature Limits', sub: 'High-temperature cutoff, flame-out detection', section: 'Safety', icon: 'shield-check', color: '#ff453a', sections: [{ id: 'safety', fields: [
    T('maxtemp', 'High-temperature cutoff', 'Above this in any mode everything shuts off and the grill goes to Error'),
    B('relight_enabled', 'Flame-out protection', 'While holding, a pit that falls away from the set point lights the igniter until the fire catches'),
    { path: 'relight_drop', label: 'Drop that triggers it', help: 'How far below the set point the pit must fall', type: 'tempdelta' },
    { path: 'relight_recover', label: 'Rise that ends it', help: 'After a fire fell away: how far the pit must climb from its lowest point before the igniter goes off', type: 'tempdelta' },
    { path: 'relight_recover_step', label: 'Rise that ends it, coasting down', help: 'After lowering the set point the fire was starved, not lost, so a small turnaround is enough', type: 'tempdelta' },
    I('relight_timeout_s', 'Give up after (s)', 'If the pit has not climbed back by then, it is treated as a flame-out', { min: 60 }),
    B('startup_check', 'Flame-out detection', 'Watch for the pit dropping below the flame-out floor in Smoke and Hold'),
    T('minstartuptemp', 'Flame-out floor (minimum)', 'Lowest floor used after a normal startup'),
    T('maxstartuptemp', 'Flame-out floor (maximum)', ''),
    I('reigniteretries', 'Re-ignite attempts', 'Tries to re-light after a flame-out before going to Error', { min: 0, max: 5 }),
    I('probe_fault_s', 'Pit probe timeout (s)', 'Seconds without a valid pit reading before Error', { min: 3 }),
    I('error_cooldown_fan_s', 'Fan run after an error (s)', 'Cools the pot when the grill errors while hot', { min: 0 }),
  ] }] },
  { key: 'coldstart', title: 'Cold-Weather Start', sub: 'Confirm ignition by temperature rise instead of a fixed floor', section: 'Safety', icon: 'snowflake', color: '#5ac8fa', sections: [{ id: 'safety', fields: [
    B('coldstart.enabled', 'Cold-weather start', 'Keep starting until the pit has risen from its cold baseline; for freezing conditions'),
    { path: 'coldstart.delta_rise', label: 'Rise that confirms ignition', help: 'Above the baseline measured in the first minute', type: 'tempdelta' },
    I('coldstart.timeout_s', 'Give up after (s)', '0 = same as the startup time', { min: 0 }),
    B('coldstart.exit_on_rise', 'End startup once the rise is confirmed', 'Otherwise the full startup time runs'),
  ] }] },
  { key: 'limits', title: 'Output Limits & Manual Control', sub: 'Igniter and auger time caps, manual overrides', section: 'Safety', icon: 'zap', color: '#ff9f0a', sections: [{ id: 'safety', fields: [
    I('igniter_max_on_s', 'Igniter maximum on time (s)', 'The igniter is forced off after this', { min: 60 }),
    I('auger_max_on_s', 'Auger maximum continuous run (s)', 'Absolute cap, regardless of controller or manual control', { min: 5 }),
    B('allow_manual_changes', 'Allow manual outputs while cooking', 'Temporarily override outputs from More → Manual outputs'),
    I('manual_override_time', 'Manual override lasts (s)', '', { min: 5 }),
  ] }] },
  // ---- Cook
  { key: 'keepwarm', title: 'Keep Warm', sub: 'After a probe reaches its target', section: 'Cooking', icon: 'flame', color: '#ff6b35', sections: [{ id: 'keep_warm', fields: [T('temp', 'Keep-warm temperature', ''), B('s_plus', 'Use Smoke+ while keeping warm', '')] }] },
  { key: 'pellets', title: 'Pellets & Hopper', sub: 'Loaded pellets, brands, low-pellet warnings, hopper sensor', section: 'Cooking', icon: 'package', color: '#ac8e68', after: renderPellets, sections: [{ id: 'pelletlevel', title: 'Hopper', fields: [
    B('warning_enabled', 'Low-pellet warnings', ''), I('warning_level', 'Warn below (%)', '', { min: 1, max: 99 }), I('warning_time', 'Repeat every (min)', '', { min: 1 }),
    I('empty', 'Sensor reading when empty (cm)', 'Distance from the sensor to the bottom of the hopper', { min: 1 }), I('full', 'Sensor reading when full (cm)', '', { min: 0 }),
  ] }] },
  { key: 'history', title: 'Data & History', sub: 'Chart sampling and retention', section: 'System', icon: 'database', color: '#5e5ce6', sections: [{ id: 'history', fields: [
    I('sample_s', 'Sample every (s)', '', { min: 1, max: 60 }), I('retention_hours', 'Keep for (hours)', '', { min: 1 }), B('clear_on_startup', 'Clear the chart when a cook starts', ''),
  ] }] },
  // ---- Connectivity
  { key: 'rules', title: 'Conditional Notifications', sub: 'Your own if-this-then-notify rules', section: 'Notifications', icon: 'git-branch', color: '#bf5af2', custom: renderRules },
  { key: 'integrations', title: 'Home Assistant & Webhooks', sub: 'MQTT with Home Assistant discovery, JSON webhook', section: 'Notifications', icon: 'house', color: '#0a84ff', sections: [{ id: 'notify', fields: [
    B('mqtt.enabled', 'MQTT', 'Publish state to a broker, with Home Assistant discovery'), X('mqtt.broker', 'Broker host', ''), I('mqtt.port', 'Broker port', '', { min: 1, max: 65535 }),
    X('mqtt.username', 'Username', ''), { path: 'mqtt.password', label: 'Password', type: 'password' }, X('mqtt.id', 'Device ID', 'Topic prefix'), I('mqtt.update_sec', 'Publish every (s)', '', { min: 5 }),
    B('webhook.enabled', 'Webhook', 'POST events as JSON to a URL'), X('webhook.url', 'Webhook URL', ''),
  ] }] },
  { key: 'push', title: 'Phone Notifications', sub: 'Predictive alerts, Pushover, ntfy', section: 'Notifications', icon: 'bell', color: '#ff453a', sections: [
    { id: 'notify', title: 'Predictive Alerts', fields: [
      { type: 'note', help: 'The grill estimates when each probe will reach its target from how fast it is climbing, and tells you before it gets there so you can be at the grill in time.' },
      I('eta_warn_min', 'Tell me this long before a probe reaches its target (minutes)', '0 = off. Sent once per target, as soon as the live estimate has settled below this', { min: 0, max: 240 }),
    ] },
    { id: 'notify', title: 'Phone & Browser Alerts', fields: [
      { type: 'note', help: 'On an iPhone these only arrive if PiFire has been added to the Home Screen and opened from there, and only while it is running or recently in the background. Once iOS closes it nothing gets through, which is what Pushover below is for.' },
      { type: 'action', label: 'Allow notifications on this device', endpoint: '' , client: 'alerts' },
      { type: 'action', label: 'Show a test notification', endpoint: '', client: 'alerttest' },
    ] },
    { id: 'notify', title: 'Pushover', collapsible: 'pushover.enabled', fields: [
      { type: 'note', help: 'Install the Pushover app ($5 once), then paste your user key from the app and create an application token at pushover.net/apps/build.' },
      B('pushover.enabled', 'Pushover', 'Send notifications to the Pushover app'),
      X('pushover.user_key', 'User key', 'Shown at the top of the Pushover app'), { path: 'pushover.app_token', label: 'Application token', type: 'password' },
      S('pushover.priority', 'Priority', 'For targets, timers and pellets', [[-1, 'Quiet (no sound)'], [0, 'Normal'], [1, 'High (bypasses quiet hours)']]),
      S('pushover.alarm_priority', 'Alarm priority', 'For limit alarms and grill errors', [[0, 'Normal'], [1, 'High'], [2, 'Emergency (repeats until acknowledged)']]),
      X('pushover.sound', 'Sound', 'Blank = your default; e.g. cosmic, bike, siren'),
      B('pushover.targets', 'Targets & timers', 'Target reached, the predictive warning, cook timer, recipe steps'), B('pushover.alarms', 'Alarms & errors', 'Probe limit alarms, flame-out, over-temperature'), B('pushover.pellets', 'Pellets low', ''), B('pushover.tuning', 'Tuning runs', 'Started, finished, or gave up; a run takes hours unattended'), B('pushover.system', 'Other system notices', ''),
      { type: 'action', label: 'Send a test notification', endpoint: '/notify/test/pushover' },
    ] },
    { id: 'notify', title: 'ntfy (Free Alternative)', collapsible: 'ntfy.enabled', fields: [
      { type: 'note', help: 'Install the ntfy app, subscribe to a private topic name, and enter it here. Use ntfy.sh or your own server.' },
      B('ntfy.enabled', 'ntfy', ''), X('ntfy.server', 'Server', 'https://ntfy.sh or your own'), X('ntfy.topic', 'Topic', 'Pick something nobody would guess'), { path: 'ntfy.token', label: 'Access token', help: 'Only for protected topics', type: 'password' },
      B('ntfy.targets', 'Targets & timers', ''), B('ntfy.alarms', 'Alarms & errors', ''), B('ntfy.pellets', 'Pellets low', ''), B('ntfy.tuning', 'Tuning runs', 'Started, finished, or gave up'), B('ntfy.system', 'Other system notices', ''),
      { type: 'action', label: 'Send a test notification', endpoint: '/notify/test/ntfy' },
    ] },
  ] },
  { key: 'network', title: 'Wi-Fi & Hotspot', sub: 'Networks, connection, the setup hotspot', section: 'Network', icon: 'wifi', color: '#0a84ff', custom: networkPage },
  { key: 'remote', title: 'Remote Access', sub: 'Tailscale: reach the grill from anywhere', section: 'Network', icon: 'globe', color: '#30d158', custom: (v) => import('./more.js').then((m) => m.remote(v)) },
  { key: 'webserver', title: 'Web Server', sub: 'Port', section: 'Network', icon: 'network', color: '#8e8e93', sections: [{ id: 'web', fields: [I('port', 'Port', 'Restart required', { min: 1, max: 65535 })] }] },
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
  { key: 'updates', title: 'Software Updates', sub: 'Check and install releases, update source', section: 'System', icon: 'refresh-cw', color: '#0a84ff', before: (v) => import('./more.js').then((m) => m.softwareUpdates(v)), sections: [{ id: 'update', title: 'Update Source', fields: [
    X('repo', 'GitHub repository', 'owner/name whose releases the updater installs'),
    B('auto_check', 'Check automatically', 'Shortly after boot and then periodically; a notice is logged when a newer release exists'),
    I('check_interval_h', 'Check every (hours)', '', { min: 1, max: 720 }),
    B('include_prerelease', 'Include pre-releases', 'Offer alpha/beta/rc builds as well as final releases'),
    B('hot_update', 'Update while cooking', 'Off: the grill must be stopped to install. On: the controller restarts mid-cook and resumes the running mode a few seconds later (fan and auger pause for the restart)'),
  ] }] },
];
// index order: what you cook with, the hardware, the safety net, connectivity, data, the app itself
// Every concern has exactly one home. Settings = what you configure; More = what you do and what you
// look at (manual outputs, events, logs, system health). Sections follow the questions people ask:
// how it cooks, what it is made of, what keeps it safe, how it tells me, how I reach it, the app itself.
const SECTIONS = ['Cooking', 'Hardware', 'Safety', 'Notifications', 'Network', 'System'];
const LINKS = {};

const get = (obj, path) => path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);
const setDeep = (obj, path, v) => { const ks = path.split('.'); let o = obj; for (const k of ks.slice(0, -1)) o = o[k] ??= {}; o[ks.at(-1)] = v; };

export function fieldInput(f, value) {
  if (f.type === 'note') return el('p', { class: 'muted', style: 'font-size:.82rem;margin:2px 0 8px' }, f.help);
  if (f.client === 'alerts') {
    /* Asking the browser for permission has to happen from a tap, so it lives here rather than
       being something the app does on its own. The state line says plainly whether it can work. */
    const state = el('div', { class: 'help' });
    const btn = el('button', { class: 'btn sm', type: 'button' }, 'Allow');
    const refresh = () => {
      const sup = alertSupport();
      state.textContent = sup.ok ? 'Allowed on this device.' : sup.why;
      btn.disabled = sup.ok || (typeof Notification !== 'undefined' && Notification.permission === 'denied');
      btn.textContent = sup.ok ? 'Allowed' : 'Allow';
    };
    btn.onclick = async () => { await requestAlertPermission(); refresh(); };
    refresh();
    return el('div', { class: 'field inline' }, el('div', {}, el('label', {}, f.label), state), btn);
  }
  if (f.client === 'alerttest') {
    const state = el('div', { class: 'help' }, 'Appears on this device only. Leave the app, or lock the phone, before tapping.');
    const btn = el('button', { class: 'btn sm', type: 'button' }, 'Show one');
    btn.onclick = async () => {
      const sup = alertSupport();
      if (!sup.ok) { toast(sup.why, true); return; }
      /* a notification only shows while the page is hidden, which is the case worth testing, so
         give the tester a few seconds to put the app in the background */
      state.textContent = 'In 5 seconds. Put the app in the background now.';
      setTimeout(async () => {
        await showSystemNotification({ title: 'PiFire', body: 'This is what an alert looks like.', code: 'Test_Notify' });
        state.textContent = 'Sent. If nothing appeared, check PiFire in your phone notification settings.';
      }, 5000);
    };
    return el('div', { class: 'field inline' }, el('div', {}, el('label', {}, f.label), state), btn);
  }
  if (f.type === 'action') return el('div', { class: 'field inline' }, el('div', {}, el('label', {}, f.label), f.help ? el('div', { class: 'help' }, f.help) : null),
    el('button', { class: 'btn sm', type: 'button', onclick: async (e) => { const b = e.currentTarget; b.disabled = true; try { await api(f.endpoint, { body: {} }); toast('Sent — check your phone'); } catch (err) { toast(err.message, true); } b.disabled = false; } }, 'Send test'));
  if (f.type === 'weather') {
    const box = el('div', { class: 'kv', style: 'margin-top:6px' });
    const load = async () => { try { const w = await api('/weather'); box.innerHTML = ''; const rows = w.valid ? [['Location', w.place], ['Outdoor', `${PF.units === 'C' ? w.temp_c.toFixed(1) + ' °C' : (w.temp_c * 9 / 5 + 32).toFixed(0) + ' °F'}`], ['Wind', `${w.wind_kmh} km/h (gusts ${w.gust_kmh})`], ['Humidity', `${w.humidity}%`], ['Updated', `${Math.round(w.age_s / 60)} min ago`]] : [['Status', w.error || (w.enabled ? 'waiting for the first fetch…' : 'off')]]; for (const [k, v] of rows) box.append(el('div', {}, k), el('div', {}, v)); } catch { /* ignore */ } };
    load();
    return el('div', {}, box, el('button', { class: 'btn sm ghost', type: 'button', style: 'margin-top:6px', onclick: async () => { try { await api('/weather/refresh', { body: {} }); toast('Refreshing…'); setTimeout(load, 4000); } catch (err) { toast(err.message, true); } } }, 'Refresh now'));
  }
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
  if (!f.path) return undefined;
  const input = form.querySelector(`[name="${CSS.escape(f.path)}"]`);
  if (!input) return undefined;
  if (f.type === 'bool') return input.checked;
  if (f.type === 'select') return f.bool ? input.value === 'true' : (f.options?.every(([v]) => typeof v === 'number') ? Number(input.value) : input.value);
  if (f.type === 'text' || f.type === 'password') return input.value;
  const v = parseFloat(String(input.value).replace(',', '.'));
  if (Number.isNaN(v)) throw new Error(`${f.label}: enter a number`);
  if (f.min != null && v < f.min) throw new Error(`${f.label}: minimum is ${f.min}`);
  if (f.max != null && v > f.max) throw new Error(`${f.label}: maximum is ${f.max}`);
  return f.type === 'int' ? Math.round(v) : v;
}

function pageCard(pg) {
  const wrap = el('div');
  const rerender = () => { wrap.innerHTML = ''; build(); };
  const build = () => {
  for (const sec of pg.sections) {
    const data = PF.settings[sec.id] || {};
    const form = el('form', { onsubmit: async (e) => {
      e.preventDefault();
      const patch = {};
      try { for (const f of sec.fields) { const v = readField(f, form); if (v !== undefined) setDeep(patch, f.path, v); } }
      catch (err) { toast(err.message, true); return; }
      const btn = form.querySelector('button[type=submit]');
      if (btn) btn.disabled = true;
      try {
        await patchSettings(sec.id, patch);
        toast('Saved');
        if (sec.id === 'globals' && 'units' in patch) location.reload();
        else if (sec.id === 'weather') api('/weather/refresh', { body: {} }).catch(() => {});
        /* Redraw from what came back. The page was built from a snapshot of the settings, so
           anything that reads the saved value -- a section that shows On or Off, a field the
           daemon tidied on the way in -- otherwise keeps showing what was there before the save. */
        rerender();
      } catch (err) { toast(err.message, true); }
      if (btn) btn.disabled = false;
    } });
    const card = el('div', { class: 'card' });
    for (const f of sec.fields) card.append(fieldInput(f, f.path ? get(data, f.path) : undefined));
    /* a section of notes and device-side buttons has nothing to store, so it has nothing to save */
    if (sec.fields.some((f) => f.path)) card.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save')));
    if (sec.collapsible) {
      // open when the service is already switched on, so a configured sink stays visible
      const on = !!get(data, sec.collapsible);
      form.append(el('details', { class: 'fold', open: on },
        el('summary', {}, el('span', {}, sec.title || pg.title), el('span', { class: `fold-state ${on ? 'on' : ''}` }, on ? 'On' : 'Off')), card));
    } else {
      if (sec.title !== '') form.append(el('h2', {}, sec.title || pg.title));
      form.append(card);
    }
    wrap.append(form);
  }
  };
  build();
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

const learningFields = [
  B('enabled', 'Learn from cooks', 'Record the steady feed for each set point and ambient temperature, and the plant model from every startup'),
  B('auto_tune', 'Apply learned tuning automatically', 'Hand the measured plant model (and autotune results) to the controller as soon as they are known'),
  I('half_life_obs', 'Memory half-life (observations)', 'How quickly old cooks fade; ~12 observations per hour of Hold', { min: 5, max: 500 }),
];
const weatherFields = [
  { type: 'note', help: 'The feed-forward and the learning use the outdoor temperature as the ambient reference. With a postal code the grill fetches local conditions (Open-Meteo, no account) every 15 minutes and records wind and humidity with each cook. An ambient-flagged probe still takes precedence.' },
  B('enabled', 'Use local weather', ''), X('country', 'Country code', 'Two letters, e.g. us, ca, de'), X('postal_code', 'Postal / ZIP code', ''),
  { type: 'weather' },
];
// Temperature control & learning: the controller, what it learns, the weather it learns against, and the learned data
async function controllerPage(view) {
  view.append(await controllerCard());
  view.append(pageCard({ title: 'Learning', sections: [{ id: 'learning', title: 'Learning', fields: learningFields }, { id: 'weather', title: 'Local Weather', fields: weatherFields }] }));
  return renderLearning(view);
}
// Wi-Fi & hotspot: live connection and networks, then the hotspot settings
function networkPage(view) {
  // the hotspot's live state, its settings and its start/stop button belong together
  const hotspotExtra = pageCard({ sections: [{ id: 'network', title: '', fields: [
    X('hotspot_ssid', 'Hotspot name', 'Blank = PiFire-XXXX from the Wi-Fi address'),
    { path: 'hotspot_password', label: 'Hotspot password', help: 'At least 8 characters', type: 'text' },
    I('setup_timeout_s', 'Start hotspot after (s)', 'If no network connects within this time after boot', { min: 10, max: 600 }),
    B('force_setup', 'Start the hotspot on next boot', 'One-shot: cleared automatically'),
  ] }] });
  return renderNetwork(view, { hotspotExtra });
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
    if (pg.custom) return Promise.resolve(pg.custom(view)).catch((e) => { toast(e.message, true); });
    // pages made of settings fields, optionally with live content before (updates) or after (pellets)
    const parts = [];
    if (pg.before) { const slot = el('div'); view.append(slot); parts.push(Promise.resolve(pg.before(slot))); }
    view.append(pageCard(pg));
    if (pg.after) parts.push(Promise.resolve(pg.after(view)));
    return Promise.all(parts).then((ts) => () => ts.forEach((t) => typeof t === 'function' && t())).catch((e) => { toast(e.message, true); });
  }
  // index: iOS-style grouped lists, one row per page
  for (const sec of SECTIONS) {
    const rows = pages.filter((p) => p.section === sec).map((p) => ({ href: `#/settings/${p.key}`, icon: p.icon, color: p.color, title: p.title, sub: p.sub }));
    for (const l of LINKS[sec] || []) rows.push(l);
    if (rows.length) view.append(listGroup(sec, rows));
  }
}
