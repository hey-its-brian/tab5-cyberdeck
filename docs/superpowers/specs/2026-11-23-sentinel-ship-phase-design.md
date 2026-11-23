# SENTINEL ship phase: viable and deliverable over the air (2026-11-23)

Phases 1 to 6 of the Sentinel suite are built on `cl/sentinel-useful-93ca39`
and have never left this machine. The Tab5 runs v1.5.0-beta.1 from
`feature/app-toggles`, which this branch already contains. Before any more
features (the phase 7 deauth sweep stays parked), this phase makes the
branch something a deck can install over the air without SENTINEL getting
in the way, and something Brian's own deck can keep using with the board.

Brian's decisions, 2026-11-23:

1. A ship phase first, no new features.
2. A setting chooses between NET TOOLS and SENTINEL, and the user switches
   as needed. (Not a build flag and not a hidden developer toggle, which
   were the two options first proposed.)
3. The three parts are: the usability review finished inside SENTINEL, the
   NET TOOLS / SENTINEL setting, and the stability work the specs, plans
   and roadmap left open.
4. Brian decides whether to publish. Nothing is pushed or released by this
   phase.

## Goals, non-goals, anti-goals

Goals:

- A deck that updates to this build and has no radio board behaves exactly
  as v1.5.0-beta.1 does: the same six home tiles, NET TOOLS under APPS,
  nothing on the rear bus (EXT5V off, no UART task, the E07 lines untouched).
- A deck that had RF WATCH switched on (Brian's) comes up with SENTINEL.
- Everything the usability review asked for that the restructure did not
  finish is done, inside SENTINEL, or recorded here as deliberately left.
- The sim tests and the CI command (`tools/check.sh --firmware`) pass.
- A signed release candidate, v1.5.0-beta.2, with its notes, built the way
  every release is built, ready for one command from Brian.

Non-goals:

- No new SENTINEL tools, no phase 7, no node (rf-sentinel) changes.
- No change to how the other DECK//OS apps look (type sizes, colours).
- Sound or a backlight flash for an alert: the notice service stays silent
  by design until Brian decides otherwise.

Anti-goals (a failure even if it works):

- A deck without the board powering the bus or starting the UART task.
- The update changing anything a v1.5.0-beta.1 user can see, other than
  the one new row in SETTINGS.
- Publishing, pushing or tagging from this phase.

## Part 1: the usability review, finished

`docs/reviews/2026-11-12-rf-watch-usability-review.md` was mostly answered
by the restructure (`2026-11-13-sentinel-restructure-design.md`). An audit
of every finding against the code at d04e938 leaves these, which this phase
does. "Consistent with the rest of DECK//OS" means: the shared pieces
(notices, the saver) change for every app; SENTINEL's own screens change
only inside `apps/app_sentinel/`.

### Warnings land (F1, F2, F3, F4)

- The notice service (`components/deck_core/deck_notice`) gets a third
  level, ALERT, above WARN. SENTINEL posts its ALERT events as ALERTs
  (today every level above NOTE is posted as a WARN).
- An ALERT banner is drawn in the danger colour with the word ALERT, stays
  for 20 s instead of 4, ignores the 10 s quiet period, and replaces any
  WARN banner on screen. A WARN never replaces an ALERT. The count in the
  status bar turns red while an unseen ALERT is in it.
- The saver draws the unseen count and the latest warning's text under the
  clock ("2 WARNINGS. LATEST: TRACKER WITH YOU 12 MIN"). An ALERT wakes the
  saver and a sleeping screen, as a key would, and that wake is not
  swallowed by SENTINEL (the deck is simply awake with the banner up).
- Switching the board off no longer clears the unseen count: warnings
  clear when LOG shows them, nowhere else.
- The home tile's second line goes from 16 px dim to 20 px label grey; the
  alert line's tag goes from 18 px to 20 px.
- A line the deck says for a while on the alert line (`sn_say`: relearn,
  survey saved, offensive ended) never covers a live ALERT: an ALERT takes
  the line back and the said line is dropped.

### Unguarded exits and silent losses (F6, section 3)

- A drive survives a link drop. Today WARDRIVE is a board screen, so when
  the link drops the frame rebuilds it as blocked and that ends the drive.
  The frame keeps a running drive through a blocked rebuild; the screen
  shows NO LINK TO THE RADIO BOARD and the drive picks up when it returns
  (the state the screen already has words for).
- Opening LOG from the alert line (L, or a tap) goes through the same check
  as BACK: a drive asks, a capture stops with a note that names its file,
  TRANSMITTERS asks before its lists are lost.
- TRANSMITTERS: retuning (Left, Right, or Enter) asks when there is a burst
  or an emitter on screen, and says what is lost. The Enter action is
  labelled with what it does ("RETUNE TO 433.92, CLEARS THE LIST").
- Opening TRANSMITTERS while a site survey is running asks before the
  survey is cancelled.
- The survey's cancel button says STOP SURVEY, so it cannot be read as the
  dialog's own CANCEL.
- CAPTURE (WI-FI and BLUETOOTH) says which file it is writing from the
  moment it starts, and names it again when it stops, by any route.
- WARDRIVE's START and END are one large button in the body (about 200 by
  100 px), not a bar button.
