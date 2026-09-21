# How the grill learns

The default controller is **Adaptive** (`controllers/adaptive.c`). It is built so that a grill gets better with every cook without anyone touching a PID number, in four layers. Everything below is on by default; *Settings → Cooking → Temperature control & learning* and the controller's own *Learn tuning automatically* switch turn parts of it off.

## 0. Autotune: the tuning library (`features/tuner.c`)

A pellet grill loses heat by convection *and* by radiation, and radiation grows with the fourth power of absolute temperature. The same extra gram of pellets therefore buys far fewer degrees at 450 °F than at 180 °F, so the grill presents a different loop at each end of its range and no single proportional band suits both. Autotune measures the loop at a temperature and files the answer in the **tuning library**, one entry per temperature.

Either kind of run is hands off. The grill starts itself, waits until the set point has been reached and held, runs the relay test (the feed oscillates gently around the target until seven crossings have been counted, giving the ultimate gain and period), turns that into PB, Ti and Td with Tyreus-Luyben, and shuts down at the end.

The relay swings around the duty the grill **actually ran** while it settled, averaged over the last ten minutes, not around a modelled figure. That distinction decides whether the test works at all: the low half of the swing has to be below the duty the grill needs, or the pit never comes back down through the set point and there is nothing to measure. The swing is also sized to fit between the minimum and maximum feed, shrinking rather than being clamped on one side, because the ultimate gain is computed from the size of the swing and a clamped one would overstate it. If the pit still will not cross, or runs far past the set point, the centre moves that way and the measurement starts over, up to eight times. Once it has crossed even once the centre is evidently workable and the test becomes patient, because a pellet grill's cooling half genuinely runs to ten minutes at a low set point.

### What the measurement computes

The relay test is the Åström-Hägglund method. The feed is switched between two levels either side
of the operating duty whenever the error passes a hysteresis band of 1 °C, which drives the loop
into a limit cycle at the frequency where the grill's own phase lag reaches 180°. From that cycle:

* **Period.** One full oscillation is a rise half-cycle plus the fall half-cycle beside it. Doubling
  either one on its own, which is only correct for a symmetric cycle, overstated the period by half
  again on a real grill, whose halves ran 336, 114, 483, 126 and 756 seconds.
* **Amplitude.** Also measured over a full oscillation, because dead time puts the high peak in one
  half and the low peak in the other. Measuring within a single half saw only part of the swing and
  overstated the ultimate gain roughly two-fold; against the grill's independently identified plant
  model the old figure was 2.7× too high, which is a proportional band less than half as wide as it
  should be, and a loop that hunts.
* **Ultimate gain.** `Ku = 4h / (π√(A² − ε²))`, where `h` is half the swing in duty *actually
  delivered* after the minimum and maximum feed limits, and `ε` is the hysteresis. The square root
  projects onto the −180° crossing that the tuning rules are written against; taking `4h/(πA)`
  alone gives the point the relay identifies, which the hysteresis leaves a little short. The
  correction is about 2% on a healthy swing and 25% on a marginal one.
* **When to stop.** Seven crossings at the earliest, and then only once two consecutive oscillations
  agree within 25% in period and 30% in amplitude. A limit cycle that is still growing describes the
  transient, not the grill. Only the last three complete oscillations are averaged; the earlier ones
  are the approach. If it has not settled by twelve crossings the result is taken anyway and the
  event says it was still drifting.

`Ku` and `Pu` become PB and Ti by **Tyreus-Luyben PI** (`Kc = Ku/3.2`, `Ti = 2.2·Pu`, no derivative).
Tyreus-Luyben is the conservative counterpart to Ziegler-Nichols and suits a process whose dead time
is around half its time constant, where derivative action buys little and mostly amplifies noise.
The rule lives in exactly one place, `pf_tuning_from_relay` in `pifire/common.h`, because the daemon
files the result in the library while the controller is handed `Ku` and `Pu` directly: these once
used different formulas, so one measurement meant two tunings and which one the grill ran depended
on whether the library happened to cover the set point being held.

