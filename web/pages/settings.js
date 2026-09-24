import { PF, el, api, patchSettings, toast, degUnit, confirmDialog, setBack, alertSupport, requestAlertPermission, showSystemNotification, ensurePushSubscription, onStatus } from '../app.js';
import { renderProbes } from './probes.js';
import { renderRules } from './rules.js';
import { icon as lucide, brandIcon } from '../icons.js';
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
  /* Named for the mode it governs. Everything here decides how the grill holds a temperature: the
     controller, its tuning, and the cycle the auger feeds on. None of it touches Smoke. */
  { key: 'controller', title: 'Hold Mode', sub: 'The controller that holds a temperature, its tuning, and the feed cycle', section: 'Cooking', icon: 'gauge', color: '#ff8a1f', custom: controllerPage },
  { key: 'hardware', title: 'Grill Hardware', sub: 'Board, pins, display, hopper sensor', section: 'Hardware', icon: 'cpu', color: '#64d2ff', custom: (v) => import('./more.js').then((m) => m.hardware(v)) },
  { key: 'probes', title: 'Probes', sub: 'Wired and Bluetooth probes, profiles, tuner', section: 'Hardware', icon: 'thermometer', color: '#ff453a', custom: renderProbes },
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
  /* Smoke is driven by the P-mode and a fixed auger cycle, not by the controller, so its timings
     live here rather than beside settings that only affect Hold. */
  { key: 'smoke', title: 'Smoke Mode', sub: 'P-mode, the auger cycle it feeds on, and Smoke+ fan cycling', section: 'Cooking', icon: 'cloud', color: '#8e8e93', sections: [
    { id: 'cycle_data', title: 'Auger Cycle', fields: [
      I('PMode', 'P-mode', 'Higher = longer pauses = fewer pellets and more smoke (0–9)', { min: 0, max: 9 }),
      I('SmokeOnCycleTime', 'Auger on (s)', 'Auger run time per cycle in Smoke and during startup', { min: 1 }),
      I('SmokeOffCycleTime', 'Auger off (s)', 'Base pause between runs; each P-mode level adds 10 s', { min: 1 }),
    ] },
    { id: 'smoke_plus', title: 'Smoke+', fields: [
    S('enabled', 'Default smoke mode', 'Which mode Smoke starts in; switch any time from the Home screen', [['false', 'Smoke'], ['true', 'Smoke+']], true),
    T('min_temp', 'Smoke+ works above', 'Below this the fan stays on continuously'),
    T('max_temp', 'Smoke+ works below', 'Above this the fan stays on continuously'),
    I('on_time', 'Fan on (s)', '', { min: 1 }), I('off_time', 'Fan off (s)', '', { min: 1 }),
    B('fan_ramp', 'Ramp fan speed', 'DC fan only: ramp up instead of switching'), I('duty_cycle', 'Ramp target speed (%)', 'DC fan only', { min: 10, max: 100 }),
    ] },
  ] },
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
  { key: 'pellets', title: 'Pellets & Hopper', sub: 'Loaded pellets, brands, low-pellet warnings, hopper sensor', section: 'Cooking', icon: 'package', color: '#ac8e68', custom: pelletsPage },
  { key: 'history', title: 'Data & History', sub: 'Chart sampling and retention', section: 'System', icon: 'database', color: '#5e5ce6', sections: [{ id: 'history', fields: [
    I('sample_s', 'Sample every (s)', '', { min: 1, max: 60 }), I('retention_hours', 'Keep for (hours)', '', { min: 1 }), B('clear_on_startup', 'Clear the chart when a cook starts', ''),
  ] }] },
  // ---- Connectivity
  /* What to say, and when. The services below decide where it goes. */
  { key: 'rules', title: 'Conditional Notifications', sub: 'What the grill tells you, and when', section: 'Notifications', icon: 'git-branch', color: '#bf5af2', custom: conditionalPage },
  /* Where a notification goes. Each service is its own dropdown saying whether it is set up, so
     the page is a short list of names rather than every field of every service at once. This is
     not only a phone: a browser on a laptop subscribes the same way, and MQTT and a webhook go
     nowhere near a phone at all. */
  { key: 'services', title: 'Notification Services', sub: 'Where notifications are delivered', section: 'Notifications', icon: 'bell', color: '#ff453a', sections: [
    { id: 'notify', title: 'This Device', sub: 'Browser notifications on the device you are holding', icon: 'smartphone', color: '#0a84ff', collapsible: 'webpush.enabled', state: browserState, fields: [
      { type: 'note', help: 'This browser, on this device. Allow notifications and subscribe, and the grill can reach it through the browser maker\u2019s push service even with PiFire closed. On an iPhone, add PiFire to your Home Screen and open it from there first. Each device subscribes separately.' },
      { type: 'pushstate' },
      { type: 'action', label: 'Allow notifications on this device', endpoint: '', client: 'alerts' },
      { type: 'action', label: 'Show a test notification', endpoint: '', client: 'alerttest' },
      /* The one above asks the browser to draw a notification locally, which proves the permission
         and nothing else. This one goes out through the push service and back to the device, which
         is the path that matters and the path that was silently failing. */
      { type: 'action', label: 'Send a push to this device', endpoint: '/notify/test/webpush' },
      /* Apple refuses a push whose sender gives no valid contact -- 403, every time, silently --
         and it is the one push service that checks. Blank uses the project's address. */
      X('webpush.contact', 'Contact for the push service', 'A mailto: or https: address, as the push standard requires. Apple rejects notifications without a valid one. Blank uses the PiFire project address'),
      B('webpush.targets', 'Targets & timers', ''), B('webpush.alarms', 'Alarms & errors', ''), B('webpush.pellets', 'Pellets low', ''), B('webpush.tuning', 'Tuning runs', ''), B('webpush.system', 'Other system notices', ''),
    ] },
    { id: 'notify', title: 'Pushover', sub: 'Push to your phone, with priorities and sounds', brand: 'pushover', collapsible: 'pushover.enabled', fields: [
      { type: 'note', help: 'Install the Pushover app ($5 once), then paste your user key from the app and create an application token at pushover.net/apps/build.' },
      B('pushover.enabled', 'Pushover', 'Send notifications to the Pushover app'),
      X('pushover.user_key', 'User key', 'Shown at the top of the Pushover app'), { path: 'pushover.app_token', label: 'Application token', type: 'password' },
      S('pushover.priority', 'Priority', 'For targets, timers and pellets', [[-1, 'Quiet (no sound)'], [0, 'Normal'], [1, 'High (bypasses quiet hours)']]),
      S('pushover.alarm_priority', 'Alarm priority', 'For limit alarms and grill errors', [[0, 'Normal'], [1, 'High'], [2, 'Emergency (repeats until acknowledged)']]),
      X('pushover.sound', 'Sound', 'Blank = your default; e.g. cosmic, bike, siren'),
      B('pushover.targets', 'Targets & timers', 'Target reached, the predictive warning, cook timer, recipe steps'), B('pushover.alarms', 'Alarms & errors', 'Probe limit alarms, flame-out, over-temperature'), B('pushover.pellets', 'Pellets low', ''), B('pushover.tuning', 'Tuning runs', 'Started, finished, or gave up; a run takes hours unattended'), B('pushover.system', 'Other system notices', ''),
      { type: 'action', label: 'Send a test notification', endpoint: '/notify/test/pushover' },
    ] },
    { id: 'notify', title: 'ntfy', sub: 'Free push through a topic you choose', brand: 'ntfy', collapsible: 'ntfy.enabled', fields: [
      { type: 'note', help: 'Free. Install the ntfy app, subscribe to a private topic name, and enter it here. Use ntfy.sh or your own server.' },
      B('ntfy.enabled', 'ntfy', ''), X('ntfy.server', 'Server', 'https://ntfy.sh or your own'), X('ntfy.topic', 'Topic', 'Pick something nobody would guess'), { path: 'ntfy.token', label: 'Access token', help: 'Only for protected topics', type: 'password' },
      B('ntfy.targets', 'Targets & timers', ''), B('ntfy.alarms', 'Alarms & errors', ''), B('ntfy.pellets', 'Pellets low', ''), B('ntfy.tuning', 'Tuning runs', 'Started, finished, or gave up'), B('ntfy.system', 'Other system notices', ''),
      { type: 'action', label: 'Send a test notification', endpoint: '/notify/test/ntfy' },
    ] },
    { id: 'notify', title: 'Home Assistant', sub: 'MQTT with discovery: the grill appears as entities', brand: 'homeassistant', collapsible: 'mqtt.enabled', fields: [
      { type: 'note', help: 'Publishes the grill\u2019s state to an MQTT broker with Home Assistant discovery, so the grill and every probe appear as entities without configuring them by hand.' },
      B('mqtt.enabled', 'MQTT', 'Publish state to a broker, with Home Assistant discovery'),
      X('mqtt.broker', 'Broker host', ''), I('mqtt.port', 'Broker port', '', { min: 1, max: 65535 }),
      X('mqtt.username', 'Username', ''), { path: 'mqtt.password', label: 'Password', type: 'password' },
      X('mqtt.id', 'Device ID', 'Topic prefix'), I('mqtt.update_sec', 'Publish every (s)', '', { min: 5 }),
    ] },
    { id: 'notify', title: 'Webhook', sub: 'POST every event as JSON to a URL', icon: 'webhook', color: '#8e8e93', collapsible: 'webhook.enabled', fields: [
      { type: 'note', help: 'POSTs every event as JSON to a URL of your choosing \u2014 for anything that is not one of the services above.' },
      B('webhook.enabled', 'Webhook', 'POST events as JSON to a URL'), X('webhook.url', 'Webhook URL', ''),
    ] },
  ] },
  { key: 'network', title: 'Wi-Fi & Hotspot', sub: 'Networks, connection, the setup hotspot', section: 'Network', icon: 'wifi', color: '#0a84ff', custom: networkPage },
  { key: 'remote', title: 'Tailscale', sub: 'Reach the grill from anywhere', section: 'Network', brand: 'tailscale', custom: (v) => import('./more.js').then((m) => m.remote(v)) },
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
/* The order a cook actually happens in. */
const ORDER = {
  Cooking: ['startup', 'controller', 'smoke', 'lid', 'keepwarm', 'pellets'],
  /* what the grill says, then where it goes */
  Notifications: ['rules', 'services'],
};
const LINKS = {};

