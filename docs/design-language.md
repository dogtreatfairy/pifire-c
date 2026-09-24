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

## The tokens

Everything the interface is made of is declared once, at the top of `web/style.css`, and nothing is
chosen again further down the file. **If a value is not from that block, either it is wrong or the
block is missing a step** — and adding a step is a decision recorded here, not a number typed into a
rule. The file once carried the palette twice over, 26 different font sizes between .6rem and
1.9rem, and nine corner radii; a stylesheet in that state cannot be consistent no matter how
carefully each rule is written.

What the systems this borrows from have in common is not a look. It is that the look is spelled out
in a small set of named decisions:

- **Radix** — a neutral ramp where every step has a job, so "which grey" is never a judgement call;
  and, separately, the step you *fill* with is not the step you *write* in.
- **shadcn** — every surface colour is paired with the ink that goes on it, so nothing has to guess
  what is readable on orange.
- **Skeleton** — the contrast value travels with the colour rather than being re-derived by whoever
  uses it next.
- **Material** — one state layer at one opacity for hover and press, applied the same way to
  everything; motion declared as duration and easing rather than typed per rule; and a minimum
  target size a thumb can actually hit.

### The neutral ramp, by job

| Token | What it is for |
|---|---|
| `--bg` | the page itself |
| `--surface` | a panel or card on it |
| `--surface2` | a control sitting on that |
| `--line` | a separator, or a quiet edge |
| `--line2` | an edge that has to be seen |
| `--muted` | text you read second |
| `--text` | text you read first |

### Meaning, twice: the fill and the ink

Each of the five meanings exists in three forms, and using the wrong one is the commonest way this
interface has broken.

| | |
|---|---|
| `--accent` `--ok` `--warn` `--danger` `--info` | the solid you **fill** a chip, tile or button with |
| `--on-accent` … `--on-danger` | what you **write on** that solid |
| `--accent-ink` … `--info-ink` | that meaning as **ink on the page**: coloured text and icons |

**A colour bright enough to fill a chip with is not a colour you can write in.** In the light theme
the orange measures 2.4:1 against white and the yellow 1.5:1; the back arrow, the active tab label
and every warning caption were written in them. The `-ink` variants are darkened for a light
background and are identical to the fills in the dark theme, where the fill already reads.

**And a filled swatch always carries its own ink.** Writing `color: #fff` on a coloured background
is how the app ended up with white on yellow at 1.4:1 — a glyph that was very nearly not there —
and white on red at 3.4:1 in seven places. Where the colour is chosen at runtime, as the settings
tiles are, the ink is computed from it: `inkOn()` in `web/icons.js` takes the relative luminance and
returns whichever ink is further away, so the next colour somebody picks is safe too.

Every pair in the app clears 4.5:1 in both themes, and that is checked by measuring the rendered
page rather than by eye.

### The scale

| | steps |
|---|---|
| Space | `--sp-1` 4px · `--sp-2` 8 · `--sp-3` 12 · `--sp-4` 16 · `--sp-5` 20 · `--sp-6` 24 |
| Radius | `--r-sm` 6px · `--r-md` 8 · `--r-lg` 10 · `--r-pill` |
| Controls | `--h-row` 46px · `--h-control` 35 · `--h-field` 41 · `--h-touch` 44 |
| Rules | `--bw` 1px, everywhere |
| Inset | `--pad-x` 12px: how far text sits from the edge of anything that holds it |
| Motion | `--dur-1` 120ms · `--dur-2` 180 · `--dur-3` 260 · `--ease` |

Type is nine steps, and each one is a **role** rather than a size, so a size is picked by what the
text is for:

| Step | | For |
|---|---|---|
| `--fs-3xs` | 10px | a count in a badge |
| `--fs-2xs` | 11px | a tab label, a unit beside a number |
| `--fs-xs` | 12px | a caption, a small upper-case label |
| `--fs-sm` | 13px | secondary text, a small button |
| `--fs-md` | 15px | body, and the title of a row |
| `--fs-lg` | 17px | a page or section title |
| `--fs-xl` | 21px | a prominent value |
| `--fs-2xl` | 28px | a readout on a card |
| `--fs-3xl` | 48px | a readout that is the whole point |