One adjustment on top: Tyreus-Luyben sets the integral time from the period alone, which on a grill
with this much dead time lands at several times the plant's own time constant, so an offset would
take the best part of an hour to clear on a barrel that responds in seven minutes. Where the passive
startup fit knows the time constant, the integral time is capped at it, which is what SIMC does and
what keeps a conservative tuning from becoming a sluggish one.

Rules written against how far the grill is from its target stay quiet while a measurement runs. The relay is deliberately driving the pit either side of the set point, so "running hot" would be reporting the tuner's own doing several times per set point. The pit temperature itself is still a fact and still testable. It drives the grill only through the ordinary command queue and reads only the published status, so it can do nothing a patient person with the web app could not do, and Stop always wins. The grill should be empty, and a run will not start while one is cooking.

*Settings → Cooking → Temperature Control & Learning → Autotune* offers two:

* **Full Profile** visits every temperature in `learning.tune_setpoints` (180, 225, 350 and 450 °F by default) in turn, climbing so the grill never has to cool down. It is the grill's new baseline, so it **clears the library** before it starts. A few hours.
* **One Temperature** tunes a single temperature you pick between 180 and 450 °F and **adds** it to the library beside what is already there. About an hour. Use it for a temperature the profile does not cover, or one that has drifted.

Every entry records the outdoor temperature and wind it was measured in, from the ambient probe or the local weather, and the app shows them: the same grill behaves differently on a still summer afternoon than in a winter wind, and an entry means little without the conditions behind it.

While holding, the daemon looks up the two entries that bracket the current set point and interpolates PB, Ti and Td between them, clamping to the nearest entry outside the measured range (`pf_learning_gains`). The controller prefers this over anything it learned passively and over the configured numbers, and its note says `tuned` when it is following the library. From there the ordinary learning carries on: the feed-forward keeps fitting how much fuel this grill needs, and the performance monitor keeps nudging the loop gain from how each cook actually behaves, so a tune is a starting point that keeps improving rather than a fixed answer.

A run that a restart interrupts is noticed at the next boot and reported, and whatever it had already measured is kept. The library survives reboots (kv namespace `learning`, key `anchors`) and is cleared by *Reset learning*.

In the simulator (`tests/test_tuner.c`) a full profile takes the mean holding error at 180 °F from 4.3 °F to 0.2 °F, with 225, 350 and 450 °F all inside half a degree; the same test asserts that the controller is actually running on the library rather than merely storing it, and that a single-temperature run adds to the library while a full profile replaces it.

## 1. Feed-forward: how much fuel this grill needs (`features/learning.c`)

While holding a temperature, whenever the pit has been within 3 °C of the set point for three minutes with no lid event or set-point change, the daemon records an observation: set point, ambient temperature, the mean auger duty it took, and the pit noise (a wind proxy). Observations are fitted with a recency-weighted ridge regression

    u_ff = a + b · (setpoint − ambient)

which is the physics of a pellet grill (heat loss is proportional to the temperature difference), so a cook on a 30 °F day and one on a 100 °F day both inform the same two numbers instead of needing separate tables. Windows where the auger sat at its minimum duty are excluded (they say nothing about demand). Newer cooks weigh more (`learning.half_life_obs`), and a prior keeps the fit sane until real data exists. The controller adds only a gentle correction on top of `u_ff`, which is why it settles fast and does not hunt.

## 2. Plant model: how this grill responds (`control.c` `learn_rise_track`)

Every startup that runs into Hold is treated as a step test. The rise from the initial pit temperature to the set point is fitted as a first-order-plus-dead-time model (K, τ, θ) using the 28 %/63 % two-point method; results are averaged with the previous estimate and stored. Nothing is injected into the cook to get this.

As soon as a new model exists (`learning.auto_tune`), the daemon hands it to the controller. Adaptive derives PB/Ti/Td from it with the SIMC rules (τ_c = θ), blends them with what it had, and keeps them within 0.5–1.5× the configured baseline because the passive fit is deliberately crude (dead time absorbs the ignition delay). The relay **autotune** (*Settings → Cooking → Temperature control & learning → Autotune*, run without food) does the same with measured Ku/Pu and is trusted over a wider band (0.33–3×). Learned gains are persisted per controller and restored on the next boot.