/* Being able to reach this browser is not a setting: the browser grants permission and then holds
   a subscription, and either can be withdrawn without PiFire being told. So the summary reports
   what is true right now rather than what was last saved. */
async function browserState() {
  try {
    const info = await api('/push');
    if (!info.available) return { on: false, label: 'Unavailable' };
    const reg = await navigator.serviceWorker?.getRegistration();
    const sub = await reg?.pushManager?.getSubscription();
    if (sub) return { on: true, label: 'Subscribed' };
    if (typeof Notification !== 'undefined' && Notification.permission === 'granted') return { on: false, label: 'Not subscribed' };
    return { on: false, label: 'Not allowed' };
  } catch { return { on: false, label: 'Unknown' }; }
}

/* Conditional notifications: the rules, and the one built-in rule that is not written as one --
   the predictive warning, which needs the grill's own estimate of when a probe will arrive. It
   belongs with what the grill says rather than with where it is sent. */
async function conditionalPage(view) {
  view.append(pageCard({ title: '', sections: [{
    id: 'notify', title: 'Predictive Alerts', sub: 'A warning before a probe reaches its target',
    icon: 'timer', color: '#bf5af2', collapsible: 'eta_warn_min',
    summary: (d) => { const v = Number(d?.eta_warn_min || 0); return { on: v > 0, label: v > 0 ? `${v} min ahead` : 'Off' }; },
    fields: [
      { type: 'note', help: 'The grill estimates when each probe will reach its target from how fast it is climbing, and tells you before it gets there so you can be at the grill in time.' },
      I('eta_warn_min', 'Tell me this long before a probe reaches its target (minutes)', '0 = off. Sent once per target, as soon as the live estimate has settled below this', { min: 0, max: 240 }),
    ] }] }));
  return renderRules(view);
}

