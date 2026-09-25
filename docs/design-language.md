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

## Mobile first

**The test is an iPhone 17 Pro: 402 × 874 points.** If it is not readable and well organised there,
it is not finished. Desktop matters but comes second. Almost every layout fault found in this
project was invisible above 900 px and obvious at 402: buttons hanging out of the card holding them,
three footer buttons running off the left edge of the screen, a label folded onto five lines beside
a narrow input, a sheet 836 px tall in an 874 px viewport with its Save button below the fold.

Nothing may extend past the viewport, and nothing may sit outside the container that holds it.
**The page never scrolls sideways.** Anything that genuinely needs more width than the phone has --
a wide table -- scrolls inside its own wrapper, where the scroll belongs to the thing that is too
wide rather than to the page.

The way to check it, when a browser is not to hand, is to run the daemon in the simulator and walk
it headlessly:

```
./build/pifired --sim --port 8099
google-chrome --headless=old --window-size=402,874 --screenshot=out.png \
  --virtual-time-budget=25000 "http://127.0.0.1:8099/#/settings"
```

`--screenshot` honours `--window-size` where `--dump-dom` clamps the viewport, so screenshots are
the reliable measure. For the numbers rather than the picture, serve a temporary script from `web/`
that walks the routes, opens each pushed screen, and compares every child's `getBoundingClientRect`
against `document.documentElement.clientWidth`. That is how the fault below was found, and the same
walk proved it gone: on the broken build the probe editor reported `view=487/485` with
`.sheet-head` and both `.form-actions` at `[-2.0 .. 487.0]`, and Save clipped off the right-hand
edge.

### A full-width element pulls out to `--bleed`, never to a number

A card's footer rule, a sheet's header rule and a scrolling sheet body all have to reach the edges
of the container that pads them, which means a negative side margin. That margin is **the
container's own side padding and nothing else**, so it is written as `--bleed`, set by whoever is
doing the padding: `var(--pad-x)` inside a dialog, `0` at page level, where the content area has no
side padding and cards run to the screen edge on their own.

Writing it as a fixed `calc(var(--pad-x) * -1)` instead is what made every pushed settings screen
scroll sideways on a phone: the rules were written for a dialog, and each new page-level container
needed its own neutralising rule that the next one was then missing again. One variable, set once
by the padder, cannot be forgotten by the next container.

Controls carry `min-width: 0`. A flex or grid child sizes to its own content by default, so a
single `<select>` holding `ADS1115 · ADC0` was enough to make a whole page wider than the screen.

**Every section has a header, a body and a footer, and a footprint you can see.** A label above it,
its content, its actions in a footer with a rule across the top, and an edge that says where it
ends. Content that merges into whatever follows is not a section.

**Use the row, do not rebuild it.** `itemRow()` is a flex line: a tile, a body that takes the slack
and ellipsizes, a chevron, and its actions held at the end. Hand-rolled markup that looks like a row
is not one — the cook file list was built by hand and laid the name, the numbers and its two marks
out on three separate lines with the icons adrift in the middle. None of what makes a row a row
comes for free, and every list in the app already has it.

**A heading with no bounded block under it is not a heading, it is a stray line of text.** The small
grey uppercase label only reads as the name of something when something with an edge follows it
immediately. The History page had "History" floating above a card it was not attached to and "Cook
files" above rows sitting straight on the page background, and the result read as two random lines
in the middle of the screen. Every heading is followed by a card or an `.ios-list`, with nothing
loose in between.

**Say a number once.** The same reading in two places is two readings that can disagree, and the
reader has to work out which to believe. The History page showed the pit temperature in the header,
on the chart, in the chart's legend, and again in a small readout underneath — the readout went.

## The mark

PiFire's own symbol is **the barrel seen end on, its chimney, and the seam where the lid closes**.
One glyph, drawn once at `24 × 24` in `icons.js` as `barrel`, used at every size: the app icon, the
raised Home button in the tab bar, and the rail on a wide window.

It is drawn to the logo's proportions rather than approximated:

* **The barrel is centred in the box**, so it lands on the centre of the raised Home button and the
  chimney rises above it — the barrel is the thing the eye centres on, not the glyph's bounding box.
* **The circle is an arc with a gap, not a circle.** It stops where the chimney's sides cross it, so
  the barrel does not cut through the stack; the arc's endpoints are computed from the circle and
  the chimney's width rather than eyeballed, as are the two chords, so everything meets exactly.
* **The gap between the two chords is wider than the logo's.** At the size the Home button actually
  renders, the logo's own narrow band merges into a single thick line and the mark stops being the
  mark. Legible at the size it is used beats faithful at a size nobody sees it. This is the one
  deliberate departure from the drawing.

