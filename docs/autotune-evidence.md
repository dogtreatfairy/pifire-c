# Autotune: what was measured, and why it is built the way it is

Kept as evidence, so the next change to the tuner is argued against numbers rather than against a
feeling. Reproduce the table with `./build/eval_rules` (built with the tests; not a ctest because it
takes minutes). The simulator is set to this grill's own plant -- pit time constant 1471 s, dead time
89 s, fitted from its cook files -- and the relay measured on it Ku 0.056, Pu 587 s, load 0.30,
which is what the real grill returned (Ku 0.054, Pu ~460-500 s uncorrected, load 0.25-0.29).

## The finding that mattered

With the feed-forward taken from the library's measured load, **every tuning rule holds the same**:

| rule (PB / Ti / Td)                 | arrival +F | hold IAE F-min | hold rms F | lid dip F | recover s |
|-------------------------------------|-----------:|---------------:|-----------:|----------:|----------:|
| Tyreus-Luyben (70 / 1292 / 93)      | 3.2 | 48 | 1.36 | -18.1 | 370 |
| TL with Ti = Pu (70 / 587 / 73)     | 3.1 | 46 | 1.34 | -17.8 | 380 |
| ZN some-overshoot (96 / 294 / 196)  | 3.1 | 47 | 1.33 | -18.0 | 370 |
| Cohen-Coon (26 / 214 / 32)          | 3.1 | 46 | 1.32 | -18.3 | 370 |

and with the feed-forward pushed a quarter high they degrade together (arrival 6.6-7.1 F, IAE
94-100). The gains were never what decided the hold on this controller. What decided it was the
feed-forward: the built-in prior said 0.37 duty at 250 F where the relay measured 0.25, so every
arrival over-fed by two fifths. The passive learner that should have corrected that needs a calm
quarter hour, which a tuning run never gives it -- the grill had been tuned nine times and was still
running the prior. The relay's measured load now IS the feed-forward, interpolated across the set
points it has been measured at, and is filed as a learning observation besides.

Tyreus-Luyben stays the rule. It is the most robust of the four and the table shows nothing to be
gained by leaving it; `pf_tuning_rule_selected` exists so this can be checked again.

## What each guard is for

- **Start from the known load, or a settled hold, or not at all.** A relay centred on the
  controller's arrival transient (90 s inside the band) began at 0.328 against a load of 0.22:
  +9 F, then -8 F, then two centrings. The profile now waits five minutes; a set point measured
  before starts from its own load.
- **Swing no wider than half the load.** +/-0.15 on 0.22 nearly doubled the fire on the high half.
- **Centrings while converging, not two and stop.** The pot relights slower than it starves, so
  the first centring lands past the answer as often as short of it; a run whose steps shrink may
  keep going, one whose steps grow is stopped.
- **The measured point is carried to the crossing.** With hysteresis 1.2 C on a 3.3 C swing the
  relay identifies 22 degrees short of -180: a period 24% long, a gain 22% short, handed to a rule
  that scales Ti and Td with the period. Corrected exactly for a first-order lag plus dead time.
- **The controller comes back on the measured load.** Its bumpless reset used to inherit the
  relay's last half-cycle as if it were steady state.
- **Every tune is verified before it is kept.** A 25-minute hold under the new tune; worse than
  8 F peak or 3 F rms and it is taken back, library and all, and the finishing message says so.
  In the tests it refused exactly one hold: the one built on an impossible fixture.

## The first run that finished cleanly on the real grill (26 September 2026, 250 F)

Baseline tune, hopper at 11%, ambient 53 F, wind 8 km/h. The typed tuning (PB 80, Ti 400, Td 30)
hunted about 9 F either side of the set point, so the relay could not start until the hold had
settled five minutes: 22 minutes after Startup, centred on the hold's own feed, 0.263.

Ten crossings in 34 minutes. Halves 90, 166, 196, 196, 376, 151, 211, 256, 226, 196 s; pit
117-127 C about 121.1. Centre 0.263 -> 0.306 -> 0.242 -> 0.255. The step to 0.306 was the
truncated-first-half defect (fixed in alpha.121): the run began with the pit 4.7 C over and
falling, so the first low half was the 90 s left of that descent, and 90 s low + 166 s high read
as a cycle said 0.306. The 376 s half was the grill: the same low feed as the half before but half
the cooling rate, in the ten minutes the hopper went from 11% to 19%.

Filed: relay |1/G| 0.048 at 457 s with 21 deg of hysteresis phase, ultimate point Ku 0.062 at
Pu 357 s along the FOPDT curve (tau 1700 s); Tyreus-Luyben PB 64 F, Ti 786 s, Td 57 s; load
0.255. Verification hold: after the 8-minute skip, max +2.7 / -2.1 F, rms 1.1 F, mean +0.5 F,
mean feed 0.252 -- kept. The feed-forward the controller then ran on was 0.25, against the 0.38
it had been running on before, which is the number that had been holding this grill high.

## The static gain is measured, not inferred (alpha.128)