const get = (obj, path) => path.split('.').reduce((o, k) => (o == null ? undefined : o[k]), obj);
const setDeep = (obj, path, v) => { const ks = path.split('.'); let o = obj; for (const k of ks.slice(0, -1)) o = o[k] ??= {}; o[ks.at(-1)] = v; };

export function fieldInput(f, value) {
  if (f.type === 'note') return el('p', { class: 'muted', style: 'font-size:.82rem;margin:2px 0 8px' }, f.help);
  if (f.type === 'pushstate') {
    /* Whether this device is actually reachable with the app closed, which is the only question
       that matters and the one the permission prompt does not answer. */
    const state = el('div', { class: 'help' }, 'Checking…');
    const btn = el('button', { class: 'btn sm', type: 'button' }, 'Subscribe');
    const refresh = async () => {
      try {
        const info = await api('/push');
        if (!info.available) { state.textContent = 'This build of PiFire cannot do web push.'; btn.disabled = true; return; }
        const reg = await navigator.serviceWorker?.getRegistration();
        const sub = await reg?.pushManager?.getSubscription();
        state.textContent = sub
          ? `This device is subscribed. The grill can reach it with the app closed. ${info.devices} device${info.devices === 1 ? '' : 's'} subscribed in total.`
          : 'Not subscribed yet. Allow notifications above, then subscribe.';
        btn.textContent = sub ? 'Re-subscribe' : 'Subscribe';
      } catch { state.textContent = 'Could not check with the grill.'; }
    };
    btn.onclick = async () => {
      const sup = alertSupport();
      if (!sup.ok) { toast(sup.why, true); return; }
      const sub = await ensurePushSubscription();
      toast(sub ? 'This device is subscribed' : 'Could not subscribe', !sub);
      refresh();
    };
    refresh();
    return el('div', { class: 'field inline' }, el('div', {}, el('label', {}, 'Reach this device with the app closed'), state), btn);
  }
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
  /* Also hung on the element below, so a page whose settings are changed by something other than
     this form -- the hopper measuring itself, say -- can redraw the fields from what was stored. */
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
      /* Built from the same parts as a settings row -- icon tile, title, one line saying what it
         is, then whether it is on -- so a service you can open looks like the rows above it rather
         than a bordered box bolted on. Every one starts closed, including the ones already
         switched on: the page is a list of services, and a configured service opening itself only
         buries the next one. What it is set to is on its own summary line, so opening it is never
         how you find out. */
      /* Off, On, or whatever the section itself would rather say -- "20 min" answers the question
         better than "On" does, and answering it on the summary line is the point of the fold. */
      const sum = sec.summary ? sec.summary(data) : null;
      const on = sum ? sum.on : !!get(data, sec.collapsible);
      const state = el('span', { class: `fold-state ${on ? 'on' : ''}` }, sum ? sum.label : on ? 'On' : 'Off');
      const det = el('details', { class: 'fold ios-fold' },
        el('summary', {},
          sec.brand ? el('span', { class: 'tile brand' }, brandIcon(sec.brand))
                    : sec.icon ? el('span', { class: 'tile', style: sec.color ? `--tile:${sec.color}` : '' }, lucide(sec.icon)) : null,
          el('span', { class: 'body' }, el('span', { class: 't' }, sec.title || pg.title), sec.sub ? el('span', { class: 's' }, sec.sub) : null),
          state, lucide('chevron-right', 'ic chev')),
        el('div', { class: 'fold-body' }, card));
      /* Some services are not a switch. Being able to reach this browser is not something you turn
         on in settings -- the browser grants it and then holds a subscription -- so the summary says
         what is actually true rather than pretending there is a toggle behind it. */
      if (sec.state) Promise.resolve(sec.state()).then((r) => {
        if (!r) return;
        state.textContent = r.label;
        state.className = `fold-state ${r.on ? 'on' : ''}`;
      }).catch(() => {});
      form.append(det);
    } else {
      if (sec.title !== '') form.append(el('h2', {}, sec.title || pg.title));
      form.append(card);
    }
    wrap.append(form);
  }
  };
  build();
  wrap.rerender = rerender;
  return wrap;
}

