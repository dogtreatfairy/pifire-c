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
| Cold-start | off; `delta_rise` 12 °F within the startup duration | baseline = running minimum in the first 60 s; startup continues until the pit rises by the delta; timeout → retry, then `E04_STARTUP_FAILED`; with `coldstart.exit_on_rise` startup ends as soon as the rise is confirmed and the pit is above `minstartuptemp` (otherwise the full timer runs, or `startup_exit_temp` ends it); the flame-out floor becomes `max(baseline+delta, 0.9×exit temperature)` so a cold-weather start is not judged against a fixed 75 °F |
| Igniter cap | 20 min | igniter forced off, `W07_IGNITER_CAP` |
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
