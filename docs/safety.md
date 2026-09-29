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
| Over-temperature, any mode | `safety.maxtemp` 550 °F | Error `E01_OVERTEMP`; auger/igniter off, fan runs `error_cooldown_fan_s` if the pit is hot |
| Flame-out in Smoke/Hold | pit below the startup floor | Reignite (`reigniteretries`) then Error `E02_FLAMEOUT` |
| Smart Start (`safety.coldstart`) | on; `delta_rise` 12 °F within `timeout_s` 300 s | shown under Settings › Startup & Shutdown. Baseline = running minimum of the 30 s-filtered pit in the first 60 s; startup cannot finish until the pit is `delta_rise` above it; no rise by `timeout_s` from the start of startup → `E04_STARTUP_FAILED` straight away (no second light: an unlit pot is a pot full of pellets). Skipped for a hot grill (pit ≥ 140 °F or `startup_exit_temp`, whichever is higher), where a failing fire is the flame-out check's. The flame-out floor becomes `max(baseline+delta, 0.9×exit temperature)` |
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

## Flame-out protection

The classic flame-out check is a fixed floor: after startup the daemon computes a temperature the
pit should never fall below, and dropping under it means the fire is out and the grill has to start
again. That floor is the last word, and by the time it speaks there is usually nothing left in the
pot to catch.

Flame-out protection is the earlier, cheaper answer. There are two ways to arrive at a fire in
trouble, and they need different triggers.

**Holding.** The grill reached its set point and the pit is sliding away from it. Nothing has been
asked of the grill, so any real distance below the target is a fault: the trigger is falling
`safety.relight_drop` (20 °F by default) below the set point.

**Coming down.** The set point was lowered by more than `safety.relight_drop`, so the grill
deliberately starves the fire and coasts. That coast is exactly when a fire dies, and by the end of
it there may be nothing left to catch — waiting for another twenty degrees of undershoot would mean
waiting through the most dangerous part of the manoeuvre. So the trigger here is the moment the pit
**crosses the new set point on the way down**: the point from which it ought to be recovering rather
than still falling. It arms only when the pit is above the new target when the change is made, since
a set point dropped to somewhere the grill has not reached yet involves no coast at all.

Both end on the pit climbing back **above the lowest point it reached** — recovery from the bottom
of the dip is the evidence the fire is winning, and waiting for the whole way back to the set point
would hold the igniter on through the entire recovery. The lowest point keeps moving down while the
pit is still falling, so the test is always against the bottom of this dip and not where the igniter
came on.

How much of a climb counts depends on which trigger started it, because the two are asking different
questions:

| Started by | Ends on | Why |
|---|---|---|
| A fire falling away from its target | `safety.relight_recover`, 10 °F | The question is whether there is a fire at all, and only a substantial rise answers it |
| A coast down to a lower set point | `safety.relight_recover_step`, 3 °F | The fire was never in doubt, only starved. The question is merely whether the pit has stopped falling, and a couple of degrees of turnaround is the whole answer |

Four things bound it:

* **The holding trigger only applies to a pit that had arrived.** A grill climbing to a set point,
  or to a new one after a change, is far below it for ordinary reasons. `target_reached` is cleared
  when the set point changes, so a step up re-arms it exactly as a fresh cook does. The coast-down
  trigger is the deliberate exception: it exists precisely for the window where the grill has not
  arrived yet.
* **An open lid is excluded.** The pit falls twenty degrees because the heat walked out, not
  because the fire went out, and the igniter has nothing to fix.
* **A tuning measurement is excluded.** The relay deliberately drives the pit to both sides of the
  set point and leaves it there for minutes at a time. That is the measurement, not a fire in
  trouble, and lighting the igniter would both corrupt it and have nothing to fix.
* **The igniter's continuous-on cap still applies and still wins.** Protection never overrides it.
* **It gives up.** If the pit has not climbed back to within half the trigger distance of the set
  point within `safety.relight_timeout_s` (5 minutes), the fire is out rather than struggling and it
  hands over to the flame-out path, which knows how to restart the grill and when to stop trying.
  Without that deadline the assist would hold the igniter on until its own cap while keeping the pit
  just warm enough that nothing else noticed.

The escalation clock is deliberately *not* reset by the igniter switching off after a recovery: the
igniter's own heat can lift the pit a few degrees with the fire still out, so the assist can cycle.
Only the pit genuinely climbing back towards the set point resets it.


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
- **Fuel without heat.** In Hold, Smoke and after a relight's light, grams fed while the pit has
  fallen over the last five minutes are counted; past `safety.max_unburnt_g` (100 g) the auger stops
  and it is E02. A hot relight must rise 3 C within the Smart Start time or it is E02.
- **Hold or Smoke from Manual, Prime or Shutdown** goes through Startup. Reignite and Error cannot be
  requested; Prime and Manual only from Stop or Monitor; Prime is clamped to 50 g and ends when its
  feed does.
- **Set point** is held 25 F below `safety.maxtemp`, whoever sets it.
- **Error cool-down fan** stops if the pit rises 10 F above where it was when the error was raised
  (it would be feeding a fire). An overtemperature in Stop or Error raises an alert.
- **Igniter cap** is enforced even while the probe is invalid and stays tripped for the mode;
  default 600 s. **Auger cap** is followed by a 15 s rest.
- **Shutdown** runs its time and then until the pit is below `restart_hot_temp`, capped at 3x.
- **A clean start with a hot pit** that is not resuming a cook runs the Shutdown cool-down.
- **Wi-Fi recovery** reloads the radio driver at any time but restarts the Pi only when the grill is
  stopped and cool (a restart mid-cook leaves the fan off while the pot smoulders, then relights).
- **Remote surface.** Requests must be addressed to the grill by a name it answers to (DNS rebinding);
  a browser Origin must match; state-changing requests must be JSON (or gzip for a restore); no
  CORS; the WebSocket checks the same. Settings limits have ceilings and are reset to defaults when
  out of range at load. Backup restores are validated and refuse symlinks. SMB values are checked and
  the folder passed with `-D`. The root helpers resolve and validate their arguments. The setup
  hotspot gets a random per-device password (shown on the panel) and serves network setup only.
  MQTT accepts no commands unless `mqtt.allow_control`, and then only stop, shutdown, set point,
  timers and targets.
- **Not done yet:** release signing (updates are checked against SHA256SUMS from the same release,
  which proves integrity, not authorship) and redacting stored secrets from `GET /settings`.
