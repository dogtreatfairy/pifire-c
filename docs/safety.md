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
