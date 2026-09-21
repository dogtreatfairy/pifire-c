# Cook log (analysis export)

*History → Analysis log* next to any saved cook, or `GET /api/v1/cooklog` (running cook, else the latest finished cook, else the last 6 hours; `?from=&to=` for an explicit wall-clock window). The file is meant to be handed to a person or an assistant to judge how the controller behaved and what to change. Everything in it is **Celsius** and duties are **0..1** so units settings never matter.

```
format        "pifire-cooklog/1"
version/arch  daemon build
cook          {name, start, end, duration_s}  (wall-clock seconds)
controller    {id, tuning_note}   the tuning in use at export time, e.g. "ff 0.42 · PB 120 Ti 237 Td 60 ×1.15 learned"
settings      controller (selected + every variant's config), cycle_data, safety, startup, shutdown,
              smoke_plus, pwm, keep_warm, learning, augerrate, platform (board, DC fan), probe map, pellets
learning      feed-forward fit (a, b, n, rms, examples), plant estimate (K, tau, theta), autotune result
samples       columnar, one entry per history sample (settings.history.sample_s, default 3 s):
  t           wall-clock seconds
  mode        0 Stop 1 Monitor 2 Prime 3 Startup 4 Reignite 5 Smoke 6 Hold 7 Shutdown 8 Manual 9 Error
  setpoint    °C, 0 outside Hold
  u_raw       controller output before clamping
  u_applied   auger duty actually run this cycle (after [u_min, u_max] and safety caps)
  u_ff        learned feed-forward for this set point / ambient
  p, i, d, ff controller terms that made u_raw
  fan_pct     0 when the fan is off, else the commanded speed (100 for an AC fan)
  outputs     bitmask: 1 power, 2 fan, 4 auger, 8 igniter
  ambient     °C, from the ambient probe or the cold-start baseline
  flags       1 lid_open, 2 smoke_plus, 4 pwm_control, 8 target_reached, 16 coldstart_active, 32 saturated at u_min, 64 saturated at u_max
  pmode       P-mode in force
  cycle_s     controller cycle length
  probes      {<label>: {temp[], target[]}} aligned to t by index (null when the probe had no valid reading)
events        {ts, level, code, message} — mode changes, safety events, TUNING_LEARNED, autotune, alerts
```

## What to look at

* **Transient time**: from Startup entry to the first sample within ±3 °C of the set point, and after every set-point change. Long approaches with `u_applied` pinned at `u_max` (flag 64) are pot/plant limited, not tuning limited; long approaches with u well below u_max mean the loop gain (PB) is too soft or the feed-forward under-estimates.
* **Overshoot**: peak of `probes.<primary>.temp − setpoint` after reaching it. Overshoot with `i` still large means the integrator wound up during the approach (Ti too short, or conditional integration not engaging); overshoot with a large `ff` means the feed-forward is high for this set point/ambient (check `learning.fit` against the steady `u_applied` later in the cook).
* **Steady state**: standard deviation and mean error while flag 8 is set and the lid is closed. Periodic swings with a period of 3–8 minutes are hunting (PB too narrow or Td too small); a slow drift is Ti too long.
* **Lid events** (flag 1): how far the pit fell and how long recovery took; the auger is paused during the pause window by design.
* **Flame-out margin**: minimum `temp` in Smoke/Hold versus `settings.safety` floors and the cold-start floor.
* **Feed-forward quality**: `u_ff` versus the mean `u_applied` in calm Hold windows; a consistent offset is what the learning layer corrects over cooks, a per-set-point difference suggests the physics prior needs a set-point term.

The adaptive controller's learned PB/Ti/Td/scale are in `controller.tuning_note`; the events show when they changed (`TUNING_LEARNED`). Manual tuning goes in *Settings → Cooking → Temperature control & learning*; turning off *Learn tuning automatically* freezes the loop at the entered values.