Three sizes are deliberately outside it: the gauge's own text, which is in SVG user units and scales
with the gauge rather than with the page, and Home's hero temperature, which is fluid
(`clamp(76px, 24vw, 120px)`) because it should fill whatever phone it is on.

### One press, one look

A pressed row, a pressed button and a pressed tab are the same event and should look like it. There
is one state layer — `--hover` at 8 % and `--press` at 12 % of the text colour, Material's figures —
laid over whatever the thing is already filled with, so it reads the same on a plain row as on the
orange button. This replaced four different gestures: a 2 % scale on a button, a 1 % scale on
another, an opacity drop on the back arrow, and two different tints on rows and section headers.
Hover only exists where there is a real pointer.

### Reachable by keyboard, and hittable by a thumb

Everything focusable shows the same ring: 2 px of `--ring` at 2 px offset, on keyboard focus only.
It is declared with `:where()` so it costs no specificity and a component that needs its own can
still have one.

**A control drawn smaller than a thumb still has to be hittable by one.** Material asks for 48 dp,
WCAG 2.5.8 for 24 px. The visible small button stays at `--h-control`, which is what keeps a list
dense, and an invisible target is grown around it to `--h-touch`. It grows **vertically only**: two
small buttons in a footer sit 8 px apart, and a target grown sideways would overlap its neighbour's,
which is worse than a small target.

### Words

Concise, technical, minimal. The reader knows what a pellet grill and a PID loop are.

A label is a noun phrase, not a sentence. Help is one short line, or nothing. State the fact, not
the reasoning behind it: a settings row does not explain why the setting exists or what will happen
in each case, and if it needs a paragraph the design is wrong rather than the wording. Prefer a
number with its unit to an adjective. Long-form explanation belongs in code comments and in this
directory, never on screen.

**Two actions are two buttons.** A segmented switcher that changes which single button you are
looking at hides one of the two things you might want and needs a sentence to say which mode you are
in — that is what Autotune's "Baseline / One Temperature" switcher did before it became **Tune
Baseline** and **Tune at 225°F**. A switcher is for choosing between two *states*; a choice between
two *actions* is one button each.

### Pages never type a size

The page modules build with named roles — `.help`, `.subhead`, `.readout-md/lg/xl` — and never an
inline `font-size`. Fifty-six of those had accumulated across the modules, in nine values, none on
the scale: the stylesheet can only be consistent if the JavaScript has a named thing to ask for.

The whole interface is checked by measurement rather than by eye: every route in both themes, with
every disclosure opened, plus every dialog and sheet — type on the scale, radii on the scale, text
contrast, row and control heights, tap targets, and that nothing clickable is a plain `<div>`. The
one standing exception is an inline link inside a line of text, which WCAG 2.5.8 exempts and which
cannot be grown without covering the lines above and below it.

### Motion

Durations and easing come from tokens, and everything stops under `prefers-reduced-motion` — with
one exception, spelled out in the rule itself: a probe past its target is *saying* something by
flashing, so with motion turned down it becomes the colour it was flashing to, said once.

## The phone

An app shell: only `<main>` scrolls, the bars are pinned inside a viewport-fixed body, and the safe
area insets place them. Home fits one screen without scrolling. The mode lives in the header pill
and nowhere else. Settings is an index of grouped lists with icon tiles, one page per group, never
one long page. No menu item appears in two places: Settings is what you configure, More is what you
do and what you look at.

**The surface is matte.** Nothing is translucent and nothing is blurred. The app once had a glass
look — frosted panels, a specular top edge, a floating pill tab bar — and it was handsome, but on a
control panel the surface behind a reading carries no information, and a number that has to compete
with a blurred picture of itself is harder to take at a glance. Flat surfaces, one-pixel rules,
small corners (10 px, 8 px for the small ones), and a tab bar that is a bar rather than a pill
floating over its own margins. Colour is reserved for meaning: the accent, the state colours, and
the icon tile that says what a row is about.

