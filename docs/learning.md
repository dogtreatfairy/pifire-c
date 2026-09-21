# How the grill learns

The default controller is **Adaptive** (`controllers/adaptive.c`). It is built so that a grill gets better with every cook without anyone touching a PID number, in four layers. Everything below is on by default; *Settings → Cooking → Temperature control & learning* and the controller's own *Learn tuning automatically* switch turn parts of it off.

## 0. Guided tuning: one button, every set point (`features/tuner.c`)

A pellet grill loses heat by convection *and* by radiation, and radiation grows with the fourth power of absolute temperature. The practical consequence is that the same extra gram of pellets buys far fewer degrees at 450 °F than at 180 °F, so the grill presents a different loop at each end of its range and no single proportional band suits both. Guided tuning measures the loop at several set points and saves the answers.

Press *Start guided tuning* in *Settings → Cooking → Temperature Control & Learning*. The run then needs nobody:

1. It starts the grill and asks for the lowest set point (180 °F by default).
2. When the pit has reached that set point and held within a few degrees for ninety seconds, it runs the ordinary relay test: the feed oscillates gently around the target until seven crossings have been counted, which gives the ultimate gain and period.
3. Tyreus-Luyben turns those into PB, Ti and Td, and the result is saved as an **anchor** for that set point.
4. It raises the set point to the next anchor and repeats, climbing so the grill never has to cool down.
5. When the last anchor is measured it shuts the grill down.

The anchors are `learning.tune_setpoints`, 180, 225, 350 and 450 °F by default, and a run takes a few hours. The grill should be empty. Anything that stops the grill, including the Stop button and any safety error, ends the run and keeps the anchors already measured. A set point that produces no usable measurement is skipped rather than wasting the rest of the run.

While holding, the daemon looks up the two anchors that bracket the current set point and interpolates PB, Ti and Td between them, clamping to the nearest anchor outside the measured range (`pf_learning_gains`). The controller prefers this schedule over anything it learned passively, and prefers passive learning over the configured numbers. The schedule survives reboots (kv namespace `learning`, key `anchors`) and is listed anchor by anchor on the same page.

The run drives the grill only through the ordinary command queue and reads only the published status, so it can do nothing a patient person with the web app could not do, and the Stop button always wins.

In the simulator (`tests/test_tuner.c`) a full run takes the mean holding error from 9.7 °F to 0.2 °F at 180 °F and from 13.3 °F to 0.2 °F at 225 °F, while 350 °F and 450 °F stay within a third of a degree.

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

The scale is bounded to 0.4–2.0, persisted, and pulled halfway back toward 1.0 whenever a fresh plant model arrives so the two mechanisms do not fight. Windows spent saturated at the minimum or maximum duty are ignored: a grill that cannot go any lower is not the loop's fault.

## What you see

* Home → *Controller* details and *Settings → Cooking → Temperature control & learning*: the tuning in use (`PB … Ti … Td … ×scale learned`), the feed-forward fit with example duties, the plant estimate, autotune results, recent observations, and *Reset learning*.
* Events: `TUNING_LEARNED` after a startup produced a model, `Autotune_Done` when a relay test finished.

In the simulator (`tests/test_learning.c`) three consecutive cooks at the same conditions go from an integrated error of ~20 500 °C·s to ~15 200 °C·s (−26 %), and the fitted model predicts more fuel on a cold day than a hot one.

## Limits (honest)

The passive plant fit is rough and the monitor is rule-based, not a model-predictive controller. The learning cannot see pellet brand or wind directly (pellet brand is stored with observations for a future per-brand offset). Until a guided tuning run has been done, every set point reuses the same PB/Ti/Td and only the feed-forward changes with set point; the guided run is what removes that limitation, and between anchors the schedule interpolates rather than measuring.