/* `tuned` is true when autotune has measured this grill and its numbers are the ones in force.
   The boxes below are then not what the grill is running, and showing them invites someone to
   change a number that changes nothing. They are put away behind the one action that brings them
   back: clearing the measurement. */
async function controllerCard(tuned, onClear) {
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
    if (tuned) {
      card.append(
        el('p', { class: 'muted', style: 'font-size:.82rem' },
          'Autotune has measured this grill, and what it measured is what the controller runs. The starting values are not in use, so they are put away; clear the measurement to type your own again.'),
        el('div', { class: 'form-actions' }, el('button', { class: 'btn sm ghost', type: 'button', onclick: onClear }, 'Clear Autotune')));
    } else {
      for (const o of c.config) {
        const f = { path: o.option_name, label: o.option_friendly_name, help: o.option_description, type: o.option_type === 'bool' ? 'bool' : o.units === 'temp_delta' ? 'tempdelta' : 'num' };
        card.append(fieldInput(f, cfg[o.option_name] ?? o.option_default));
      }
      card.append(el('div', { class: 'form-actions' }, el('button', { class: 'btn primary', type: 'submit' }, 'Save controller')));
    }
    form.append(el('h2', {}, 'Controller'), card);
    wrap.append(form);
  };
  render(sel);
  return wrap;
}

