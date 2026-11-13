# SENTINEL: one module, one purpose per screen (2026-11-13)

RF WATCH grew tool by tool, and an independent review
(`docs/reviews/2026-11-12-rf-watch-usability-review.md`) found it hard to
use in the hand: warnings do not land, text is small and dim, controls do
not look like controls, and each screen does several jobs. Brian's
decisions, made on 2026-11-13 after reading it:

1. Restructure now, before any more features. Phase 6 (the transmit tests)
   stays paused.
2. Function before looks: "we are hurting functionality for aesthetic
   appeal."
3. Anything that can be tapped or clicked must be easy to identify.
4. The purpose of each screen must be obvious. Less on screen at a time. A
   top-level overview may be dense; below it, one screen per function, and
   extra menu screens are fine. The test: ask "why am I on this screen",
   and whatever is not tied to the answer does not belong on it.
5. The home screen of DECK//OS is TERMINAL, NOTES, SETTINGS, APPS, FILES,
   SENTINEL. CALC moves under APPS. RF WATCH and NET TOOLS become one
   module, SENTINEL, with no distinction between them.
6. No mode gate. The two modes are called DEFENSIVE and OFFENSIVE (not
   SAFE and RED TEAM, and not TESTS).

## The module

SENTINEL is a core app and a home tile (Alt+6, and Alt+X as before). It
always opens in DEFENSIVE, on its home page. The radio board's power is a
setting on the BOARD screen, not the app's on/off switch as it was (the old
`app_rf` setting is read once as its default, so a deck that had RF WATCH
on keeps its board on). The screens that use the deck's own Wi-Fi (the old
NET TOOLS) work with the board off.

## The screen tree

A menu page is a grid of tiles. A tile is a button: it shows its name, one
live line about its area, and a mark when that area has a warning.

| Home tile | Screens under it | The question each answers |
|---|---|---|
| WI-FI | NETWORKS | What networks are around me, and is one an impostor? |
| | CHANNELS | How busy is each Wi-Fi channel right now? |
| | BEST CHANNEL | Which of 1, 6 and 11 should my own network use? |
| | DRONES | Is an aircraft announcing itself nearby? |
| | CAPTURE | Record the Wi-Fi frames in the air to a file. |
| BLUETOOTH | DEVICES | What Bluetooth devices are around me? |
| | TRACKERS | Is a tracker or a surveillance device near me? |
| | CAPTURE | Record Bluetooth advertising to a file. |
| SPECTRUM | 433 MHZ | What is on the 433 MHz band now and lately? |
| | TRANSMITTERS | What transmits at one 433 MHz frequency, and how often? |
| | 2.4 GHZ | What is on the 2.4 GHz band now and lately? |
| | SITE SURVEY | Measure this place for a minute and keep it as a file. |
| | JAM WATCH | Is someone jamming, and what is the evidence? |
| NETWORK | PING | Does a host answer, and how fast? |
| | DNS | What address does a name have? |
| | TRACEROUTE | What path do packets take to a host? |
| | PORT CHECK | Is a port open on a host? |
| | LAN DEVICES | Who else is on the network I joined? |
| | SPEED TEST | How fast is my connection? |
| WARDRIVE | (one screen) | Record what is heard, with position, as I move. |
| LOG | (one screen) | What has SENTINEL warned about? |
| BOARD | RADIO BOARD | Is the radio board on, linked and whole? |
| | GPS | Do I have a position fix? |
| | DIAGNOSTICS | The link's counters and test commands, for when it misbehaves. |
| OFFENSIVE | HONEYPOT | Which devices would join a network with this name? |

A list screen (NETWORKS, DEVICES, TRACKERS, LAN DEVICES) shows the list and
nothing else; choosing a row opens that row's detail as its own page, and
BACK returns to the list.

## The frame every screen shares

Top to bottom:

1. **Where you are and why**: `SENTINEL > WI-FI > NETWORKS` and one
   sentence of purpose, with the mode chip (DEFENSIVE green, OFFENSIVE
   red) at the right.
2. **The alert line**: quiet, it says ALL CLEAR in one thin line; with a
   warning it is a full-width bar with the warning's words and age, coloured
   by its level. It is the same on every SENTINEL screen, so a warning is
   seen wherever the user is.
3. **The body**: the one thing this screen is for.
4. **The action bar**: every action the screen has, as buttons. A button
   shows its key and what it does (`S  START SURVEY`), is at least 64 px
   tall, and is the only filled, accent-bordered shape on the screen apart
   from tiles and list rows. A key and its button are the same action.
   BACK is always the first button. Explanations live behind HELP.

Rules:

- Only things that can be tapped look tappable: buttons, tiles and list
  rows are filled and bordered; information is plain text under a plain
  heading, with no panel around it that could be taken for a tile.
- No text under 20 px. Values a user reads to decide something are 24 px or
  more; each screen has at most one state word at 36 px.
- A secondary text colour with real contrast (`g_pal.label`, about 6.6:1)
  replaces the dim grey for anything that must be read.
- Colour never carries meaning alone: a level has a word or a mark too.
- Yellow and red are for abnormal things only. "Busy" is not a warning.
- Keys mean the same thing everywhere: arrows move, Enter opens or does
  the main action named on its button, Esc and Del go back one level,
  digits pick a tile on a menu page and do nothing else.

## Levels

Three, used the same way everywhere:

- NOTE: worth writing down (a tracker first heard, 433 MHz activity, a fix
  gained). White in the log. No count, no banner, no alert line.
- WARNING: something to look at (a tracker staying with you, an open twin
  of a known name, a drone, a surveillance device). Yellow, a triangle.
