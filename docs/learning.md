# How the grill learns

The default controller is **Adaptive** (`controllers/adaptive.c`). It is built so that a grill gets better with every cook without anyone touching a PID number, in four layers. Everything below is on by default, and one switch governs all of it: *Settings → Cooking → Hold Mode → Learning → Learn from cooks*. Beside it, *Use measured tuning* decides whether what autotune measured overrides the Proportional Band, Integral Time and Derivative Time typed on the same page.

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

### Conditioning the relay: three things a real run got wrong

A run on the real grill returned `Ku 0.065, Pu 432, amplitude ±5.7, sitting 3.1 °F from the set
point`, and its own log said why:

* **It started by feeding into a climbing pit.** The relay begins the moment the pit arrives, so the
  error is nearly zero and the *rate* is what matters; picking the first half from the sign of the
  error started it feeding on top of the momentum the pit already had. The first half lasted fifteen
  seconds and the first excursion reached eleven degrees against ±5.5 once settled. It now starts in
  the half that opposes the pit's motion.
* **A deliberate widening was undone by the next re-centring.** `widening to 0.165` followed
  immediately by `centre 0.231, swing ±0.131`: the sizer works from the room around the centre and
  knew nothing about why the swing had been grown. There is a floor now as well as a cap.
* **It finished off-centre.** High halves of 196 and 166 s against low halves of 256 s — a 41 %
  asymmetry — because the centre sat at 0.294 where the cycle's own load was 0.268. Being centred is
  now a condition of finishing, not something hoped for on the way, with the crossing limit still
  there to end a run that cannot manage it.

Those compound: a thin swing inflates the describing function (±5.7 °F against a 1.2 °C band is 8 %
of inflation on its own) and an off-centre cycle stretches the period and widens the swing again.

### The cycle has to have settled, and the test for that has to work

Three runs at 250 F on an unchanged grill returned ultimate gains of **0.0685, 0.0667 and 0.052** —
a 24 % slide in one direction, which is drift with no physical cause. Two faults, compounding:

* **The settling test could not fail.** `autotune_cycle(i)` is `halves[i] + halves[i-1]`, and the
  check compared `cycle(n-2)` against `cycle(n-1)` — two sums that **share a half-cycle**. Sums
  sharing one of their two terms agree almost whatever the grill is doing: on the run whose
  consecutive cycles were 498 s and 362 s, twenty-seven per cent apart, that pair came out four per
  cent apart. It now compares two **disjoint** cycles, which needs five crossings rather than four.
* **An unsettled result was filed anyway**, with "(still drifting)" appended to the message. A
  transient averaged into the library is worse than no answer, because it quietly widens the band
  every time. A run that never settles now fails and files nothing.

The crossing budget went from 14 half-cycles to 24 to make settling reachable: conditioning can
spend eight of them, and the settling test needs two consecutive cycles after that. It is the run's
time limit that is meant to end a hopeless run, not the crossing count ending a healthy one early.

### An uneven cycle is not a square wave

`4h/π` is the fundamental of a relay that spends half its period in each state. A pellet grill heats
faster than it cools, so it does not: this grill's cycles ran 39/61. For a two-level relay holding
its high level for a fraction γ of the period,

```
U₁ = (2/π)·(u_hi − u_lo)·sin(πγ)
```

which is exactly `4h/π` at γ = 0.5 and six per cent below it at 39/61. Using the even-split figure on
an uneven cycle overstates the drive the grill actually received, and so overstates its gain. It is
computed from the measured halves now.

### The relay's own answer

A relay test measures two numbers and two only: the ultimate gain `Ku` and the period `Pu` of the
limit cycle it drives the grill into. Both are read straight off the swing. The tuning comes from
those, by the rule written for exactly that measurement — **Tyreus-Luyben**, in
`pf_tuning_from_relay`:

    Kc = Ku / 2.2,   PB = 2.2 / Ku,   Ti = 2.2·Pu,   Td = Pu / 6.3

Tyreus-Luyben is the conservative of the classical relay rules — Ziegler-Nichols hunts on a process
as lag-dominant as a barrel of air — and it suits a controller whose feed-forward already carries
the steady load, so the integral only has to trim.

