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