The app icon puts that glyph in white on a soft vertical warm gradient with a generous corner
radius, which is what reads as a modern iOS icon; the maskable variant runs the face to the edges
and insets the glyph into the safe circle, since the platform crops it to whatever shape it likes.
The PNGs are rendered from `icon.svg` — there is no second drawing to keep in step.

## Navigation

Five tabs: **Cook · Probes · Home · History · Settings**, with **Home in the middle, raised out of
the bar as a circle** carrying the end of a barrel grill — the barrel, the seam where the lid
closes, and the grate. It is the page you open the app to look at, so it sits where a thumb reaches
without moving, and it is the one shape in the app that is its own rather than a fifth identical
icon. The bar is still a bar; one thing rises out of it.

There is no "More". Its pages are the **Diagnostics** group at the foot of Settings, because "more"
names the leftovers rather than anything. Manual outputs left that group entirely: they apply in
Monitor mode and Home already carries them when you are in it.

**A probe appears in one place.** Everything you do to a probe is on its row in the Probes tab: the
reading, the target and its alarms, and its settings behind the chevron. The targets were cards on
Cook while the settings were a list under Settings, so one probe was in two places and neither
showed the whole of it.

## Screens and dialogs

**Anything bigger than a question is a screen, not a box.** Editing a notification, setting a probe
up, reviewing what the grill has said: you go there and come back, the way a native app works. A
large modal is a website's idea of the same thing — it hangs over the page, the back gesture cannot
reach it, and while it holds the page inert the tab bar does not answer, so there is no navigating
away from it either.

`pushScreen()` covers the content **between the bars**, so the mode readout and the tabs stay
exactly where they were: you have gone somewhere inside the app rather than had a panel thrown over
it. It pushes a history entry, so **the back gesture unwinds it**, the back arrow closes it, and
**tapping a tab closes it and navigates**. All three are one event to the caller: the promise
resolves.

**An edit screen's actions are one row, pinned to the tab bar.** Delete, Cancel and Save sitting at
the end of a long form are a scroll away from whatever you just changed: you edit a field at the top
and then go looking for Save. The row is **fixed to the straight top edge of the tab bar** — no gap,
no floating — so it is in the same place on every screen and always within reach of a thumb. One
row, always the same shape:

```
[🗑]                  ( ⌂ )                  [Cancel] [Save]
```

The destructive action is **a mark alone on the left**, with no word — it is not something to reach
for by reading, and naming it gives it the same weight as Save. Dismissive then committing on the
right, which is the order everything else uses. **The middle stays empty**, because the Home button
rises out of the tab bar and passes over this row: the bar sits below it in the stacking order, so
the circle is a layer on top and lands on nothing.

**A tab page can carry the same bar**, for the one or two things that page is for: the Probes page
puts *Add Probe* on it, in the primary colour because it is why you came to the page with a probe in
your hand, and *Filter* opposite. Same bar, same rules, so a page and an editor do not look like two
different apps.

What decides the side is what the action does, not what it is called: **the left slot changes the
thing** — add it, delete it — and **the right slot ends or governs the view** — Cancel and Save
finish the edit, Filter says what you are looking at. `actionBar(left, right)` in `app.js` is the
bar; `screenActions()` is the edit form's arrangement of it. An editor with nothing to delete passes
no `onDelete`, and a screen with an extra verb (Test on a notification rule) puts it before Cancel. The screen reserves
the row's height at its foot so the last field can still be scrolled clear. A footer with a rule
across the top remains right for a **section** inside a page; it is the whole-screen edit form that
moved.

**One navigation bar, and one back affordance, at every depth.** The app header already has one and
a place for it, so a pushed screen takes it over for as long as it is up — relabelled with where it
came from — and puts it back exactly as it found it. A screen that draws its own gives you
`‹ Settings` and `‹ Probes` stacked down the page: a navigation stack rendered twice, which belongs
to no app. A tab root shows no back at all.

A dialog is for a question — a confirmation, a dial pad. **Navigating away closes any dialog still
open**, because a box left hanging over the new page is the surest sign you are looking at a
website.

**A closed thing is gone, and that belongs to the attribute that closes it.** The browser hides a
`<dialog>` with `dialog:not([open]) { display: none }` in its own stylesheet, and any author rule
beats it — so `display: flex` written on `.dialog` unqualified kept every dialog on the screen
after `close()` had run: still drawn, no longer modal, no backdrop, and nothing left that could
dismiss it. Clearing the learning was where it showed plainest, because nothing opens a dialog
afterwards to wipe that element out of the way. **Layout properties go on `.dialog[open]`**, and
`[hidden]` is stated once with `!important` for the same reason. Two earlier fixes went at the
symptom — a guaranteed close mark, then closing on navigation — and both handed the user a button
that did exactly nothing they could see. When something will not go away, check what is keeping it
laid out before adding another way to dismiss it.