It used to go the long way round: `Ku` and `Pu` were turned into a three-parameter model and SIMC
designed from that. The model needs a static gain the relay cannot see, borrowed from the
feed-forward or from a startup rise, and then splits the measured phase lag between a time constant
and a dead time — and SIMC's band is proportional to that dead time. The split is decided almost
entirely by the period. Two runs on the same grill a day apart measured periods of 370 s and 603 s,
which became dead times of 99 s and 168 s and bands of 82 °F and 150 °F, while the relay's own rule
put the second run at 93 °F. A tuning that swings by nearly a factor of two because the limit cycle
was slower is not a measurement of the grill, and the number that moved was never one the relay had
measured.

The model is still identified and filed, because the controller looks ahead by the dead time and
the app shows the grill it measured, but it no longer decides the tuning:

    tau   = √((K·Ku)² − 1) / ω,     theta = (π − atan(ω·tau)) / ω,     ω = 2π/Pu

with `K` from the feed-forward fit (`K = 1/b`) or the last cold startup rise.

### The swing has to sit on the set point

Everything above is only about the grill if the limit cycle is centred on the temperature being
measured. Feed a little too much at the centre and the pit lives above the set point, coming down
only on the low half: the halves stop being equal, the period stretches and the swing widens, all
of which the describing function reads as a grill that answers feed weakly. A real run did exactly
this — 20 minutes above 250 °F against 8 below, peaks of +12.9 and −5.7 °F — and came back with
150 °F where 82 °F had been holding the same grill.

The correction needs nothing new. **Over one full cycle the average feed delivered is the load the
grill needs at that temperature**, whatever the centre was set to, so after each completed cycle the
centre moves to that average: the first correction whole, later ones capped at half the swing so a
single noisy cycle cannot move the experiment far. Each correction costs two more cycles before the
result may be read, so what is measured always comes from a settled, centred relay. From a
deliberately overfed start the simulator now holds its swing on 248.9 °F of a 250 °F set point,
three minutes either side, where the same test off centre sat five degrees high.

The swing is symmetric — the same step up as down. Stepping up harder was tried, to get more
authority on a grill holding near its minimum feed, and it costs precisely the property this
depends on: an uneven relay makes an uneven cycle, whose mean sits off the set point even when the
centre is exactly the load. A low set point with little room below therefore gets a small, slow
swing, which is the honest price; slow is recoverable, biased is not.

Two more things the measurement must not depend on. The grill's model is fitted **only from a cold
start**, because the two-point method reads a step response and a step begins at rest: lighting a
barrel still hot from the last cook fits the tail of that cook as though it were the whole rise.
And a baseline run does not clear the steady-state observations, which measure how much fuel the
grill burns to hold a temperature — a new proportional band does not change that, and the next run
needs them to settle onto its operating point before the relay starts.

Rules written against how far the grill is from its target stay quiet while a measurement runs. The relay is deliberately driving the pit either side of the set point, so "running hot" would be reporting the tuner's own doing several times per set point. The pit temperature itself is still a fact and still testable. It drives the grill only through the ordinary command queue and reads only the published status, so it can do nothing a patient person with the web app could not do, and Stop always wins. The grill should be empty, and a run will not start while one is cooking.

*Settings → Cooking → Temperature Control & Learning → Autotune* offers two:

* **Baseline** visits every temperature in `learning.tune_setpoints` (250 °F alone by default) and
  **refines** what is already in the library. It does not erase anything.

  A relay test measures the grill on one afternoon, with that day's wind and that hopper's pellets,
  so a single run carries that day's noise with it. A repeat measurement at a set point already in
  the library therefore moves it part of the way rather than replacing it: half on the second run,
  a third on the third, and a quarter from the fourth onwards. Successive runs average the noise
  out. The quarter is a floor, not a decay to nothing — a grill that has been re-gasketed, rebuilt
  or is burning a different pellet has genuinely changed, and a library that kept averaging in
  years of old evidence could never follow it. Each anchor records how many runs are behind it.