## Approach without overshoot (in the controller)

Two rules keep the climb to a new set point from overshooting, learned from a real cook that went 17 °F over:

* **Integrator trim on arrival** — whatever the integrator accumulated while the pit was more than 15 °F away is approach wind-up, not a steady-state correction; on entering the ±15 °F band it is clipped to ±0.15 duty (never reset to zero, so the pit does not sag).
* **Coast look-ahead** — the pot keeps heating for about one dead time after the feed is cut. While climbing at ≥ 3 °C/min, the controller predicts `rise rate × θ` (θ from the plant model, 40–240 s) and drops to the steady-state feed as soon as the pit would coast to the target on its own. The tuning note shows `· coasting` while this is active.

The plant model's clock starts when the fire is evidently lit (+3 °C over the startup baseline), so the ignition delay no longer inflates θ.

Two more rules came from a cook that sat 20–35 °C *under* a 350 °F target for its whole length:

* **Ambient must be plausible** — the feed-forward is `b · (setpoint − ambient)`, and that cook had restarted Startup on a grill that was still at 166 °C, so "ambient" was taken from the pit (129 °C) and the feed-forward came out at 0.19 duty instead of the 0.62 the grill needed. Only a reading that could be outdoor air (≤ 50 °C) is accepted, from the pit at Startup, the cold-start baseline or an ambient-flagged probe; otherwise the last plausible value (persisted across restarts) is used, and 20 °C before any exists.
* **Integrator seeding** — entering Hold far from the target no longer seeds the integrator "bumplessly" from the previous duty (that duty was the smoke cycle or the `u_min` placeholder, and the seed parked a −0.3 duty integral that unwound at Ti = 286 s, holding the feed back for many minutes). Far from the target the integrator starts at zero; within ±15 °F it is seeded bumplessly, capped at ±0.15 duty. The integral also never opposes a large error: a negative integral while the pit is far below target (or positive far above) is cleared.

## 3. Performance monitor: does it actually behave? (in the controller)

Every ten minutes of Hold the controller scores itself:

| observation | action on the loop gain |
|---|---|
| ≥ 3 error sign changes with peaks ≥ 3 °C (hunting) | × 0.85 |
| mean error > 3 °C, no crossings, not saturated, no recent set-point change (sluggish) | × 1.15 (× 1.25 if > 6 °C) |
| overshoot > 5 °C and > 15 % of a set-point step | × 0.9 |

The correction is learned **per temperature band** (below 200 °F, to 275 °F, to 400 °F, above), because a grill that hunts while smoking at 180 °F is not necessarily hunting while searing at 450 °F, and one number across the whole range would average away the very difference the controller is trying to learn. Each band is bounded to 0.4–2.0, persisted, and pulled halfway back toward 1.0 whenever a fresh plant model arrives so the two mechanisms do not fight. Windows spent saturated at the minimum or maximum duty are ignored: a grill that cannot go any lower is not the loop's fault.

Together with the tuning library this is how the grill learns what temperature does to it: autotune measures each temperature directly, the feed-forward keeps fitting how much fuel each set point needs against the weather, and the monitor keeps correcting the loop at each end of the range from how the cooks there actually went.

## What you see

* Home → *Controller* details and *Settings → Cooking → Temperature control & learning*: the tuning in use (`PB … Ti … Td … ×scale learned`), the feed-forward fit with example duties, the plant estimate, autotune results, recent observations, and *Reset learning*.
* Events: `TUNING_LEARNED` after a startup produced a model, `Autotune_Done` when a relay test finished.

In the simulator (`tests/test_learning.c`) three consecutive cooks at the same conditions go from an integrated error of ~20 500 °C·s to ~15 200 °C·s (−26 %), and the fitted model predicts more fuel on a cold day than a hot one.

## Limits (honest)

The passive plant fit is rough and the monitor is rule-based, not a model-predictive controller. The learning cannot see pellet brand or wind directly (pellet brand is stored with observations for a future per-brand offset). Until an autotune run has been done, every set point reuses the same PB/Ti/Td and only the feed-forward changes with set point; the tuning library is what removes that limitation, and between its entries the daemon interpolates rather than measuring.