## Sheets

A sheet is three parts: a **header that stays**, a **body that scrolls**, a **footer that stays**.
`.sheet-head`, `.sheet-body`, then the footer. It is capped at 88 % of the viewport height, so on a
phone the title never scrolls away and the committing button is never below the fold.

**There is always a way out, and it is not the backdrop.** `showModal()` makes the rest of the page
inert, so a dialog you cannot dismiss is not a stuck dialog, it is a stuck app: the tab bar stops
answering and nothing moves. Escape needs a keyboard and the backdrop is a sliver beside a
full-height sheet on a phone, so neither is an exit there. **Every dialog carries a close mark** —
in its header if it has one, floating at the top corner if it does not — and it is added by the
`dialog()` helper rather than by each caller, so one written later cannot forget it.

**A dialog that is not a sheet scrolls as a whole.** A dialog taller than the screen that hides its
overflow is a trap: its buttons are below the fold and there is nothing to scroll, so the only way
out is the sliver of backdrop beside it. That is what the conditional-notification editor was on a
phone — it opened and simply sat there. Every form long enough to need it is a sheet; everything
else at least scrolls.

Inside the body: **one column**, related fields grouped under a small label, and a field that only
applies in one case appears only in that case — the ambient-reference switch shows for an Aux probe
and is not there otherwise. **Three choices get a segmented control, not a dropdown** you must open
to see what the options are (NN/g: a dropdown for two or three options hides them for no reason).
The destructive action is a row of its own, in red, above the footer — never in the row that
commits.

## A page scrolls; nothing inside it scrolls on its own

A capped, inner-scrolling region — `max-height: 60vh; overflow-y: auto` — is a **dialog's** answer
to being taller than the screen: the header and footer stay put while the middle moves. On a page it
is wrong twice over. It ends the content part way down and leaves a slab of empty background beneath
it that reads as a rendering fault, and it puts a second scroll inside the one the thumb is already
using. The notification centre had both: the list stopped at 60% of the viewport, cutting the last
notice in half, with a black band between it and the tab bar.

Content runs to the end and **disappears behind the tab bar**, which the content area's bottom
padding already accounts for, so the last row can always be scrolled clear of it.

## Actions carry a mark and a colour

**Say it the way it is written, not the way it is spoken.** The conditional editor offered "All Of
These" and "Any Of These" where every other tool on earth writes AND and OR; a rule is a logical
expression and the person building one already knows the words. A technical term that is exact beats
a friendly phrase that is longer and vaguer.

A modern interface says what a control does with a shape and a colour before it says it with a word.
Every action button leads with its icon — **a trash can for delete, a pencil for edit, a plus for
add, a tick for save, a cross for cancel** — and the colour carries the same message:

| | |
|---|---|
| **Destructive** | red (`.btn.danger`): delete, remove, unpair, clear, erase |
| **Affirmative** | the accent (`.btn.primary`): save, add, load, start |
| **Everything else** | the plain button, or `.btn.ghost` for cancel and backing out |

`actionBtn(kind, label, attrs, icon)` in `web/app.js` is where the pairing lives, so a delete cannot
be built grey and a cancel cannot be built orange. On a narrow screen the mark carries the meaning
when the label is the first thing to be cut.

**Red is a tint, not a fill.** A solid saturated red made Remove the loudest thing on a settings
page — louder than Save, which is the action actually wanted. `.btn.danger` is a red-tinted surface
with red ink and a red edge, which is also what the icon-only version always looked like, so the
worded button and the trash mark are one family. The **solid** fill is kept for the two places that
are genuinely an alarm: the Error mode pill and the alert banner.

**A row's own actions are marks; a section's action keeps its words.** One cook file in a list gets
an icon-only download and an icon-only delete, because the row already says which cook it is and
four words repeated down the page are noise. The button that clears the whole list, or the whole
history, stays a full-width worded button — it acts on everything, so it says so. Where two marks
would need explaining, there is one too many: the cook list had a *Download* and an *Analysis log*
and nothing said what the difference was, so it now has one download, the analysis log, which is
the superset.

## Information has an order

Group what belongs together and then put the groups in the order the thing actually happens, not the
order the pages were written. Settings reads as a cook does: **Startup & Shutdown, then Hold, then
Smoke, then Lid-Open, then Keep Warm, then Pellets** — light it, hold it or smoke it, what happens
during, what is left afterwards. The same rule governs a page's own sections: the probe editor is
Identity, then Connection, then Visibility, because that is the order you would think about a probe
in. A list the user scans for something wrong is ordered by urgency instead; a history is newest
first, because there the time is the subject.

## What a row shows, and what is one layer in

**The surface carries the live state; the configuration you set once lives inside.** Anything with
many fields — a probe, a probe profile, a tuning anchor, a hardware device, a notification rule — is
listed by what changes and what you check, not by how it is set up.

