# App toggles and the new home screen

Status: approved design, 2026-11-05. Target release: v1.5.0.

## Why

The home screen got busy: five tiles each carried a number, a badge, a
tagline, a module list or widget, a shortcut footer and a status word.
Upcoming modules (SDR, Bambu printer, Claude Code client) would make it
worse, and not everyone wants every module. This change:

1. lets each user switch optional apps on and off in SETTINGS, and a switched
   off app does no background work at all;
2. replaces the home row with a fixed layout: a live panel and six tiles that
   never change, with optional apps one tap further in under APPS.

The look around the tiles stays exactly as it is: status bar, grid
background, colors, the `// SELECT MODULE` header row with the shortcut hint
and node/uptime, and the SYS ticker. Only what sits inside the tile area
changes.

## Scope

In: the app model (core vs optional, enabled state), the home layout, the
APPS screen, SETTINGS > APPLICATIONS, the panel, shortcuts.

Out (separate projects): the sync service picker and its backend API; the
SDR, Bambu and Claude Code modules themselves; the firmware launcher.

## 1. The app model

Additions to `deck_app_t` (`components/deck_core/include/deck_app.h`):

| Field | Meaning |
|---|---|
| `bool optional` | false for core apps, which are always on |
| `bool default_on` | enabled state before the user has ever touched it |
| `const char *desc` | one line for SETTINGS > APPLICATIONS ("Forecast and the home panel widget") |
| `void (*on_enable)(deck_app_t *)` | optional: start background work |
| `void (*on_disable)(deck_app_t *)` | optional: stop background work |
| `void (*on_panel)(deck_app_t *, lv_obj_t *section)` | optional: draw a live section in the home panel |

`on_tile` is removed; its one user (WEATHER) moves to `on_panel`.

New calls in `deck_app.c`:

```c
bool deck_app_enabled(const deck_app_t *app);  /* core: always true */
void deck_app_set_enabled(deck_app_t *app, bool on);
```

- Enabled state lives in NVS as `app_<name>` (i32, lower-case name,
  e.g. `app_weather`). Missing key means `default_on`.
- `deck_app_set_enabled()` saves the key, calls `on_enable` / `on_disable`,
  and asks the shell to rebuild the home and APPS screens if one is
  showing. Takes effect immediately; no
  restart.
- At boot, after registration, the shell calls `on_enable` for every enabled
  optional app.
- `deck_app_by_hotkey()` ignores disabled apps, so their Alt+letter does
  nothing.
- If the running app is disabled (not possible from its own screen today,
  but guard it), the shell returns home first.

Core apps: TERMINAL, NOTES, SETTINGS (today's SYSTEM, renamed on its tile
and title), CALC, FILES. FILE PORTAL and the Ollama chat stay part of FILES
and TERMINAL.

Optional apps today:

| App | default_on | Background work behind on_enable / on_disable |
|---|---|---|
| WEATHER | on | panel section and its refresh timer |
| PLAYER | on | `on_enable` calls `player_init()` (moved out of `app_main`, made safe to call twice). Disable stops playback and hides the status bar indicator; Alt+P does nothing. A task already created (28 KB stack) stays until the next boot, when it is no longer created |
| NET TOOLS | on | none outside its screen |
| RF WATCH | off | none (placeholder, still `eta = "v1.5"`) |

Future apps (SDR, BAMBU, CLAUDE CODE) register as optional with
`default_on = false`.

## 2. Home screen

```
 status bar (unchanged)
 // SELECT MODULE        SHORTCUTS: ALT+H        NODE TAB5 :: UPTIME
 +--------+ +----------+ +----------+ +----------+
 | LIVE   | | TERMINAL | |  NOTES   | | SETTINGS |
 | PANEL  | +----------+ +----------+ +----------+
 |        | +----------+ +----------+ +----------+
 |        | |   CALC   | |  FILES   | |   APPS   |
 +--------+ +----------+ +----------+ +----------+
 SYS> ticker (unchanged)
```

- Fixed: the panel plus six tiles, always in this order. Toggling apps never
  changes the home screen's tiles.
- Tile content: icon, name, the Alt+letter in the top right corner, the pink
  accent bar. No number, badge, tagline, footer or status word.
- Shortcuts: Alt+1..6 open the six tiles in reading order; Alt+letter opens
  any enabled app wherever it lives. APPS is Alt+A, so RF WATCH moves from
  Alt+A to Alt+X. The Alt+H shortcut list is updated to match.
- Keyboard: arrows move through the 3x2 grid; the panel is not in the focus
  order (its sections are touch targets).
- Size check: the tile area is 1232 x 470 px today; with a ~360 px panel and
  18 px gaps each tile is about 270 x 226 px (about 23 x 19 mm on the
  Tab5), well above the 105 px touch minimum.