Ryan asked for the profile to include a set-point change -- heat to 250, stabilise, heat to 350,
stabilise -- and for the tuning to be calculated from all of it. The default profile is now
250 F then 350 F, a step up between two settled holds, and the static gain K comes from those
holds: (T2 - T1)/(load2 - load1), the one thing a relay cannot see, measured directly; with one
hold, (T - ambient)/load. With K known, the relay's point on the frequency response fixes the
time constant and the dead time outright (`pf_plant_from_relay` given K), and the step capture's
own time constant is logged beside it as a check rather than being the source of the gain. The
run before this one filed K = 512 C per unit at 250 F, derived from a captured tau of 1800 s;
the settled hold said 430 (121 C on 0.255 duty, ambient 11.5 C) and the startup fit 495. The
tuning rule itself is unchanged: Tyreus-Luyben from the relay, which the verification hold has
now passed on the real grill.


## The countdown to the set point, replayed over a real climb (27 September 2026)

The estimate on the gauge used to be recomputed from a standing start every second -- the dead
time re-added each time, the ceiling read from the feed-forward fit, the controller ignored -- and
Ryan called it "extremely erratic". The replacement runs the climb the tuning describes under the
adaptive controller's own law and reads the time left from where the real pit stands on that
curve, re-paced by the climb so far and handed over to the climb's own average rate as it goes.

It was checked by replaying the daemon's own estimator (`pf_learning_climb_eta`, linked from
`libpfcore.a`) over the recorded pit of cook 13's step from 250 to 300 °F, given at 20:31:50 with
the pit at 119.8 °C, using the 300 °F anchor as it stands in the library (K 432.8 °C per unit feed,
τ 1644 s, θ 91.4 s, PB 32.9 °C, ambient 10.8 °C, feed 0.1–0.9). The pit was within a degree of the
set point after 471 s.

| elapsed | pit °C | estimate | predicted arrival | error |
|---|---|---|---|---|
| 0 s | 119.8 | 753 s | 753 s | +282 s |
| 120 s | 125.9 | 497 s | 617 s | +146 s |
| 240 s | 139.9 | 152 s | 392 s | −79 s |
| 360 s | 143.7 | 105 s | 465 s | −6 s |

The modelled feed followed the real feed closely (0.9 for the dead time, then about 0.48, 0.40,
0.45, 0.37 as the controller held back), and the modelled pit was within 2 °C of the real one for
the first three and a half minutes. After that the real grill crept in faster than its static gain
says it should, which is why the hand-over to the climb's own pace is there: it is what brought
the last four minutes to within a minute. The early error is the plant's: this grill's step captures
and relay tests disagree about the time constant threefold (540 s against 1644 s, in the daemon's
own log), and no curve drawn from that can call the first minutes better than a few tens of percent.
What it no longer does is lurch: no reading in the replay moved the predicted arrival later by more
than a minute, and in the daemon the value is further smoothed over twenty seconds.

## The intercept at 225 F, and what was narrowing the band (27 September 2026)

Ryan: "my grill way overreacted to leveling off at 225, so the autotune is good for stabilizing
but doing a poor job of intercepting a temp setting." The daemon's history from the restart at
13:00 holds the approach that followed a fresh startup at 13:00:31 (feed and pit every fifteen
seconds; the set point was 225 F from 13:03:48):

| time | feed | pit F | |
|---|---|---|---|
| 13:04:03 | 0.90 | 115 | full feed from startup's end |
| 13:05:33 | 0.76 | 141 | the brake begins |
| 13:08:21 | 0.34 | 192 | |
| 13:10:21 | 0.15 | 216 | nine degrees short, still climbing: the brake takes the feed below the 0.24 that holds the pit |
| 13:10:36 | 0.15 | 218 | the peak of the first approach |
| 13:11:54 | 0.35 | 215 | the pit has fallen back three degrees; the loop opens up |
| 13:13:39 | 0.20 | 224 | the second approach arrives |
| 13:16:09 | 0.10 | 228 | and overshoots by three, on the momentum of the 0.37 feed two minutes earlier |
| 13:17:24 | 0.22 | 227 | then settles into a swing of ±3 F, period about five and a half minutes |

Two mechanisms, one of them a bug in the learning:

1. **The monitor read the climb as a sluggish hold.** The adaptive controller's monitor judges each
   ten-minute window of Hold and narrows the band when the pit sits far from the target with a
   free feed ("slow to reach target"). Its first window after Hold is entered from startup *is the
   climb* -- the pit far below, the feed free most of the time -- and nothing marked that climb as
   a step, so at 13:14:01 it logged `band 28.9 C around 107 C, refining 36.2 C (slow to reach
   target)`: the tune's 36.2 C band narrowed by the full 20% learning is allowed, on the first
   window of every cook. The band that had been tuned to hold was being replaced, every time, by
   one a fifth tighter, and the intercept and the hunting after it are what a fifth tighter looks
   like. Fixed: arriving in Hold far from the set point opens a step (so the step rules judge the
   arrival and the slowness rule stands down for twenty minutes), the slowness rule also needs the
   target to have been reached once, and bands learned under the old rules are dropped on load so
   the tune stands again.

2. **The brake short of the set point.** The derivative term took the feed to 0.15 at 216 F with
   the pit climbing at about 0.12 C/s; the feed that holds the pit there is about 0.24, so the pit
   fell back and the loop opened up, which is where the overshoot came from. A floor on the way up
   -- never below what holds the pit -- was tried, with the plant's static gain as the estimate,
   and overshot the simulator's capture by 18.6 F against the 8 F the test allows, for the reason
   the brake exists: a pit fed at its hold feed until it arrives carries a dead time's rise past
   the target. The brake stays. With the band back at the tuned width the brake is a fifth weaker
   too (Kd = Td / PB), which is the proportion the log shows it was over-braking by.