**Space is not free.** Rows are 46 px, not 56. Padding is what a thumb needs and no more. A section
label sits close to what it labels, in small upper case. Numbers are tabular so columns line up and
a reading does not jitter as it changes. The test is how much of a list you can take in without
scrolling, because scrolling is what costs you your place.

**Opening a section draws a card under its header; it does not restyle the header.** Closed, a
section is a plain row with a rule above it, like every other row. Its contents, once open, have to
be visibly *inside* something, because everything else on these pages puts content in a card — a
body that is loose text between two hairlines does not read as a section at all, and you cannot see
where it ends and the next header begins. So opening one draws a filled panel *beneath* the header,
hanging from its rule and rounded off at the bottom where the section ends.

The header itself is untouched by opening: the same band, the same fill, the same height, the same
rule above it, the same column. The only things that change are the chevron and what appears
underneath. A header that is a plain band when shut and a shaded, rounded plate when open is two
different controls wearing the same words, and in a list where some sections are open and some are
not, the run of headers stops lining up at all.

**One grid, and opening something moves nothing.** Every row, every section header and every open
section starts and ends on the same two pixel columns, and a header is a row: the same height, the
same padding, the same icon position, differing only in which way the chevron points. An open
section's surface is drawn with an inset shadow rather than a border, because a border is a pixel of
width and would shift everything inside it. The measurements that matter are worth checking with a
ruler rather than an eye: rows were coming out 46, 47 and 48 pixels tall on one page, small buttons
35 and 51, selects a pixel taller than the inputs beside them.

**Every form ends in a footer, and a footer looks the same wherever it is**: the container's own
width, a rule across the top, the actions on the right with the committing one last. A row of
buttons floating in the middle of a card's padding is not a footer, it is some buttons. The space
below it matches the space above the first field — inner containers contribute no padding of their
own, or the two stack and the bottom ends up twice the top.

**A rule belongs where two things meet.** A section header carries its rule on top, separating it
from whatever ended above it, and gains one underneath only while it is open, separating it from
its own content. A small upper-case label carries none: the rows beneath it already separate
themselves, and a rule under a label that sits above a card draws a second line right against the
card's own edge.

**A section header holds its place, and every heading is the same thing.** A label over a list, a
title on a page, a heading with an action beside it, and the header of a section you can open are
all one recipe: a full-width band on the page background with a rule under it, pinned to the top of
the screen while you read what it introduces, pushed off by the next one. Nothing is a box inside a
box — a header with its own frame, its own corners and its own shade reads as something bolted onto
the page rather than part of it, which is exactly how it looked when a section was a card and its
header was a chip inside it. For the same reason a list is rows separated by rules rather than a
card floating on a background, and a card that holds nothing but a list is not a card at all.

Two things this depends on: no ancestor may clip its contents (`overflow: hidden` makes the section
its own scrollport and the header never moves), and the scrolling area begins exactly where the
header bar ends — any padding between them is a window that content shows through above the pinned
header. Headings *inside* an open section do not stick: the section's own header is already holding
the top of the screen, and two stacked is a header for a header.

Sheets follow one shape: what it is about at the top with its current value, the choices in the
middle as full-width cards, and the commit row at the foot.

## The same thing looks the same on both screens

A mode has one mark wherever it appears — the header readout, the control bar on Home, the settings
row: **Hold is crosshairs, Smoke is a cloud, Stop is a plain square**, Startup a flame, Shutdown a
power symbol. `MODE_ICON` in `web/icons.js` is that map, and **everywhere a mode is drawn must read
from it rather than naming a glyph**, because a name typed in a second place drifts: Hold was a
bullseye on its own settings row while it was crosshairs everywhere else, and Stop was a square in
the control bar and a square-inside-a-circle in the header. Neither was visible to whoever changed
the map, because neither was reading it.

