# Safety design

PiFire drives an igniter, an auger and a fan on a fire. The daemon is built so that a software fault, a crash or a stalled thread never leaves the pot feeding or the igniter on.

## Output path

Every relay write goes through one arbiter (`src/core/outputs.c`) guarded by a mutex and an atomic **safe latch**. Only the control thread writes outputs in normal operation. Once the latch is set, every write is refused until the process restarts.

* **Watchdog thread**: pets systemd only while the control thread has ticked in the last 2 s. On a stall it sets the latch, tries to drive all outputs off, and aborts the process so systemd restarts it (`Restart=always`, `WatchdogSec=15`).
* **Crash state**: on Linux a released GPIO line floats. `pifire-boardcfg --pulls` parks the relay pins as inputs with a pull that keeps active-low or active-high relays *off* in `config.txt`, so the relay state after `SIGKILL` or a kernel watchdog reboot is defined by hardware, not software. Verify on your board: kill `pifired` with the auger on and confirm the relay drops.
* **Hardware watchdog**: `dtparam=watchdog=on` + `RuntimeWatchdogSec=10` (installer) reboots a hung kernel.
* **Unclean restart**: if the previous run did not exit cleanly and the pit is above `safety.restart_hot_temp` (150 °F), the daemon enters Shutdown (fan on) instead of Stop so a burning pot is never left without air.

## Interlocks (checked every 100 ms, independent of the controller)

| check | default | action |
|---|---|---|
| Overheat, any mode | `safety.maxtemp` 650 °F | Error `E01_OVERTEMP`; auger/igniter off, fan runs `error_cooldown_fan_s` if the pit is hot (probes read to 750 °F so this trips as overheat, not as a probe fault) |
| Set point range | `safety.min_target` 160 °F – `safety.max_target` 550 °F | the API refuses a set point outside it; the controller holds any other source inside it |
| Smart Start (Startup, Relight) | `startup.smartstart`: +3 °F, then +12 °F, each within 300 s | see *Smart Start* below; no proof → `E04_STARTUP_FAILED`, grill stops |
| Flame-out in Smoke/Hold | 20 °F (`safety.relight_drop`) below the working temperature, or the peak while heating | Relight (`reigniteretries`, 1 per cook) then Error `E02_FLAMEOUT`; see *Flame supervision* |
| Igniter cap | 5 min (`safety.igniter_max_on_s`, never more than 300 s) | igniter forced off, `W07_IGNITER_CAP` |
| Auger cap | 60 s continuous | auger forced off (also applies in Manual), `W08_AUGER_CAP` |
| Primary probe fault | 10 s without a valid reading while cooking | Error `E05_PROBE_FAULT` |
| Controller fault | 3 invalid outputs in a row | switch to the built-in PID, `E06_CONTROLLER_FAULT` |
| Manual overrides | expire after `manual_override_time` unless in Manual mode | |
| E-stop | `POST /api/v1/stop`, the Stop button, MQTT `{"cmd":"stop"}` | always honoured |

Error is sticky: only Stop clears it, and the UI shows the reason until then.

## Feed limits

The controller output is a ratio of the cycle; the daemon enforces `cycle_data.u_min` (prevents flame-out) and `u_max` (lets the pot catch up on a step change) and never changes the on-time mid-cycle.

## Learning and autotune

Learning only *observes* (steady-state feed vs. set point and ambient); it never bypasses the interlocks. Autotune oscillates the feed by ±0.15 around the learned feed-forward with a 1 °C hysteresis, aborts if the pit runs 50 °F over the set point or stops oscillating for 15 min, and any mode change cancels it. Its result is a suggestion until you press *Apply*.

## Smart Start

One sequence decides the grill is lit, for a first light and for a Relight alike. It follows a burner
control's trial for ignition (a hard time limit, then a second proof before handing over), with the
pit temperature in place of a flame sensor. Every threshold is a rise over the grill's own lowest
reading, so it works the same at -30 °F outside as at 90 °F; the weather only moves where it starts.

1. **Prove.** The pit, filtered over 10 s, must hold `prove_rise` (3 °F) above its running minimum
   for 15 s within `prove_s` (300 s, never more than the igniter may run). The minimum keeps moving
   down while the fan cools the pit, so the turn upward is the evidence. A barrel 150 °F or more
   above the outdoor air may instead prove it by its fall stopping: the lowest reading not moving
   down by 1 °F for 2 min, which no barrel without a fire can do (one with a time constant three
   times Ryan's still loses more than 4 °F).
2. **Exit.** Proven, startup carries on until the pit is `exit_rise` (12 °F) over the minimum, within
   a second `prove_s`, then hands over to Smoke or Hold. Three degrees alone cannot tell a fire from
   the igniter's own heat; twelve can, and a real fire makes it at once (27 s on Ryan's grill).
3. **Fail.** No proof, or no exit rise, in time is `E04_STARTUP_FAILED` and the grill stops. There is
   no second light: a pot that did not catch is a pot full of pellets.