/* A collapsed section that still answers its own question from the summary line: you should be
   able to read what the controller is set to without opening anything. */
/* Built from the same parts as a settings row -- icon tile, title, value, chevron -- so a section
   you can open looks like the rows you tap, rather than a bordered box sitting on top of them. */
const fold = (title, meta, body, icon, color, open = false) =>
  el('details', { class: 'fold ios-fold', open },
    el('summary', {},
      icon ? el('span', { class: 'tile', style: color ? `--tile:${color}` : '' }, lucide(icon)) : null,
      el('span', { class: 'body' }, el('span', { class: 't' }, title), meta ? el('span', { class: 's' }, meta) : null),
      lucide('chevron-right', 'ic chev')),
    el('div', { class: 'fold-body' }, body));

const holdCycleFields = [
  I('HoldCycleTime', 'Control cycle (s)', 'One auger cycle while holding. The controller decides the feed once per cycle, so a shorter cycle corrects sooner but feeds less per pulse. Smoke has its own timings and is not affected.', { min: 5, max: 120 }),
  N('u_min', 'Minimum auger duty', 'Smallest fraction of each cycle the auger runs (0.1 = 10%). Keeps the fire alive at low set points', { step: 0.01, min: 0, max: 1 }),
  N('u_max', 'Maximum auger duty', 'Largest fraction of each cycle the auger runs (0.9 = 90%). Stops the pot from over-filling', { step: 0.01, min: 0, max: 1 }),
  B('FanPidEnabled', 'Modulate AC fan at minimum feed', 'When the auger is already at its minimum duty, pulse the fan to hold temperature (AC fans only)'),
];

/* One switch for one question. There were three -- this one, "apply learned tuning" beside it, and
   the adaptive controller's own copy on the same page -- and they could disagree. */