- BACK names where it goes ("BACK TO WI-FI"), as the review asked; at the
  home page it still says LEAVE SENTINEL.

### Colour and type (F5, F11, F16)

- SENTINEL never draws data in the accent colour, because two of the four
  accents are the warn yellow and the ok green. Waterfalls and heat, the
  running states (RECORDING, MEASURING, SENDING, ASKING, a network tool
  working) and the quiet alert-line tag use one new palette entry,
  `g_pal.live`, a blue that is none of warn, ok or danger. The accents
  themselves do not change (that is every app's look).
- The slashed zero stays. Orbitron was checked (2026-11-23): its three
  stylistic sets and `salt` leave the zero alone, so there is no alternate
  to map to, and editing the glyph would mean a renamed copy of the font.
- Alt+H gets a SENTINEL section and drops the old NET TOOLS one (in NET
  TOOLS mode the section is headed NET TOOLS and lists its keys).

### Lists (F14)

- NETWORKS gains the two things DEVICES has: F cycles ALL and FLAGGED, and
  M marks the selected network as mine (eight BSSIDs, kept in settings as
  `wifi_mine`). A network that is mine shows MINE in place of its mark and
  is never flagged as an impostor or as open.
- A wardrive leaves out Bluetooth devices marked as mine (the roadmap's
  open line: headphones with a fixed address were written every 25 m).

### Left as it is, on purpose

- Keys that mean different things on different screens (C is CHANNEL on
  the honeypot and STOP SURVEY elsewhere): every key is on its button.
- The status bar's HOME button at 44 px: it is every app's.
- A day picker in LOG, and touch selection inside charts beyond the cursor.
- The channel chart of beta.1's SCAN in NET TOOLS mode (see part 2).
- The 60 s line after OFFENSIVE ends because the screen slept, rather
  than a dialog.

## Part 2: NET TOOLS or SENTINEL

### The setting

SETTINGS > APPLICATIONS gains one row at the top of the optional apps,
WIRELESS TOOLS, with a button that cycles NET TOOLS and SENTINEL, the way
STYLE > HOME TILES cycles its three. Under it, one line:

> NET TOOLS: Wi-Fi scan, ping, DNS, traceroute, LAN devices and a speed
> test on the deck's own Wi-Fi. SENTINEL: the radio board suite, with
> those tools inside it. Takes effect when the deck restarts.

Changing it shows a RESTART button on the row. RESTART asks first (CANCEL
focused, as every dialog that ends something) and restarts the deck. The
choice is saved as `wireless`: 0 for NET TOOLS, 1 for SENTINEL. Its
default is read once from the old `app_rf` setting (RF WATCH on in v1.5),
so Brian's deck comes up with SENTINEL after the update and every other
deck with NET TOOLS.

Applied at restart, not live, by design: the home grid and the app
registry are built once at boot, and a live switch would have to rebuild
both while an app runs. A restart is a few seconds and the row says so.

### What each choice registers

Both `main/main.c` and `sim/main.c` read the choice through one helper in
`deck_core` (`deck_wireless_sentinel()`), so the simulator can be started
either way (`--set wireless=1`).

| | NET TOOLS (default) | SENTINEL |
|---|---|---|
| Registered | NET TOOLS (optional, default on, id `net`, Alt+R) | SENTINEL (core, Alt+X) |
| Not registered | SENTINEL: no `on_enable`, so no EXT5V, no UART task, no E07 lines, no background timer, no notices | NET TOOLS: its screens are inside SENTINEL |
| Home tiles | TERMINAL, NOTES, SETTINGS, CALC, FILES, APPS (v1.5.0-beta.1's) | TERMINAL, NOTES, SETTINGS, APPS, FILES, SENTINEL (the restructure's) |
| CALC | core, on the home grid | optional, under APPS |
| APPS holds | WEATHER, PLAYER, NET TOOLS | WEATHER, PLAYER, CALC |

CALC's `optional` flag is set at boot from the choice, before registration.
The `app_net` on/off setting from v1.5 still applies to NET TOOLS, and
`sn_board` (the board's power) still applies to SENTINEL, so switching back
and forth loses nothing.

### NET TOOLS, the app

`apps/app_sentinel/app_net.c`, in the same component, because it is the
SENTINEL frame with a home page of seven tiles: NETWORKS (v1.5.0-beta.1's
SCAN: the deck's own radio lists every network when the board is off, with
the new filter and mark-as-mine), then PING, DNS, TRACEROUTE, PORT CHECK,
LAN DEVICES and SPEED TEST. All seven already work with the board off. The
frame is started without a log screen, and that means: no mode chip, no
alert line, the crumb starts at NET TOOLS, and the home page's button says
LEAVE NET TOOLS. Nothing in it touches `deck_sentinel`.

The one thing beta.1's SCAN had that NET TOOLS mode loses is the channel
chart: in SENTINEL it is the CHANNELS screen, which needs the board. The
release notes say so.

Considered and not chosen: restoring `apps/app_net` from v1.5.0-beta.1 as
it was. It is proven on hardware, but it is a second copy of six screens
in the old small type, and the one copy is the one the review fixed.

## Part 3: stability

What the specs, plans and roadmap noted and did not do, sorted into this
phase or out of it.

In this phase:

- The link-drop drive kill and the alert-line exits (part 1).
- ALERT reaching the banner as an ALERT (part 1).
- The wardrive's "mine" exclusion (part 1).
- The SD paths, reviewed against: the card pulled during a capture, during
  a drive, and during a site survey; the card full; a capture whose file
  cannot be opened asking the node for frames it then drops. Whatever that
  review finds is fixed if it is a loss or a lie on screen, and logged in
  the "as built" notes if it is only a counter.
- The link-loss paths, reviewed against: the node restarting mid-drive,
  mid-capture and in OFFENSIVE; the baud negotiation failing; the board
  switched off from BOARD while a tool is open. Same rule.
- `check.sh --firmware` green locally, which is exactly the CI job. CI
  itself runs on push, and nothing is pushed by this phase, so "CI green"
  means the CI command passed here on the branch head.

### What "sim tests" means here

The host unit tests (ctest) stay the automatic check; screenshots are for
eyes. Two things in this phase are not unit-testable as the code stands,
and are made so:

- The simulator gains `--list`: in headless mode it prints the registered
  app ids and the home tiles in order and exits. Two ctest cases run
  `deck_sim --headless --list` with `wireless=0` and `wireless=1` and
  compare the output, so the real `sim/main.c` registration is what is
  tested. `main/main.c` mirrors it line for line, as it does today.
- The notice rules (which level replaces which, the quiet period, when an
  ALERT wakes the screen, what the count's colour is) move into a pure
  file, `deck_notice_rules.c`, with no LVGL in it, and `deck_notice.c`
  only draws what the rules decide. The rules get a host test.

Phase 2 (Bluetooth) was designed while Brian was away and he has not
reviewed its decisions. They stand unless he says otherwise; listed here so
the review has a place:

1. BLE scanning is part of `watch`, so trackers are watched in any app.
2. The node aggregates devices, the host decides.
3. "Following" is ten minutes and 300 m with a fix, thirty minutes
   without one; one warning per tracker per session.
4. Mark as mine, eight addresses, in settings.
5. Fingerprints: the published Find My, SmartTag, Tile, Chipolo and Google
   formats; Flock by name and Axon by address prefix from community
   research, unverified against real units.
6. Capture rebuilds link-layer frames with a zero CRC, on 460800 baud or
   faster.

Out of this phase (recorded, not done): the node's half of phase 6 on
hardware (needs the C5 on USB); the hardware checks still open for phases
3, 4 and 5; a drive that carries on under other screens; a learned
baseline tagged with position; thresholds in SETTINGS; quiet mode for the
board while the screen sleeps.

## The release candidate

- Version 1.5.0-beta.2, continuing the beta the deck is on. What a user
  sees is still the v1.5 home and app toggles; the notes say so, and say
  what the WIRELESS TOOLS row is for.
- Notes in `docs/releases/v1.5.0-beta.2.md`, committed.
- `tools/release.sh` gains `--local`: everything up to and including the
  signed images (version bump commit, build, sign, verify, merge) and no
  tag, no push, no GitHub release. Brian runs the same command without
  `--local` to publish; the version commit is reused.
- The deck only installs updates from GitHub releases of this repository,
  so the over-the-air check from v1.5.0-beta.1 can only happen after Brian
  publishes the prerelease. The device checklist for that moment:
  1. The deck, BETA on, offers 1.5.0-beta.2 in the popup and installs it.
  2. It boots, confirms the build (15 s), and shows the six beta.1 tiles
     with NET TOOLS under APPS, or SENTINEL if RF WATCH was on.
  3. In NET TOOLS mode: the rear bus is dead (EXT5V off, no `sentinel`
     task in the log), ping and LAN work.
  4. SETTINGS > APPLICATIONS > WIRELESS TOOLS switched, RESTART, the other
     layout comes up; switched back the same way.
  5. In SENTINEL mode with the board: HELLO within ten seconds, the alert
     line, a capture naming its file, a drive surviving the board being
     unplugged and replugged.
- Before publishing, the same checklist (minus step 1) runs in the
  simulator, with screenshots of every changed screen in `docs/screenshots/`.

## Build order

1. The WIRELESS TOOLS setting, the helper, both mains, NET TOOLS as an app
   on the frame; sim tests for what each mode registers and puts on the
   home grid.
2. Notices: the ALERT level, the banner rules, the saver's lines, the wake;
   sim tests for the replace rule and the count.
3. The SENTINEL screen fixes, one area at a time: frame (BACK names, the
   alert-line exits, `sn_say` yielding, live colour), drive (link drop, big
   button, mine), wifi (capture file, NETWORKS filter and mine), spectrum
   (TRANSMITTERS guards, survey wording), ble (capture file), home tile.
4. The SD and link-loss review, with fixes.
5. Alt+H.
6. `check.sh --firmware`, screenshots, ROADMAP and README lines, the
   release notes, `release.sh --local`, the images.

## As built

(Filled in when the phase lands.)