**An output that is running lights up whole.** FAN green, AUGER blue, IGN orange — the tile fills
with the colour on both the panel and the phone, rather than a small dot beside a word. **A probe
that has reached its target flashes**, in the colour of how far past it has gone: done, a step over
(5 °F / 3 °C), two steps over. The same thresholds on both screens, so a probe that is amber on the
grill is amber on the phone.

**A brand mark keeps its own shape.** A logo is drawn as its owner draws it — Pushover's disc,
ntfy's softer rectangle — at the same size as every other tile and with no coloured square behind
it. Cropping them all to one silhouette was tried and looked worse: a mark you recognise beats a
column that lines up.

**Selection has one shape.** On the panel a selected row is a filled rounded block with an outline
that follows the same corner. It used to be a square frame around a rounded fill, which stepped
outside the shape at every corner — invisible on an orange row against a dark card, and a stray box
around the red Stop row.

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

## The header, in three slots

What the grill is **called** on the left, what it is **doing** in the middle, how you are **reaching
it** on the right. The readout is centred on the bar itself rather than balanced between its
neighbours, so the number a glance goes to is in the same place whatever sits beside it; the left
slot is capped in width and ellipsises rather than pushing into it. On a sub-page the back
affordance takes the name's place, because a navigation bar says where you came from rather than
what the machine is called.

## One indicator, one question

The header carries one mark for "can this app reach the grill, and by what road". It is the
Tailscale logo when this browser is talking to the grill through the tailnet — decided by the
address in the address bar, not by the grill merely having Tailscale installed, because those are
different facts and only the first is about this connection — and a plain network glyph otherwise.
Green when the live link is up, red when it is not. There were two marks for this once, a coloured
dot beside the name and a Tailscale icon next to it, which answered the same question twice and
disagreed about how to say it.

## Telling the user something

In-app alerts are iOS-style banners under the header, backed by a notification centre behind the
bell. Never a small pop near the button that caused it. A banner that means "connection lost" waits
several seconds first, because a reconnect takes under a second and must not flash.

What reaches a phone is decided by the rule that fired, not by a second layer of filtering. A rule
names its own services and its own urgency; the category switches exist for the daemon's own
chatter and must not silently swallow anything a person wrote.

## Where a control goes

**A control belongs to the thing it acts on**, in the section that owns that subject, not in
whichever card happens to be nearest on the page. The hopper's calibration buttons once sat inside
the Loaded Pellets card, which is about which brand is in the grill and how much of it has burned:
a different subject that happened to share a page. A page that covers two subjects gives each its
own section, whole — the hopper's reading, its buttons and its two distances together; the brand,
the usage and the profile list together.

Before building a control, argue against it: where else does this number appear, and can the two
disagree? What happens when the input is nonsense? Does this duplicate something that already
exists? Each of those questions has caught a real fault here — a measured value that left the typed
box beside it showing the old number, and a calibration that could kill the reading altogether with
nothing on screen to say why.

## One question, one control

A new capability that overlaps something the app already does is folded into that control, never
added beside it. Two buttons for one idea — "Clear learning" next to "Erase everything" — read as
duplicates even when they differ, because the difference is only visible after you have opened both
and compared the wording. Find the control that already answers the question and extend it; if the
new thing genuinely belongs elsewhere, put it where it belongs and say so in the one place, as
Clear learning points at Autotune's Start From Scratch.

The same holds for switches. One question gets one switch, in one place. Learning was once asked
about three times on a single page — "Learn from cooks", "Apply learned tuning automatically", and
the controller's own copy of the same idea — and they could disagree with each other.

## The order things are listed in

A list the user scans for something wrong reads worst first, then alphabetically inside each
level: conditional notifications, the notification centre, alarms. Urgency decides the group and
the name decides the place within it, so the thing most worth knowing about is at the top and
everything else is where its name says it will be rather than where it happened to be added.

Lists that are a history — events, logs, cook files — stay newest first, because there the time is
the subject.