const learningFields = [
  B('enabled', 'Learn from cooks', 'Measure the grill on every startup, record the steady feed at each set point and ambient temperature, and refine the tuning from how each cook actually goes'),
  B('use_library', 'Use measured tuning', 'On: what autotune measured overrides the Proportional Band, Integral Time and Derivative Time typed on this page. Off: the grill uses exactly what is typed there, which is what makes those three numbers portable to another grill of the same kind'),
  I('half_life_obs', 'Memory half-life (observations)', 'How quickly old cooks fade; ~12 observations per hour of Hold', { min: 5, max: 500 }),
];
const weatherFields = [
  { type: 'note', help: 'The feed-forward and the learning use the outdoor temperature as the ambient reference. With a postal code the grill fetches local conditions (Open-Meteo, no account) every 15 minutes and records wind and humidity with each cook. An ambient-flagged probe still takes precedence.' },
  B('enabled', 'Use local weather', ''), X('country', 'Country code', 'Two letters, e.g. us, ca, de'), X('postal_code', 'Postal / ZIP code', ''),
  { type: 'weather' },
];
/* Hold Mode: five sections, in the order you would think about them. What holds the temperature,
   how it is measured, what it learns from ordinary cooks, how the auger feeds, and the weather all
   of that is judged against. Everything to do with one of them is inside that one section --
   including the button that clears it, which is why there is no row of clearing buttons anywhere. */
async function controllerPage(view) {
  const cyc = PF.settings?.cycle_data || {};
  let tuned = '', anchors = 0, deep = 1;
  try {
    const t = await api('/tune');
    anchors = (t.anchors || []).length;
    if (anchors) {
      deep = Math.max(...(t.anchors || []).map((a) => a.runs || 1));
      tuned = PF.settings?.learning?.use_library === false
        ? ' · measured, not in use'
        : ' · tuned';
    }
  } catch { /* the summary simply says less */ }

  const clearTuning = async () => {
    if (!await confirmDialog('Clear the measured tuning?',
      'The tuning library, the last autotune and the grill model measured from startups are thrown away, and the grill goes back to the Proportional Band, Integral Time and Derivative Time you type here. Measuring them again takes hours and a hopper of pellets, so back them up first if you might want them.', 'Clear', true)) return;
    try { await api('/tune/clear', { body: {} }); toast('Back to the typed values'); setTimeout(() => location.reload(), 600); }
    catch (e) { toast(e.message, true); }
  };
  const ctl = await controllerCard(anchors > 0 && PF.settings?.learning?.use_library !== false, clearTuning);
  const sel = PF.settings?.controller?.selected || '';

  /* The three parts that report rather than configure -- which tuning is in force, the autotune and
     its library, what has been learned -- are built by the learning page and dropped into the
     section each belongs to. The tuning in force goes at the top of the Controller section, above
     the boxes you type in, because it is the answer and they are only where it starts. */
  const noteInto = el('div'), tuningInto = el('div'), learningInto = el('div');

  /* The summary line carries the numbers actually in force, not the ones typed into the form below
     it. Reading "PB 80 Ti 400 Td 30" on a grill running 82/523/33 is worse than reading nothing:
     the whole reason to put a value on a collapsed row is so it can be trusted without opening it. */
  const SRC = { tuned: 'measured', learned: 'learned', typed: 'typed' };
  const summarise = () => {
    const t = PF.status?.controller?.tuning;
    if (t) return `${sel} · PB ${t.PB} Ti ${t.Ti} Td ${t.Td} · ${SRC[t.src] || t.src}`;
    const cfg = PF.settings?.controller?.config?.[sel] || {};
    const pid = [cfg.PB != null ? `PB ${cfg.PB}` : null, cfg.Ti != null ? `Ti ${cfg.Ti}` : null, cfg.Td != null ? `Td ${cfg.Td}` : null].filter(Boolean).join(' ');
    return `${sel}${pid ? ` · ${pid}` : ''}${tuned}`;
  };
  const ctlFold = fold('Controller', summarise(), el('div', {}, noteInto, ctl), 'sliders-horizontal', '#ff8a1f');
  const offStatus = onStatus(() => { const l = ctlFold.querySelector('summary .s'); if (l) l.textContent = summarise(); });
  view.append(ctlFold);

  view.append(fold('Auto Tuning', anchors
    ? `${anchors} temperature${anchors === 1 ? '' : 's'} measured${deep > 1 ? ` · ${deep} runs deep` : ''}`
    : 'nothing measured yet',
    tuningInto, 'target', '#0a84ff'));

  view.append(fold('Learning', PF.settings?.learning?.enabled === false ? 'off' : 'on',
    el('div', {}, pageCard({ title: '', sections: [{ id: 'learning', title: '', fields: learningFields }] }), learningInto),
    'brain', '#bf5af2'));

  view.append(fold('Feed', `${cyc.HoldCycleTime ?? '—'} s · ${cyc.u_min ?? '—'}–${cyc.u_max ?? '—'} duty`,
    pageCard({ title: '', sections: [{ id: 'cycle_data', title: '', fields: holdCycleFields }] }), 'timer', '#ff9f0a'));

  view.append(fold('Weather', PF.settings?.weather?.enabled ? (PF.settings.weather.postal_code || 'on') : 'off',
    pageCard({ title: '', sections: [{ id: 'weather', title: '', fields: weatherFields }] }), 'cloud-sun', '#64d2ff'));

  const stop = renderLearning(view, { note: noteInto, tuning: tuningInto, learning: learningInto });
  return () => { offStatus(); stop?.(); };
}
/* Pellets & Hopper: two subjects, each whole.
 *
 * The hopper is a sensor and a scale -- what it reads now, the two buttons that teach it the ends
 * of that scale, and the numbers those produce. The pellets are a brand, how much of them has been
 * burned, and the list to choose from. The level used to be printed inside the loaded-pellets card
 * with the calibration buttons under it, which put a control for the sensor inside a card about
 * which wood is in the grill. */