* **Start From Scratch** is the only thing that erases the library, and it is a separate red
  button rather than a side effect of running a tune. It is for a grill that has changed, not for
  taking another measurement. Back the library up first (**Back Up** on the same page).

  **The baseline is measured first, and it is not the bottom of the range.** A grill holds 180 °F on
  very little fuel — close enough to the minimum feed that the relay has almost no room to swing
  below its centre. The swing gets clamped on one side, and the describing function behind the
  result assumes a symmetric square wave, so a lopsided one reports an ultimate gain that is too
  high. Starting there meant the least trustworthy of the four measurements was the one setting the
  grill's baseline and seeding every set point after it. Near 250 °F (`learning.tune_baseline`, or
  whichever configured point is closest to it) there is real room either side, so that one is
  measured first and the rest follow upward. Once it is stored, the daemon interpolates the library
  for whatever set point is being held and the controller prefers that over its own configured
  proportional band — so the remaining points, and any cook after an interrupted run, are governed
  by a measurement of this grill rather than by the untuned numbers someone typed in.

  This costs exactly one downward step, the shortest in the run. Cooling is passive, so the settle
  allowance follows how far the grill must travel and which way: roughly five degrees a minute
  climbing, one and a half cooling, plus an hour to settle once it arrives.

  **What ships is the baseline on its own.** Four set points is most of a day of the grill's time
  and a good part of a hopper, and most of that is spent walking between temperatures rather than
  measuring. The gain schedule clamps outside its anchors, so a single honest anchor at 250 °F
  governs the whole range sensibly, and the temperatures that matter for a particular cook are
  worth adding one at a time with **One Temperature** when that cook comes up. Adding more to
  `learning.tune_setpoints` restores the multi-point walk for anyone who wants it.
* **One Temperature** tunes a single temperature you pick between 180 and 450 °F and **adds** it to the library beside what is already there. About an hour. Use it for a temperature the profile does not cover, or one that has drifted.

Every entry records the outdoor temperature and wind it was measured in, from the ambient probe or the local weather, and the app shows them: the same grill behaves differently on a still summer afternoon than in a winter wind, and an entry means little without the conditions behind it.

While holding, the daemon looks up the two entries that bracket the current set point and interpolates PB, Ti and Td between them, clamping to the nearest entry outside the measured range (`pf_learning_gains`). The controller prefers this over anything it learned passively and over the configured numbers, and its note says `tuned` when it is following the library. From there the ordinary learning carries on: the feed-forward keeps fitting how much fuel this grill needs, and the performance monitor keeps nudging the loop gain from how each cook actually behaves, so a tune is a starting point that keeps improving rather than a fixed answer.

A run that a restart interrupts is noticed at the next boot and reported, and whatever it had already measured is kept. The library survives reboots (kv namespace `learning`, key `anchors`) and is cleared by *Reset learning*.

In the simulator (`tests/test_tuner.c`) a full profile takes the mean holding error at 180 °F from 4.3 °F to 0.2 °F, with 225, 350 and 450 °F all inside half a degree; the same test asserts that the controller is actually running on the library rather than merely storing it, and that a single-temperature run adds to the library while a baseline run refines it. Another
test walks the refinement itself: two runs at 40 and 60 give 50, a third at 80 gives 60, and a
grill whose real answer has moved to 100 converges there rather than being averaged into
irrelevance.

## 1. Feed-forward: how much fuel this grill needs (`features/learning.c`)

While holding a temperature, whenever the pit has been within 3 °C of the set point for three minutes with no lid event or set-point change, the daemon records an observation: set point, ambient temperature, the mean auger duty it took, and the pit noise (a wind proxy). Observations are fitted with a recency-weighted ridge regression

    u_ff = a + b · (setpoint − ambient)

which is the physics of a pellet grill (heat loss is proportional to the temperature difference), so a cook on a 30 °F day and one on a 100 °F day both inform the same two numbers instead of needing separate tables. Windows where the auger sat at its minimum duty are excluded (they say nothing about demand). Newer cooks weigh more (`learning.half_life_obs`), and a prior keeps the fit sane until real data exists. The controller adds only a gentle correction on top of `u_ff`, which is why it settles fast and does not hunt.

## 2. Plant model: how this grill responds (`control.c` `learn_rise_track`)

Two kinds of capture are treated as step tests, and both are fitted the same way:

* **the rise from cold** that follows a light, provided the grill really was cold (within 30 °F of ambient and under 150 °F — a warm light gives the tail of somebody else's step); and
* **every step up between held set points**, once the pit has been at the previous one for five minutes. This is what gives a tuning profile, which walks deliberately up through the anchors, a model at each of them rather than only at the first.

The fit is a least-squares search over the whole capture: a grid of dead times (0–240 s) and time constants (180–2400 s), with the gain and offset solved by linear regression at each grid point, keeping the model with the smallest residual. The 28 %/63 % two-point method it replaces took the set point as the step's final value, which it is not — the pit only arrives there because the controller backs the feed off — so it understated the gain and roughly halved the time constant. On one real grill it returned τ = 534 s where fitting four of that grill's own cooks properly gives 930–1110 s at a 4 °F residual.

A fit is refused rather than believed when it is not identifiable: if the search runs to the end of the grid, if the feed barely moved during the capture (< 0.15 duty), or if the residual exceeds 15 °F. A wrong model is worse than the previous one, because everything downstream trusts it. This is also why the capture is not fitted the moment the pit arrives: a rise that stops at the set point is still on the steep part of the curve and pins down only the ratio K/τ. The fit waits ten minutes into the hold that follows, where the feed settles to whatever balances the losses and fixes the static gain outright.

### The relay measures the plant; the capture measures only its time constant

A capture fitted on its own gives all three of K, τ and θ, and it is poor at two of them: θ and τ
trade off against each other, and the search happily returns a short dead time with a long lag. On
this grill's own baseline run the capture returned θ = 15 s and K = 466 °C per unit feed where four
of its real cooks fit 70–80 s and 313–348.

A relay test does not have that problem. It locates one point of the frequency response exactly —
the frequency where the phase reaches −π, and the gain there — and for a first-order-plus-dead-time
plant that fixes θ and K outright, given τ:

```
ω = 2π/Pu        θ = (π − atan(ωτ)) / ω        K = √(1 + (ωτ)²) / Ku
```

τ is the one thing the relay cannot see, because it never waits for the pit to finish arriving
anywhere; the capture into the set point measures exactly that. So each measurement supplies the
half it is good at. On the run above that gives θ = 97 s and K = 336 °C per unit feed — both inside
the range the real cooks fit. It matters because the Smith prediction scales as `K·θ/τ`: taking the
whole plant from the capture made the prediction about five times too small, and a prediction that
small lets the loop feed straight through the dead time and sail past the set point, which is the
one thing it is there to stop.

θ always lands between Pu/4 and Pu/2 whatever τ is, because atan is bounded, so the relay brackets
the dead time on its own and a poor τ can only move it inside that bracket.

The model is filed **against the set point it was taken at**, in the same tuning-library entry that holds that temperature's PB/Ti/Td, and the controller is handed the model for whatever set point it is holding, interpolated between entries exactly as the gains are. A pellet grill is a different plant at 180 °F than at 450 °F, and one model stretched across the range mis-states how much fuel is on its way at both ends. When the library has no entry yet, the most recent fit is used for everything.

Only the **rise from cold** is allowed to design gains from its model: adaptive derives PB/Ti/Td from it with the SIMC rules (τ_c = θ), blends them with what it had, and keeps them within 0.5–1.5× the configured baseline because the passive fit is deliberately crude. A step between set points measures the grill and nothing else — designing gains from it too would have every set point change in an ordinary cook quietly re-tune the loop. The relay **autotune** (*Settings → Cooking → Hold Mode → Autotune*, run without food) designs from measured Ku/Pu and is trusted over a wider band (0.33–3×). Learned gains are persisted per controller and restored on the next boot.

## Approach without overshoot: the prediction (in the controller)

A pellet grill answers a change in feed about seventy-five seconds later and then keeps moving for several minutes. A loop that watches only the thermometer is therefore always acting on news that is out of date, and drives the fire hard right up to the moment the pit arrives — which is exactly when the fuel already in the pot is about to carry it past. Capturing 250 °F from cold used to overshoot by more than 20 °F for this reason.

The controller runs a **Smith predictor** (Smith, 1957). Alongside the real grill it runs the model above, driven by the same feed, and acts on

```
error = (pit − set point) + K · [ x(t) − x(t − θ) ]
```

where `x` is the model's normalised response to the feed. The bracket is the rise that is committed and has not been felt yet: it is zero whenever the feed has been steady, so the loop still settles exactly on the set point and the prediction costs nothing at a hold. It goes large during a hard climb, which is precisely when the loop needs to stop feeding. The tuning note shows `· holding back` while the prediction is pulling the feed down.

The model is kept in normalised form and seeded from the feed currently being applied, with an empty delay line, so it predicts nothing until the feed moves. Both matter: started from zero it spends a whole time constant climbing to meet the duty and reads that climb out as a rise on its way to the pit, which starves a fire that is doing nothing of the kind and settles the pit ten degrees high; and keeping the state normalised means a fresh plant model — a new fit, or the library handing over the one measured at this set point — changes K and τ without jolting anything.

Each term acts on the signal it is there for: **P and I on the prediction**, because banking every second of a half-hour climb as a deficit to be repaid is the wind-up that throws the pit past the target; **D on the pit's own slope**; and the performance monitor and the integral's wind-down guards on the **real** error, because a pit sitting on its set point with fuel still on its way is not an error the grill is making.

Alongside it:

* **Integrator trim on arrival** — whatever the integrator accumulated while the pit was more than 15 °F away is approach wind-up, not a steady-state correction; on entering the ±15 °F band it is clipped to ±0.15 duty (never reset to zero, so the pit does not sag).

The plant model's clock starts when the fire is evidently lit (+3 °C over the startup baseline), so the ignition delay no longer inflates θ.

In the simulator, capturing 250 °F from cold overshoots by **+2 to +4 °F** where the same grill without the prediction went **+23 °F**, and a 250 → 300 °F step by **+1 to +8 °F** where it went **+20 °F**, settling within a degree either way.

Two more rules came from a cook that sat 20–35 °C *under* a 350 °F target for its whole length:

* **Ambient must be plausible** — the feed-forward is `b · (setpoint − ambient)`, and that cook had restarted Startup on a grill that was still at 166 °C, so "ambient" was taken from the pit (129 °C) and the feed-forward came out at 0.19 duty instead of the 0.62 the grill needed. Only a reading that could be outdoor air (≤ 50 °C) is accepted, from the pit at Startup, the cold-start baseline or an ambient-flagged probe; otherwise the last plausible value (persisted across restarts) is used, and 20 °C before any exists.
* **Integrator seeding** — entering Hold far from the target no longer seeds the integrator "bumplessly" from the previous duty (that duty was the smoke cycle or the `u_min` placeholder, and the seed parked a −0.3 duty integral that unwound at Ti = 286 s, holding the feed back for many minutes). Far from the target the integrator starts at zero; within ±15 °F it is seeded bumplessly, capped at ±0.15 duty. The integral also never opposes a large error: a negative integral while the pit is far below target (or positive far above) is cleared.

## Time to the set point, before the climb starts

When the set point changes the app shows an estimate of how long the pit will take to get there.
It is asked at the moment of the change, so there is no climb yet to fit a line through; it comes
instead from the two things the grill teaches itself.

The feed-forward fit says what duty holds what temperature, `u = a + b·(setpoint − ambient)`. Read
backwards it says the opposite — the temperature a given duty would eventually hold — and at full
feed that is where the pit is heading. The plant model says how fast it gets there and how long
before it starts:

```
T∞ = ambient + (u_max − a) / b
t  = θ + τ · ln((T∞ − T₀) / (T∞ − T₁))
```

Both halves improve with every cook: the feed-forward gains an observation every five minutes of
steady holding, and the plant is re-measured on every step between set points, so the estimate
sharpens as the grill learns. Before either exists there is nothing honest to say, and it says
nothing rather than inventing a number — as it also does for a temperature this grill cannot reach,
or for a set point below where the pit already is, since cooling is not a climb the fire controls.

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

* Home → *Controller* details and *Settings → Cooking → Hold Mode*: the tuning in use (`PB … Ti … Td …`, in degrees, which is the tuning actually running), the feed-forward fit with example duties, the plant estimate, autotune results, recent observations, *Clear learning* and *Erase everything*.
* Events: `TUNING_LEARNED` after a startup produced a model, `Autotune_Done` when a relay test finished.

In the simulator (`tests/test_learning.c`) three consecutive cooks at the same conditions go from an integrated error of ~20 500 °C·s to ~15 200 °C·s (−26 %), and the fitted model predicts more fuel on a cold day than a hot one.

## Limits (honest)

The passive plant fit is rough and the monitor is rule-based, not a model-predictive controller. The learning cannot see pellet brand or wind directly (pellet brand is stored with observations for a future per-brand offset). Until an autotune run has been done, every set point reuses the same PB/Ti/Td and only the feed-forward changes with set point; the tuning library is what removes that limitation, and between its entries the daemon interpolates rather than measuring.
