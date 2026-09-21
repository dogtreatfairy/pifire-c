# Conditional Notifications — design

A rule engine in the daemon that turns grill state into notifications, edited from the web app the
way Node-RED and Home Assistant let you build a condition: pick a thing, pick one of its traits,
pick a test, optionally require it to hold for a while, then write the message that goes out.

Today's notifications are hard-coded in C (`Probe_Temp_Achieved`, `Probe_ETA`, `Pellet_Level_Low`,
the `E0x` errors). This design keeps every one of them working by shipping them as built-in rules,
so what was fixed behaviour becomes something you can read, edit and switch off.

Status: **built.** The engine is `src/features/rules.c` with `tests/test_rules.c`; the editor is
`web/pages/rules.js` under *Settings → Notifications → Conditional Notifications*. The built-in
rules ship in the defaults and a schema-5 migration carries the old predictive setting across.

---

## 1. Entities and traits

Everything testable is an *entity* with named *traits*. The daemon publishes the catalogue at
`GET /api/v1/rules/entities` and the editor builds its dropdowns from it, so adding a trait in C
makes it selectable in the UI with no web change — the same trick the hardware wizard uses with
`manifest.json`.

| Entity class | Instances | Traits |
|---|---|---|
| `grill` | one | `mode`, `temp`, `setpoint`, `error`, `cook_elapsed`, `mode_remaining`, `rate` (°/min), `lid_open` |
| `probe` | every configured probe, by name | `temp`, `target`, `eta`, `over_target`, `valid`, `connected`, `battery`, `signal`, `rssi` |
| `output` | `auger`, `fan`, `igniter`, `power` | `state` (on/off), `percent` (fan) |
| `hopper` | one | `level`, `brand` |
| `controller` | one | `duty`, `feedforward`, `error`, `p`, `i`, `d` |
| `weather` | one | `temp`, `wind`, `humidity` |
| `system` | one | `wifi_signal`, `tailscale_online`, `cpu_temp`, `throttled`, `uptime` |
| `timer` | one | `running`, `remaining` |

Each trait carries its type (`number`, `temperature`, `duration`, `percent`, `bool`, `enum`), its
unit, and the operators that make sense for it. A `temperature` trait is stored in °C and displayed
in the user's units; rules store °C so switching units never rewrites a rule.

### Selecting instances: one, any, or every

The scope of a rule is either a **specific instance** (`probe:BT1`) or a **class**:

* `any food probe` — evaluate every food probe independently and fire **once per probe** that
  matches. This is what makes `{probe} reached {target}` work as a single rule covering all probes.
* `every food probe` — fire once, only when *all* of them match (useful for "everything is done").

Class rules keep their state per instance, so BT1 firing does not arm or silence BT2.

---

## 2. Conditions

A rule holds a list of conditions joined by AND, and optionally one group joined by OR. Each
condition reads:

```
<entity>.<trait>  <operator>  <value>  [for <duration>]
```

Operators, by trait type:

| Kind | Operators |
|---|---|
| number / temperature / percent / duration | `>`, `>=`, `<`, `<=`, `between`, `rises above`, `drops below`, `is within ± of` |
| bool | `is on`, `is off`, `turns on`, `turns off` |
| enum (mode) | `is`, `is not`, `changes to` |
| any | `is unavailable`, `becomes available` |

`value` is a literal **or another trait of the same entity**. That is what lets one class rule say
"temp is at or above its own target" — `probe.temp >= probe.target` — without naming a number.

**`for <duration>`** requires the condition to hold continuously before the rule fires, which is the
"battery under 20 % for 10 seconds" case and also debounces noisy readings.

`rises above` / `drops below` are edge operators: they need the previous sample on the other side of
the threshold, so a probe that starts a cook already above its target does not fire immediately.

---

## 3. When a rule actually fires

A rule fires on the **rising edge of the whole condition set**, after any hold time. It will not
fire again until the set goes false. That is the Node-RED trigger semantic and it is what stops a
"temp ≥ target" rule from sending one notification per second for the rest of the cook.