const hopperFields = [
  /* The low-pellet warning is a conditional notification like everything else, so it is set up
     where the others are rather than having a second switch here that disagrees with it. */
  { type: 'note', help: 'Low-hopper warnings are conditional notifications: see Notifications \u2192 Conditional Notifications, where "Hopper Low" and "Hopper Critical" set the levels and where they are sent.' },
  I('empty', 'Sensor reading when empty (cm)', 'Distance from the sensor to the bottom of the hopper', { min: 1 }),
  I('full', 'Sensor reading when full (cm)', 'Distance from the sensor to a full load', { min: 0 }),
];
function pelletsPage(view) {
  const hopper = el('div', { class: 'card' });
  const fields = pageCard({ sections: [{ id: 'pelletlevel', title: '', fields: hopperFields }] });
  view.append(el('h2', {}, 'Hopper'), hopper, fields);
  /* Measuring an end of the scale writes the same setting the boxes below hold, so those boxes are
     redrawn from what was stored. Two places showing one number, disagreeing, is the whole reason
     anyone stops trusting a screen. */
  return renderPellets(view, { hopper, onCalibrated: async () => {
    try { PF.settings = await api('/settings'); fields.rerender(); } catch { /* the boxes redraw on the next visit */ }
  } });
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
    setBack('#/settings', 'Settings');
    /* Auger & Feed was split: the cycle and the duty limits only ever affected Hold, and the smoke
       timings only ever affected Smoke. The old address still works rather than dead-ending. */
    const MOVED = { auger: 'controller', push: 'services', integrations: 'services' };
    if (MOVED[page]) { location.replace(`#/settings/${MOVED[page]}`); return; }
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
    /* Cooking reads in the order a cook happens rather than the order the pages were written:
       light it, hold it or smoke it, the things that happen during, and what is left afterwards.
       Anything not named here follows, so a new page appears rather than disappearing. */
    const order = ORDER[sec] || [];
    const rank = (p) => { const i = order.indexOf(p.key); return i < 0 ? order.length : i; };
    const rows = pages.filter((p) => p.section === sec).sort((a, b) => rank(a) - rank(b))
      .map((p) => ({ href: `#/settings/${p.key}`, icon: p.icon, brand: p.brand, color: p.color, title: p.title, sub: p.sub }));
    for (const l of LINKS[sec] || []) rows.push(l);
    if (rows.length) view.append(listGroup(sec, rows));
  }
}