| Thing | The row shows | Behind it |
|---|---|---|
| Probe | reading, signal and battery, target and time left | port, device, profile, type, visibility |
| Probe profile | name, how many probes use it | the Steinhart–Hart coefficients |
| Tuning anchor | set point, runs behind it, the weather it was measured in | PB, Ti, Td, Ku, Pu |
| Hardware section | what it is **set to** — the board, the panel, the sensor | every option for it |
| Notification rule | name, urgency, one line of what it watches, its switch | the condition tree |
| Pellet profile | brand, wood, rating, whether it is loaded | notes, and the actions |

The test is a saved credit card: it is listed as "Visa •••• 4242, expires 12/25, Default", never as
the entry form that created it.

**Tapping goes to what you probably wanted.** Tapping a probe on the dashboard offers a target, an
alarm and a timer, because that is why you tapped it during a cook; its settings are a further step
in, on the Probes tab. The ADC port and the profile were once the probe's subtitle, advertising
themselves all cook to say something that had not changed since the grill was built.

## The pit probe is not a food probe

The pit probe has **no target of its own and no fixed alarms**. What it is aiming at is the set
point, which is what Hold mode is for, and offering a second place to type one would be a second
answer to the same question. Its over- and under-temperature alarms are **conditional
notifications** comparing it with the set point, so they keep meaning the same thing when the set
point changes; a limit typed once would not. Its row and its sheet point at Hold Mode and at the
rules instead of duplicating either.

A food probe keeps both: a target is the whole point of it, and a fixed alarm either side is
meaningful on something that only goes one way.

## Off, and out of the way, are different

A **disabled** probe is switched off: it reads nothing, and it lives in Settings, which is where you
would go to switch it back on. A **hidden** one is working perfectly and simply is not part of this
cook — the third grate probe, an ambient sensor you are not using today. It keeps reading, keeps
being logged and keeps feeding the notification rules; it is only out of the way.

They are different questions, so they are different controls in different places. Enabling lives in
Settings with the rest of the setup. Hiding lives on the Probes tab, where you are cooking, behind
**Show or Hide Probes**, and the tab says how many are hidden and how many are disabled so neither
is ever silently missing.

## The row vocabulary

Four shapes, and everything in the app is one of them.

**A settings row** — icon tile, title, optional subtitle, and **what it is set to on the right**,
then the chevron: `Board   PiFire Compact PWM PCB  ›`. The value goes on the right, not folded into
the subtitle, because that is where the eye looks for the answer to "what is this at?".

**A saved-item row** — the shape a saved card has: a mark for what it is, its name, one line of
detail, a **badge** when it is the one in use (`Loaded`, the equivalent of `Default`), and the
action on it as a **bare icon** at the end. The row itself opens it.

**A toggle row** — title, optional one-line subtitle, and the switch. Nothing else; whatever the
switch governs is behind the row.

**A data row** — for several of the same measured thing, where the columns matter: a table on a wide
screen, and on a phone each row becomes a card of captioned values. Use it when the values line up
and want comparing (the tuning library), not for a collection of saved things, which wants the
saved-item row.

**Adding another one is a full-width outlined button at the TOP of the section**, under the
heading, not a small button beside it and not at the foot. At the foot it sits below every item and
its actions, which on a phone is a screen and a half of scrolling to reach the one thing you came to
the page to do when you have a new probe in your hand.

**`view.append()` is the DOM's, not `el()`'s.** `el()` drops null children; `append` writes them out
as the word "null", which is how a bare `null` appeared under the probe list. Filter before
appending: `view.append(...[a, cond ? b : null].filter(Boolean))`.

**An input may carry its own mark**, inside it and ahead of the text, so the field says what it is
for before its label is read.

## Managers

Anything the user keeps several of — probe profiles, pellet profiles, probe hardware — is a
**manager**: a section bar with the name and an Add button, then one row per item showing what it is
and what it is worth knowing (how many probes use this profile; the coefficients; whether these
pellets are loaded). Opening a row shows its fields over a footer that saves or deletes **that item
alone**. A stack of bare disclosures under one Save button does not say which item a field belongs
to or what saving will affect.

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

## The header, in two slots

What the grill is **doing** on the left — the mode plate and the temperature it is actually at — and
how you are **reaching it** on the right. The bar is as tall as the plate it carries and no taller.

The readout shows the **same two things on every page**. It used to show the set point (or a
countdown) on Home and the real temperature everywhere else, so one plate in one place meant two
different things depending on which tab you were on; the target and the countdown are both on Home
already, beside the gauge that gives them context. The grill's name is gone from the bar: it is not
a reading, it never changes, and it was taking the position a glance goes to first. On a sub-page
the back affordance sits to the left of the readout.

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
