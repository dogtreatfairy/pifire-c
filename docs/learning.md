# How the grill learns

The default controller is **Adaptive** (`controllers/adaptive.c`). It is built so that a grill gets better with every cook without anyone touching a PID number, in three layers. Everything below is on by default; *Settings → Grill → Learning* and the controller's own *Learn tuning automatically* switch turn parts of it off.

## 1. Feed-forward: how much fuel this grill needs (`features/learning.c`)

While holding a temperature, whenever the pit has been within 3 °C of the set point for three minutes with no lid event or set-point change, the daemon records an observation: set point, ambient temperature, the mean auger duty it took, and the pit noise (a wind proxy). Observations are fitted with a recency-weighted ridge regression

    u_ff = a + b · (setpoint − ambient)

which is the physics of a pellet grill (heat loss is proportional to the temperature difference), so a cook on a 30 °F day and one on a 100 °F day both inform the same two numbers instead of needing separate tables. Windows where the auger sat at its minimum duty are excluded (they say nothing about demand). Newer cooks weigh more (`learning.half_life_obs`), and a prior keeps the fit sane until real data exists. The controller adds only a gentle correction on top of `u_ff`, which is why it settles fast and does not hunt.

## 2. Plant model: how this grill responds (`control.c` `learn_rise_track`)

Every startup that runs into Hold is treated as a step test. The rise from the initial pit temperature to the set point is fitted as a first-order-plus-dead-time model (K, τ, θ) using the 28 %/63 % two-point method; results are averaged with the previous estimate and stored. Nothing is injected into the cook to get this.

As soon as a new model exists (`learning.auto_tune`), the daemon hands it to the controller. Adaptive derives PB/Ti/Td from it with the SIMC rules (τ_c = θ), blends them with what it had, and keeps them within 0.5–1.5× the configured baseline because the passive fit is deliberately crude (dead time absorbs the ignition delay). The relay **autotune** (*More → Learning*, run without food) does the same with measured Ku/Pu and is trusted over a wider band (0.33–3×). Learned gains are persisted per controller and restored on the next boot.

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

* Home → *Controller* details and *More → Learning*: the tuning in use (`PB … Ti … Td … ×scale learned`), the feed-forward fit with example duties, the plant estimate, autotune results, recent observations, and *Reset learning*.
* Events: `TUNING_LEARNED` after a startup produced a model, `Autotune_Done` when a relay test finished.

In the simulator (`tests/test_learning.c`) three consecutive cooks at the same conditions go from an integrated error of ~20 500 °C·s to ~15 200 °C·s (−26 %), and the fitted model predicts more fuel on a cold day than a hot one.

## Limits (honest)

The passive plant fit is rough and the monitor is rule-based, not a model-predictive controller. The learning cannot see pellet brand or wind directly (pellet brand is stored with observations for a future per-brand offset). Very different set points reuse the same PB/Ti/Td; only the feed-forward changes with set point.