The feed during a light is the ambient startup profile, or, in a hot barrel, the learned feed for
36 °F above the pit when that is more (Ryan's choice for Relight, 2026-09-29): enough that a caught
fire raises the pit, which is how the light is proven.

The thresholds were checked against Ryan's grill: a warm restart on 2026-09-29 fell for 219 s under
the fan with the igniter on (no igniter heat reached the probe), caught at about 220 s, and passed
+3 °F at 258 s and +12 °F at 285 s. A 30 s filter and 30 s hold would have proven it at 309 s, a
failed start on a fire that had lit; the 10 s filter and 15 s hold prove it at 279 s.

## Flame supervision

Startup raises the pit from its baseline; Smoke and Hold go on raising it from where startup handed
over until it reaches the **working temperature** — the set point in Hold, `safety.smoke_min`
(180 °F) in Smoke, where the P-mode is set so that Smoke sits at or above it. Anything else is a
flame-out or a feed that is too low.

* **Fall.** The reference is the working temperature once reached, and before that the highest the
  pit has been. Falling `relight_drop` (20 °F) below it is a flame-out: a Relight through Smart Start
  if `relight_enabled` and attempts remain, otherwise `E02_FLAMEOUT` and the grill stops.
* **Climb.** Below the working temperature (by more than 10 °F) the pit must gain 3 °F in every
  300 s. Until it is 12 °F past the handover, a pit that does not is a fire that never got going:
  `E04_STARTUP_FAILED`, grill stops. After that the fire is burning what it is given, so nothing is
  piling up: it is reported (`W10_NOT_HEATING`, "feed too low" — lower the P-mode in Smoke), and the
  fall rule stops the grill if the fire is failing.
* **Coast.** A set point lowered below the pit, or Smoke entered from a hotter Hold, is a coast: a
  starved fire and a dead one cool alike, so nothing is judged on the way down. When the pit reaches
  the working temperature the igniter runs for `relight_prove_s` (180 s) while the feed comes back,
  catching the fire if it has died down. Temperature alone cannot tell a dying fire during a coast;
  this proves it where it can be proven.
* **Excluded:** an open lid and the recovery after it (at most 5 min, as for notifications — a fall
  still going after that is not the lid), and a tuning measurement.

Notifications: the *Heating* rule fires when startup hands over ("Hold · Heating to 225°F", or
"Heating" in Smoke); *Relight* fires when a flame-out is relit; *At Temperature* when the working
temperature is reached.

Background: industrial burner controls (NFPA 85/86, EN 298) prove flame within seconds, allow at most
one or two recycles with a purge, then lock out. Pellet appliances prove ignition by a flue or pit
rise within a time limit and tell the owner to empty the pot after a failed ignition (EN 14785
manuals); Weber's pellet-grill patent relights automatically a limited number of times. With minutes
of thermal lag this grill cannot detect flame loss in seconds, so it is kept safe by not feeding a
fire it has not proven and not relighting a pot that never caught.

## Safety audit, 2026-09-29

Three independent reviews (outputs and process failure; control and safety logic; the remote
surface) and the fixes that followed.

- **Relays after a crash.** On Raspberry Pi kernels a released GPIO line keeps the level it was last
  driven to (`pinctrl_bcm2835.persist_gpio_outputs=Y`, confirmed on the reference grill), so a crash
  with the auger on left it on. Now: `ExecStopPost=pifired --outputs-off` drives every relay off
  after any stop; `StartLimitIntervalSec=0` keeps systemd restarting; the installer adds
  `persist_gpio_outputs=n` to the kernel command line so released pins fall to their pulls.
- **Emergency paths** switch the outputs off before logging; the safe latch is re-checked under the
  HAL lock; PWM frequency is latch-gated; shutdown has an 8 s deadline after which outputs are forced
  off.
- **Recovery after a restart** (power loss or crash) relights at most once per cook; never an
  interrupted startup whose fire Smart Start had not confirmed; never when the Pi rebooted with an
  unsynchronised clock and the pit has lost more than 30% of its heat; restores relight retries; an
  interrupted tuning run shuts down instead of resuming. Otherwise E08.
- **Flame-out and ignition** are Smart Start and flame supervision (above). Every trigger is a
  temperature or a time, never a weight, so it behaves the same on any grill.
- **Hold or Smoke from Manual, Prime or Shutdown** goes through Startup. Relight and Error cannot be
  requested; Prime and Manual only from Stop or Monitor; Prime is one auger run at most (`auger_max_on_s`)
  and ends when its feed does.
- **Set point** stays within `safety.min_target`–`max_target` (160–550 F), whoever sets it.
- **Error cool-down fan** stops if the pit rises 10 F above where it was when the error was raised
  (it would be feeding a fire). An overtemperature in Stop or Error raises an alert.
- **Igniter cap** is enforced even while the probe is invalid and stays tripped for the mode;
  default and ceiling 300 s. **Auger cap** is followed by a 15 s rest.
- **Shutdown** runs its time and then until the pit is below `restart_hot_temp`, capped at 3x.
- **A clean start with a hot pit** that is not resuming a cook runs the Shutdown cool-down.
- **Wi-Fi recovery** reloads the radio driver at any time but restarts the Pi only when the grill is
  stopped and cool (a restart mid-cook leaves the fan off while the pot smoulders, then relights).
- **Remote surface.** PiFire runs on trusted home networks and is driven by Home Assistant,
  automations and people's own dashboards, so the API, the WebSocket and MQTT accept commands from
  any host or origin (CORS open), the API answers normally while the setup hotspot is up, and the
  hotspot password is the documented `pifire1234`. Restrictions on all of these were tried in
  alpha.189 and rolled back. What stays: settings limits have ceilings and are reset to defaults when
  out of range at load; backup restores are validated and refuse symlinks; SMB values are checked and
  the folder passed with `-D`; the root helpers resolve and validate their arguments; webhooks are
  http and https only.
- **Not done yet:** release signing (updates are checked against SHA256SUMS from the same release,
  which proves integrity, not authorship) and redacting stored secrets from `GET /settings`.
