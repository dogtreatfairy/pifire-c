# The PiFire design language

One grill, two screens: a phone held at arm's length and a small panel bolted to a hot barrel and
turned with one knob. They are not the same medium and should not look identical, but a person
moving between them should never have to relearn anything. This document is what an interface
decision is checked against. If a question comes up that this does not answer, answer it here first
and then build it.

## The rule that is easiest to get wrong

**The dismissive action is on the left. The committing action is on the right.**

Back, Cancel and Close go left. Save, Set, Start, Solve and Confirm go right. This holds in web
dialogs, in panel screens, and in every row of two buttons anywhere in the project. On the panel the
knob turns clockwise through the row, so the last thing under your thumb is the one that commits,
which is also the one you are most likely to want after adjusting something.

A destructive action that is the point of the screen, like Clear target, is a commit and sits right.
A destructive action that is an aside, like Emergency Stop in a menu, is not part of a pair at all:
it is a row of its own, in red.

## Naming

Titles are Title Case. Menu rows name a **mode or an action**, never a sentence: "Hold Mode", not
"Hold at a temperature". Body text is plain sentences with ordinary capitalisation.

Never invent a name for something the user names. Probes are called whatever they were called in the
probe settings, everywhere they appear.

## Colour

| Meaning | Token | Used for |
|---|---|---|
| The grill is doing what you asked | `--ok` green | Hold, a probe that has reached its target |
| The grill is working towards it | `--accent` orange | Smoke, Startup, Prime, the primary button |
| Something is being done *to* the grill | `--info` blue | Shutdown, a tuning run |
| Attention, not yet a fault | `--warn` yellow | Manual mode, an unsaved change |
| A fault, or an action that ends something | `--danger` red | Error, Emergency Stop, End Cook |

A red menu row means it stops something. Nothing else is red.

**Red is for a fault, not for a number getting smaller.** A hopper at a quarter, a battery at a
fifth and a probe on one bar are all *attention* — amber — because each is still working and each
has time left. Red is for the state that has actually gone wrong: the hopper about to run out, the
battery about to die, the probe that has dropped off. Painting the ordinary low end of a range red
means the panel spends most of a long cook claiming a fault it does not have, which is the surest
way to teach someone to ignore the colour. Where a reading has a low end worth mentioning, it gets
two thresholds: amber first, red when it is genuinely nearly over.

**Identity is not status.** An icon that says *what* something is keeps its own colour whatever is
happening to it. The Bluetooth rune on a probe card means "this probe is wireless"; it was once
drawn in the signal-strength colour and so turned red whenever the probe was a room away, which
read as a fault on a probe that was working. The bars beside it carry the strength; the rune
carries the identity, and it is always Bluetooth blue.

## Numbers

Temperatures carry their unit. A number that changes while you watch it is sized from the widest
digits it could contain, never from the digits currently in it, or it changes size as the grill
warms through a boundary. Times are `1:15` under an hour and `1:02:30` above it. A measurement that
would fit in a sentence still goes on its own line or in a table, never inline in prose.

## The phone

An app shell: only `<main>` scrolls, the bars are pinned inside a viewport-fixed body, and the safe
area insets place them. Home fits one screen without scrolling. The mode lives in the header pill
and nowhere else. Settings is an index of grouped inset lists with icon tiles, one page per group,
never one long page. No menu item appears in two places: Settings is what you configure, More is
what you do and what you look at.

Sheets follow one shape: what it is about at the top with its current value, the choices in the
middle as full-width cards, and the commit row at the foot.

## The panel

An industrial HMI read at arm's length in sunlight, not a shrunken phone. A filled mode banner
across the top with exactly one clock in its right corner. Real anti-aliased type at the panel's
own resolution, never a scaled bitmap font. Status tiles are filled blocks with a single colour
each: FAN green, AUGER blue, IGN orange, in that order.

Every screen is pushed onto a navigation stack, so Back is always the same operation and a long
press unwinds to the main screen from anywhere. A value being edited is highlighted in the accent
colour; the same value merely selected is highlighted in the surface colour.

Margins exist because a bezel hides the edge of the panel. They are set from the panel, with the
screen in front of you, not from a number typed on a phone.

## Settings that can be judged by eye

Anything whose effect is visible on the panel — colour order, margins, theme, rotation — takes
effect the moment it changes, with no restart. A setting you have to reboot to test is a setting
nobody finds.

## Telling the user something

In-app alerts are iOS-style banners under the header, backed by a notification centre behind the
bell. Never a small pop near the button that caused it. A banner that means "connection lost" waits
several seconds first, because a reconnect takes under a second and must not flash.

What reaches a phone is decided by the rule that fired, not by a second layer of filtering. A rule
names its own services and its own urgency; the category switches exist for the daemon's own
chatter and must not silently swallow anything a person wrote.

## The order things are listed in

A list the user scans for something wrong reads worst first, then alphabetically inside each
level: conditional notifications, the notification centre, alarms. Urgency decides the group and
the name decides the place within it, so the thing most worth knowing about is at the top and
everything else is where its name says it will be rather than where it happened to be added.

Lists that are a history — events, logs, cook files — stay newest first, because there the time is
the subject.