- ALERT: something happening now that needs attention (likely jamming, a
  deauth flood, karma, a beacon flood). Red, stays on the alert line until
  it ends or is acknowledged, and always replaces a lower banner.

The node and the core still only say "warn" or not; the deck classifies a
warning's text into WARNING or ALERT, and treats "MHz active" and "MHz
quiet" as NOTEs.

## Guards

- Entering OFFENSIVE asks first, with CANCEL focused and a one second
  hold-off. Leaving the OFFENSIVE area, the screen sleeping, or leaving
  SENTINEL returns to DEFENSIVE, and says so when the user comes back.
- "Learn this as normal" asks first, says what is lost, and warns again if
  a jam state is active.
- Ending a wardrive asks first. (As built: leaving the WARDRIVE screen
  still ends the drive, because every other screen asks the board's radio
  for something else, but BACK asks first and says so. A drive that carries
  on under other screens needs the driver to refuse those requests, and is
  left for later.)
- A site survey keeps running if the screen is left and its result is
  waiting on return.

## Out of scope here

- Sound, a backlight flash or waking the screen for an ALERT (the notice
  service is silent by design; Brian's call, later).
- Changing type sizes or colours for the other DECK//OS apps.
- Phase 6.

## Build order

1. The frame (`apps/app_rf/sn_ui.[ch]`), the screen tree and the menu
   pages; the DECK//OS home tiles; CALC under APPS; the modes.
2. Each tool's screens, split: spectrum, Wi-Fi, Bluetooth, network,
   wardrive, log, board.
3. Levels, the alert line and the guards.
4. Simulator screenshots of every screen for Brian, then the boards.

## As built (2026-11-13)

- `apps/app_sentinel/` replaces `apps/app_rf/` and `apps/app_net/`:
  `sn_frame.c` (the frame, the stack of screens, menus, lists, levels, the
  mode), `app_sentinel.c` (the module, the board's power setting, notices in
  the background, the line on the DECK//OS home panel) and one file per
  area. `sn_ui.h` is what a screen is and what it may use.
- `g_pal.label` (#8E98B5) and `g_font.mono_b` (24 px) are new in the theme.
  The other apps do not use them yet.
- `deck_app_boot()` now calls `on_enable` for core apps too, which is how
  SENTINEL brings the board up at boot.
- The alert line shows the newest warning for two minutes and the newest
  alert for five, and a band that is up for as long as it lasts. It opens
  the LOG (`L` or a tap). It does not need acknowledging.
- Lists show seven rows of 50 px. Charts take a tap or a drag to move the
  cursor.
- Checked in the simulator: every screen; a jam scene (the home page shows
  the alert and marks SPECTRUM, JAM WATCH names the evidence, RELEARN warns
  that a band is abnormal); the screen sleeping with the honeypot on air
  (back on the home page in DEFENSIVE, flag gone, with a line saying why);
  Alt+X, Enter and digits typed fast (never reaches OFFENSIVE).
- Checked on the Tab5 with the board linked: a bench opened every screen
  outside OFFENSIVE twice (48 in all, about three seconds each). No crash.
  Free internal memory stayed at 310 to 312 K (lowest ever 217 K, set
  before SENTINEL opened). Free PSRAM was 4 to 15 K lower on the second
  round than on the first for the same screen, out of 26.8 MB: most likely
  glyph caches for the new 24 px face still filling, but not proven, so a
  longer soak is worth doing.
- Not checked on hardware yet: touch (the bench presses keys), the
  OFFENSIVE area, a drive, and how the type reads on the real panel.

## Changes after Brian tried it (2026-11-13)

- BACK's left end is an arrow head pointing the way it goes, not a corner.
- Tiles have the rise of colour from their top edge that the DECK//OS home
  tiles have, and their icon again at 200 px and about 13 % opacity behind
  everything else on the tile (`g_font.icon_xl`, with a small glyph cache
  because each glyph at that size is tens of kilobytes).
- Under its name a tile shows what its area is doing now, when it has
  something to say, and then a line from a film that fits it (`quote` in
  `sn_screen_t`; the film is in a comment beside each). The sentence saying
  what a screen is for is still the first thing on the screen itself. The
  lines are written without quotation marks, because LVGL may wrap after a
  full stop and leave a closing mark on a line of its own.
- The alert line moved from under the title to the bottom edge of every
  SENTINEL screen, below the buttons: where the info line is on the DECK//OS
  home screen. The order is now title, body, buttons, alert line.
- Measured on the Tab5: a tile page takes 250 to 325 ms to draw without
  the faint icons and 250 to 360 ms with them, so they cost 30 to 45 ms a
  page. Their glyph cache settles at about 500 K of PSRAM and does not
  grow on a second pass. Free internal memory is unchanged.

## A second round of changes (2026-11-13)

- The film line sits at the foot of its tile, in a light tint of the
  tile's own colour, so it cannot be taken for the status above it.
- The small icon beside each tile's name is gone: the large faint one
  behind the text does that job.
- The alert line is built like the info bar on the DECK//OS home screen: a
  chamfered panel with a filled tag at its left end. The tag says SENTINEL
  when quiet and WARNING or ALERT otherwise, so the level is a word as well
  as a colour, and the marks in front of the text went.
- The status bar's top-left button says HOME and always goes to the home
  screen, in every app (`deck_statusbar.c`). It used to say DECK or BACK and
  go back one level. Going back a level is Esc, Del, or a module's own BACK.
  Leaving WARDRIVE by HOME ends a running drive without asking, as an Alt
  shortcut to another app always did; only BACK asks.