Three knobs sit on top:

* **Deadband** — the set must go false by a margin before the rule re-arms, so a probe hovering on
  its target does not chatter.
* **Cooldown** — a minimum gap between two firings of the same rule and instance.
* **Repeat** — optionally re-send every N minutes *while the condition stays true* ("keep nagging me
  that the hopper is low"). Off by default.

Plus one guard: **only while cooking**, on by default, so charging a probe on the bench or a Wi-Fi
blip overnight does not page you.

State per (rule, instance) is small — `held_since`, `armed`, `last_fired` — and bounded.

---

## 4. Message templates

Title and body are templates with `{token}` substitution, resolved against the matched instance
first and the grill second, so a class rule names the probe that actually matched.

| Token | Renders |
|---|---|
| `{probe}` | the matched probe's name, e.g. `BT1` |
| `{temp}` `{target}` `{over}` | temperatures in the user's units with the degree sign |
| `{eta}` `{eta_min}` | `1h 22m` / `82` |
| `{battery}` `{signal}` | `36%` / `3 of 4` |
| `{grill}` `{grill_temp}` `{setpoint}` `{mode}` | grill name, pit temperature, target, mode |
| `{hopper}` `{outdoor_temp}` `{cook_time}` | `21%`, `74°F`, `3:42` |
| `{value}` | the value that satisfied the condition, whatever trait it was |
| `{time}` | local time of firing |

`{{` is a literal brace. An unknown token renders as `—` rather than failing, so a half-finished
template never blocks a notification. The editor offers the valid tokens for the chosen scope as
tappable chips, and shows a live preview rendered against current readings.

The user's two examples become, verbatim:

```
{probe} reached {target}
{probe} — 1 min to {target}
```

---

## 5. Criticality

Four levels, mapped onto what each service actually supports:

| Level | Pushover | ntfy | In-app |
|---|---|---|---|
| Info | −1 (no sound) | 2 | banner, auto-dismiss |
| Normal | 0 | 3 | banner + centre |
| High | 1 (bypasses Pushover quiet hours) | 4 | sticky banner + centre |
| Critical | 2 (repeats until acknowledged, retry 60 s / expire 30 min) | 5 + `rotating_light` | sticky red banner, centre, vibrate |

**Honest note on "critical alerts".** An alert that pierces silent mode and Focus on iOS is an Apple
entitlement held by the *receiving* app, not something PiFire can set from outside. What we can do
is send the highest priority each service exposes, which is Pushover's Emergency (priority 2) and
ntfy's max. Whether that breaks through a Focus mode depends on allowing the Pushover or ntfy app
as Time Sensitive, or enabling its Critical Alerts switch, in the phone's own notification settings.
The design should say this in the UI next to the Critical option rather than implying more.

Each rule also chooses its sinks: Pushover, ntfy, in-app only, or MQTT/webhook for automation.

---

## 6. Storage

Rules live in `settings.notify.rules[]` so they are covered by the existing backup and migration
machinery.

```json
{
  "id": "probe-done",
  "name": "Probe reached target",
  "enabled": true,
  "scope": { "class": "probe", "role": "food", "match": "any" },
  "when": [
    { "trait": "target", "op": ">", "value": 0 },
    { "trait": "temp", "op": ">=", "value": { "trait": "target" }, "for_s": 0 }
  ],
  "title": "{probe} reached {target}",
  "body": "{probe} is at {temp} after {cook_time}.",
  "level": "high",
  "sinks": ["pushover", "app"],
  "deadband": 2.0,
  "cooldown_s": 600,
  "repeat_s": 0,
  "only_while_cooking": true
}
```

---

## 7. Engine

New `src/features/rules.c`, evaluated once a second from the services thread against a snapshot
built from `pf_status`, the sensor snapshot, weather and the network block — all of which the status
publisher already assembles.

1. Build the entity snapshot.
2. For each enabled rule, resolve the instances in scope.
3. Evaluate the conditions per instance, tracking `held_since` for `for` durations and the previous
   sample for edge operators.
4. Apply edge, deadband, cooldown and repeat.
5. Render title and body, then emit.

Emission goes through the existing event bus so the notification centre, MQTT and webhooks all see
it, but the bus needs to carry more than it does today. `pf_events_emit()` gains a sibling that also
takes a level and a sink mask:

```c
void pf_events_emit_ex(const char *code, int level, unsigned sinks,
                       const char *title, const char *fmt, ...);
```

`push.c` then routes on the mask and level instead of inferring a category from the event code,
which also cleans up the current `category()` string matching.

---

## 8. Web UI

**Settings → Notifications → Conditional Notifications**, a collapsible group holding a table:

```
┌────────────────────────────────────────────────┐
│ Conditional Notifications              [+ Add] │
├────────────────────────────────────────────────┤
│ Probe Reached Target                    HIGH ⬤ │
│ Any food probe · temp ≥ its target             │
├────────────────────────────────────────────────┤
│ Almost There                          NORMAL ⬤ │
│ Any food probe · time to target ≤ 15 min       │
├────────────────────────────────────────────────┤
│ Probe Battery Low                       INFO ⬤ │
│ Any probe · battery < 20% for 10s              │
└────────────────────────────────────────────────┘
```

Each row shows the rule name, a generated plain-English summary of its conditions, its level, and an
enable switch. Tapping opens the editor:

* **Name**
* **When** — condition rows of `[entity ▾] [trait ▾] [operator ▾] [value] [for __ s]`, with *Add
  condition*; the value field turns into a trait picker when you choose "another trait"
* **Message** — title and body, with token chips underneath and a live preview
* **Level** — segmented Info / Normal / High / Critical, with the note about phone settings
* **Send to** — Pushover / ntfy / app only
* **Advanced** — cooldown, repeat, only while cooking, deadband
* **Send Test** — renders against live values and sends it now, so you can confirm the phone side
  before a cook rather than during one

## 9. API

| Route | Purpose |
|---|---|
| `GET /api/v1/rules/entities` | the catalogue: entities, instances, traits, types, operators, tokens |
| `GET /api/v1/rules` | the rules plus live state (armed, held since, last fired) |
| `PUT /api/v1/rules` | replace the rule list (validated; a bad rule is rejected with a reason) |
| `POST /api/v1/rules/test` | render and send one rule immediately |

## 10. Built-in rules

Shipped as ordinary, editable rules so the current behaviour is visible rather than magic:

| Rule | Condition | Level |
|---|---|---|
| Probe Reached Target | any food probe, `temp >= target` | High |
| Almost There | any food probe, `eta <= 15 min`, held 40 s | Normal |
| Probe Went Offline | any Bluetooth probe, `connected is off` for 60 s | High |
| Probe Battery Low | any Bluetooth probe, `battery < 20 %` for 10 s | Info |

Grill errors and the low-pellet warning stay native for now: the `E0x` events already carry
critical urgency and reach every sink, and the hopper warning has its own repeat interval in
settings, so turning either into a rule would double up rather than migrate. They move once the
editor exists and the settings can move with them.

For "Probe Reached Target" to be a rule at all, the notify engine had to stop clearing a probe's
target the moment it is met: a rule compares `temp` against `target`, so both have to stay visible.
A `reached` latch now does that job, the after-action (keep warm, shutdown) still fires exactly
once, and the probe card keeps showing what it was aiming at.

The schema migration converts the existing `notify.eta_warn_min` into the "Almost There" rule's
threshold, so nobody loses a setting they had configured.

## 11. Phasing

1. **Engine and schema** — catalogue, evaluation, templates, `pf_events_emit_ex`, built-in rules
   and the migration. Behaviour identical to today, now driven by rules.
2. **Editor** — the table and the condition editor, token chips, live preview, Send Test.
3. **Class rules and per-instance firing** — "any food probe" fanning out correctly.
4. **Edge, hold and repeat** — `for`, `rises above`, deadband, repeat.
5. **Criticality plumbing** — per-rule sinks and levels through `push.c`.

Steps 1 and 2 are the useful half; 3 to 5 refine it.