### The live panel

- Stacks the `on_panel` sections of enabled optional apps in registration
  order, at most three (each about 140 px tall). More than three: the first
  three.
- Each section is a touch target that opens its app, so an app with live
  info is one tap from home even though its tile is under APPS.
- No sections (all panel apps off): the panel shows a quiet DECK//OS mark in
  dim text.
- The status bar already shows the clock and date, so the panel does not.

## 3. APPS screen

- Opened by the APPS tile. Uses today's group screen: same chrome, back
  button, `ESC OR DEL: BACK TO DECK` hint. `deck_group_t` stays, reduced to
  this one group, filled at build time from every registered optional app.
  `deck_home_add_group()` / `deck_group_add()` calls in `main.c` go away.
- Every optional app has a fixed slot (registration order) whether it is on
  or off, so tiles never move when apps are toggled.
- Grid: 3x2 for up to 6 optional apps, 4x3 for 7 to 12. Sized by the total
  number of optional apps, so it only changes when a new app ships. Cap 12.
- Three tile looks:
  - enabled: normal tile, taps open the app;
  - disabled: greyed (dim icon, name and accent bar, `OFF` where the
    hotkey letter sits). A tap opens SETTINGS > APPLICATIONS with that app's
    switch focused;
  - offline (`eta` set, RF WATCH today): the existing hazard look,
    independent of enabled state, because it means "not built yet" rather
    than "switched off".
- One tile state, one build function: `build_tile()` takes an
  `is_disabled` flag and switches styles.

## 4. The settings hint

One disabled tile carries an overlay: `DISABLED` and `ENABLE IN SETTINGS`.

- Tone: muted text one step brighter than the greyed tile, over a light
  scrim. No accent fill, no button look. Readable when looked for; does not
  pull the eye each time APPS opens.
- Which tile: the disabled app with the lowest ID, where the ID is the
  registration order (`deck_app_index()`), which is also grid order. So the
  hint is always on the greyed tile nearest the top left corner.
- Computed when the APPS screen builds; nothing is stored. Toggling an app
  rebuilds the screen, so the hint follows automatically, including on a
  fresh install where default-off apps were never switched by the user.
- Offline tiles never carry the hint (they are skipped when looking for the
  lowest ID).

## 5. SETTINGS > APPLICATIONS

- SETTINGS (today's SYSTEM module) gains a tab row like FILES uses
  (`small_button` tabs): `SETTINGS | APPLICATIONS`. The SETTINGS tab is
  today's three-column screen unchanged.
- APPLICATIONS lists every app in one column: icon, name, `desc`, and on the
  right a switch (optional apps) or a dim `CORE` label (core apps). Core apps
  come first, then optional apps in grid order.
- Flipping a switch calls `deck_app_set_enabled()`.
- `app_sys_open_applications(const deck_app_t *focus)` opens SETTINGS on this
  tab with `focus`'s switch selected; the APPS screen uses it.

## 6. Error handling and edge cases

- NVS write fails: the switch flips back and the toggle does not apply.
- All optional apps off: APPS still opens; every tile is greyed and the
  first one carries the hint.
- Hotkey for a disabled app: ignored (no toast). Its tile and the hint show
  the way back.
- A module's `on_disable` must be safe to call when it never started, and
  `on_enable` safe to call twice.

## 7. Testing

Sim first (`sim/`), screenshots into `docs/screenshots/`:

- home with all panel apps on, with WEATHER only, with none (empty panel);
- APPS with every optional app on, with some off (hint on the first one
  disabled), with all off;
- SETTINGS > APPLICATIONS;
- a 12-app APPS grid using stub apps, to check the 4x3 sizing.

Unit-style check in the sim build for the hint rule (a pure function over
the app list): none disabled gives no hint; disabled apps 3 and 5 give 3;
re-enable 3 gives 5; an offline app with the lowest ID is skipped.

On the Tab5 (after a full 16 MB flash backup with esptool, so the previous
state can be restored in one command): toggle each optional app; confirm
WEATHER stops refreshing (no fetch lines in the log), PLAYER stops playback
and its indicator and Alt+P go away; Alt+letter for a disabled app does
nothing; settings survive a reboot; an OTA update from v1.4.x keeps every
current app enabled.

## 8. Rollout

- Ships as v1.5.0 betas from `feature/app-toggles`, then merges to main.
- Upgrade path: no NVS keys exist yet, so every current app falls back to
  `default_on`; only RF WATCH (a placeholder) starts off.
- README and ROADMAP: new home layout, the APPLICATIONS tab, the shortcut
  changes (Alt+1..6, Alt+A, RF WATCH on Alt+X); new screenshots.
