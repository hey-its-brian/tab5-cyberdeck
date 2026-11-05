# App Toggles and Fixed Home Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Optional apps can be switched on and off in SETTINGS > APPLICATIONS, and the home screen becomes a fixed live panel plus six tiles, with optional apps under APPS.

**Architecture:** `deck_app_t` gains core/optional metadata, enable hooks and an `on_panel` section; enabled state lives in NVS and is read through `deck_app_enabled()`. The home keeps today's chrome; `deck_launcher.c` draws a new tile area (panel plus 3x2 grid on home, fixed-slot grid on APPS with greyed tiles and one hint). SETTINGS (today's SYSTEM) gains an APPLICATIONS tab.

**Tech Stack:** C, ESP-IDF 5.5, LVGL 9, desktop simulator in `sim/` (SDL2, CMake, headless screenshots).

**Spec:** `docs/superpowers/specs/2026-11-05-app-toggles-home-design.md`

## Global Constraints

- Work on branch `feature/app-toggles`; never commit to `main`.
- No em dashes anywhere (code comments, docs, commit messages). Grep before each commit.
- NVS keys are at most 15 characters. Enabled-state key is `"app_" + id`, so every `id` is at most 11 lower-case characters.
- Missing NVS key means `default_on`. Current apps: WEATHER, PLAYER, NET TOOLS on; RF WATCH off. Future apps off.
- Optional apps cap: 12. Registry cap `APP_MAX` raised from 12 to 20.
- Home tile order is fixed: TERMINAL, NOTES, SETTINGS, CALC, FILES, APPS (Alt+1..6). APPS is Alt+A; RF WATCH moves from Alt+A to Alt+X.
- Keep the home chrome unchanged: status bar, grid background, `// SELECT MODULE` header row, SYS ticker.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Sim: configure once with `cmake -S sim -B sim/build`. Firmware: `. ~/esp/esp-idf-v5.5/export.sh && idf.py build` (unsigned source build is fine until Task 8).

## Review Focus

1. Disabling PLAYER while a track is playing: playback stops, the status bar indicator hides, nothing crashes; re-enabling later plays again. (Task 2, manual check in sim; Task 8 on hardware.)
2. A settings write that does not stick (NVS full or failing): the switch flips back and `on_disable`/`on_enable` is not called. `hal_cfg_set_i32` returns void, so `deck_app_set_enabled` reads the key back to detect it. (Task 1 test `set_enabled_reverts_when_write_lost`.)
3. Upgrading from v1.4 with no `app_*` keys: every current app stays on, RF WATCH starts off. (Task 1 test `defaults_without_keys`.)
4. Keyboard selection on a greyed tile and Enter: behaves like a tap (opens SETTINGS > APPLICATIONS on that app), never launches the disabled app. (Task 4 test via headless `--key` script.)
5. Alt+7..9 on a six-tile home and Alt+letter for a disabled app from inside another app: no effect, no crash. (Task 3 headless script.)

---

### Task 1: App model, enabled state and the hint rule

**Files:**
- Modify: `components/deck_core/include/deck_app.h`, `components/deck_core/deck_app.c`
- Create: `components/deck_core/include/deck_hint.h`, `components/deck_core/deck_hint.c`
- Create: `sim/apps_test.c`; Modify: `sim/CMakeLists.txt` (target `apps_test`, `add_test(NAME app_model COMMAND apps_test)`), `components/deck_core/CMakeLists.txt` (add `deck_hint.c`)

**Interfaces:**
- Produces, in `deck_app.h`, new `struct deck_app` fields: `const char *id; const char *desc; bool optional; bool default_on; void (*on_enable)(deck_app_t *self); void (*on_disable)(deck_app_t *self); void (*on_panel)(deck_app_t *self, lv_obj_t *section);` (`on_tile` stays until Task 5).
- Produces functions:
  - `bool deck_app_enabled(const deck_app_t *app);` core always true.
  - `bool deck_app_set_enabled(deck_app_t *app, bool on);` false when the write did not stick (state and hooks unchanged).
  - `deck_app_t *deck_app_by_id(const char *id);`
  - `void deck_app_boot(void);` calls `on_enable` for every enabled optional app; call once after registration.
  - `deck_app_by_hotkey()` now skips disabled apps.
- Produces, in `deck_hint.h` (no LVGL or HAL includes): `int deck_hint_slot(const bool *disabled, const bool *offline, size_t n);` index of the lowest slot with `disabled && !offline`, or -1.

- [ ] **Step 1: Write the failing tests in `sim/apps_test.c`**

Same style as `sim/calc_test.c` (count, print FAIL lines, exit non-zero). The file defines its own `hal_cfg_get_i32` / `hal_cfg_set_i32` over a small key/value array, plus a `s_drop_writes` flag that makes `set` a no-op. Two stub apps (`core`, `opt` with `default_on = true`, `opt2` with `default_on = false`, hotkeys set) whose hooks count calls.

```c
/* deck_hint_slot */
expect(deck_hint_slot((bool[]){0,0,0}, (bool[]){0,0,0}, 3) == -1, "none disabled");
expect(deck_hint_slot((bool[]){0,0,1,0,1}, (bool[]){0,0,0,0,0}, 5) == 2, "lowest disabled");
expect(deck_hint_slot((bool[]){0,0,0,0,1}, (bool[]){0,0,0,0,0}, 5) == 4, "after re-enable");
expect(deck_hint_slot((bool[]){1,0,1}, (bool[]){1,0,0}, 3) == 2, "offline skipped");
/* defaults_without_keys */
expect(deck_app_enabled(&core) && deck_app_enabled(&opt) && !deck_app_enabled(&opt2), "defaults");
/* set_enabled persists and calls hooks once */
expect(deck_app_set_enabled(&opt, false) && !deck_app_enabled(&opt) && opt_disable_calls == 1, "disable");
expect(hal_cfg_get_i32("app_opt", -1) == 0, "key written");
/* hotkey and id lookups skip / find */
expect(deck_app_by_hotkey(opt.hotkey) == NULL, "disabled hotkey ignored");
expect(deck_app_by_id("opt") == &opt, "by id");
/* set_enabled_reverts_when_write_lost */
s_drop_writes = true;
expect(!deck_app_set_enabled(&opt, true) && !deck_app_enabled(&opt) && opt_enable_calls == 0, "write lost");
s_drop_writes = false;
/* core cannot be disabled */
expect(!deck_app_set_enabled(&core, false) && deck_app_enabled(&core), "core stays on");
/* boot enables only enabled optional apps */
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -S sim -B sim/build && cmake --build sim/build --target apps_test`
Expected: compile errors (`deck_hint_slot`, `deck_app_enabled` undeclared).

- [ ] **Step 3: Implement**

`deck_hint.c`: the loop described by its signature. `deck_app.c`: `APP_MAX 20`; key is `snprintf("app_%s", id)`; `deck_app_register` rejects an optional app with no `id`, an `id` longer than 11 characters, or a 13th optional app (log via `LV_LOG_ERROR`). `deck_app_set_enabled`: no-op false for core; write, read back, return false if it differs; otherwise call the matching hook and return true.

- [ ] **Step 4: Run tests**

Run: `cmake --build sim/build --target apps_test && sim/build/apps_test`
Expected: `all N passed`, exit 0. Also `cmake --build sim/build --target deck_sim` still builds.

- [ ] **Step 5: Commit** `App model: optional apps, enabled state in NVS, the settings hint rule`

### Task 2: Give every app its metadata; PLAYER and RF WATCH changes

**Files:**
- Modify: `apps/app_term/app_term.c`, `apps/app_notes/app_notes.c`, `apps/app_calc/app_calc.c`, `apps/app_files/app_files.c`, `apps/app_sys/app_sys.c` (core: `id` and `desc` only)
- Modify: `apps/app_weather/app_weather.c`, `apps/app_player/app_player.c`, `apps/app_net/app_net.c`, `apps/app_rf/app_rf.c` (optional)
- Modify: `components/deck_audio/player.c` (`player_init` idempotent), `main/main.c`, `sim/main.c`

**Interfaces:**
- Consumes: Task 1 fields and `deck_app_boot()`.
- Produces: ids `term`, `notes`, `calc`, `files`, `sys`, `weather`, `player`, `net`, `rf`.

- [ ] **Step 1: Fill the fields**

| app | optional | default_on | desc |
|---|---|---|---|
| WEATHER | yes | yes | `Forecast and the home panel widget` |
| PLAYER | yes | yes | `MP3 player, Alt+P, status bar indicator` |
| NET TOOLS | yes | yes | `Wi-Fi scan, ping, LAN sweep, speed test` |
| RF WATCH | yes | no | `Receive-only radio monitor (coming in v1.5)` |

RF WATCH: `.hotkey = HID_LETTER('X')`. Core apps get a `desc` too (shown in APPLICATIONS): TERMINAL `SSH and AI chat`, NOTES `Markdown notes`, CALC `Calculator`, FILES `File browser and Wi-Fi portal`, SETTINGS `Display, network, time, updates`.

- [ ] **Step 2: PLAYER hooks**

`on_enable` calls `player_init()`; `on_disable` calls `player_stop()`. Make `player_init()` return early when already initialised (static flag), and `player_stop()` a no-op before `player_init()` has run (PLAYER can start disabled, so `on_disable` may come first). Remove `player_init()` from `app_main` and from `sim/main.c`; both call `deck_app_boot()` right after the last `deck_app_register()`.

- [ ] **Step 3: Gate Alt+P**

In `deck_shell.c`, Alt+P only toggles when `deck_app_enabled(deck_app_by_id("player"))`.

- [ ] **Step 4: Verify**

Run: `cmake --build sim/build --target deck_sim apps_test && sim/build/apps_test` then `sim/build/deck_sim --headless --no-boot --set app_player=0 --key 500:alt+p --shot 1500:/tmp/p.ppm`
Expected: tests pass; sim runs with no player task log line and no crash. Firmware `idf.py build` succeeds.

- [ ] **Step 5: Commit** `Apps declare core or optional; PLAYER starts from on_enable; RF WATCH on Alt+X`

### Task 3: Home model: fixed tiles and the APPS group

**Files:**
- Modify: `components/deck_core/include/deck_home.h`, `components/deck_core/deck_home.c`, `components/deck_core/deck_shell.c` (Alt+1..9 handling unchanged, shortcut list text), `main/main.c`, `sim/main.c`
- Modify: `components/deck_ui/include/deck_icons.h`, `tools/subset_icons.sh`, `assets/fonts/icons.ttf`

**Interfaces:**
- Produces: `#define DECK_GROUP_APPS 12`; `deck_group_t *deck_home_add_apps(const char *name, const char *icon);` adds a home tile whose group holds every registered optional app in registration order (call after registration); `deck_group_t *deck_home_apps(void);` returns that group (NULL before it is added), used by the Alt+A handler. `deck_home_add_group` and `deck_group_add` stay for this one caller and are no longer used by `main.c`.
- Produces: `ICON_APPS` (MDI `apps`, codepoint F003B).

- [ ] **Step 1: Icon**

Add `F003B  # apps` to `tools/subset_icons.sh` and `#define ICON_APPS "\xF3\xB0\x80\xBB" /* F003B */` to `deck_icons.h`. Rerun `tools/subset_icons.sh <materialdesignicons-webfont.ttf>` (from the `@mdi/font` package, same major version as the other glyphs) and commit the new `assets/fonts/icons.ttf`.

- [ ] **Step 2: Home registration**

`main.c` and `sim/main.c`: replace the three groups and two `deck_home_add_app` calls with, in order: `deck_home_add_app(app_term()); deck_home_add_app(app_notes()); deck_home_add_app(app_sys()); deck_home_add_app(app_calc()); deck_home_add_app(app_files()); deck_home_add_apps("APPS", ICON_APPS);`. In `deck_shell.c`'s Alt handler, before `deck_app_by_hotkey()`: `HID_A` opens `deck_home_apps()` with `deck_shell_open_group()`.

- [ ] **Step 3: Shortcut list**

`deck_shell_shortcuts()` text: `ALT+1..6    open a home tile`, `ALT+A       apps`, `ALT+T N S C F W M R   jump to an app` (letters of core apps plus enabled optional apps is fine as static text).

- [ ] **Step 4: Verify (Review Focus 5)**

Run: `sim/build/deck_sim --headless --no-boot --key 500:alt+8 --key 900:alt+a --shot 1500:/tmp/apps.ppm --set app_net=0 --key 2000:esc --key 2200:alt+t --key 2600:alt+r --shot 3200:/tmp/term.ppm`
Expected: no crash; `/tmp/apps.ppm` shows the APPS group (old tile style is fine at this point); `/tmp/term.ppm` shows TERMINAL, not NET TOOLS.

- [ ] **Step 5: Commit** `Home: six fixed tiles, APPS group of every optional app, ICON_APPS`

### Task 4: Launcher tiles: simplified, fixed grids, greyed tiles, the hint

**Files:**
- Modify: `components/deck_core/deck_launcher.c`, `components/deck_core/deck_launcher.h`

**Interfaces:**
- Consumes: `deck_app_enabled`, `deck_hint_slot`, `deck_home_add_apps` group.
- Produces: `void deck_launcher_set_manage(void (*fn)(const deck_app_t *focus));` called for a tap or Enter on a greyed tile. Home leaves a left column of `PANEL_W 360` px empty for Task 5 (an `lv_obj_t *` created by `deck_live_create`, stubbed as an empty panel here).

- [ ] **Step 1: Tile content**

`build_tile()` takes `bool is_disabled`. Normal: icon, name, Alt+letter top right (pink, `g_font.mono_s`), pink accent bar. Remove number, chip/badge, tagline, module list, footer, status word. Disabled: icon and name in `g_pal.dim`-darker grey, accent bar grey, `OFF` instead of the letter. Offline (`eta` set): keep the hazard look regardless of enabled state.

- [ ] **Step 2: Grids**

Home: panel column then a 3x2 grid in the remaining width (`TILE_H` area 470 px, `TILE_GAP` 18). APPS: 3x2 when the group has up to 6 apps, otherwise 4x3, sized by group count (enabled or not). Arrow keys move in 2D over tiles only (the panel is not in the focus order); Enter opens; Enter or tap on a greyed tile calls the manage callback with that app.

Add a sim-only flag `--stub-apps N` to `sim/main.c`: registers N extra optional apps (`id` `stub1`.., name `STUB n`, `ICON_TOOLS`, `default_on` true, empty screen) before `deck_home_add_apps`, so the 4x3 layout can be checked before real apps exist.

- [ ] **Step 3: Hint overlay**

On the APPS screen, build `disabled[]`/`offline[]` from the group and put an overlay on `deck_hint_slot(...)`: two centred labels `DISABLED` (`g_font.disp_s`) and `ENABLE IN SETTINGS` (`g_font.mono_s`), both in a muted color one step brighter than the greyed tile, over a translucent scrim (`LV_OPA_60` of the background). No accent fill.

- [ ] **Step 4: Verify (Review Focus 4)**

Screenshots: `sim/build/deck_sim --headless --no-boot --shot 1500:/tmp/home.ppm --key 2000:alt+a --shot 2600:/tmp/apps_all_on.ppm` and with `--set app_player=0 --set app_net=0` for `/tmp/apps_hint.ppm`, plus `--key 3000:right --key 3200:enter --shot 3800:/tmp/greyed_enter.ppm` with the selection on PLAYER.
Expected: home shows six simplified tiles and an empty left panel; APPS shows WEATHER, PLAYER (greyed, hint), NET TOOLS (greyed), RF WATCH (hazard); Enter on greyed PLAYER does not open PLAYER (manage callback not wired yet: no-op). Then `--stub-apps 8 --key 500:alt+a --shot 1200:/tmp/apps_12.ppm`: a 4x3 grid of 12 tiles, none clipped.

- [ ] **Step 5: Commit** `Launcher: icon and name tiles, home 3x2 grid, APPS fixed slots, greyed tiles and the settings hint`

### Task 5: The live panel and WEATHER's section

**Files:**
- Create: `components/deck_core/deck_live.c`, `components/deck_core/include/deck_live.h`; add to `components/deck_core/CMakeLists.txt` and `sim/CMakeLists.txt`
- Modify: `components/deck_core/deck_launcher.c` (use it), `apps/app_weather/app_weather.c` (`on_tile` becomes `on_panel`), `components/deck_core/include/deck_app.h` (remove `on_tile`)

**Interfaces:**
- Produces: `lv_obj_t *deck_live_create(lv_obj_t *parent, int32_t w, int32_t h);`
- Consumes: `on_panel` from Task 1.

- [ ] **Step 1: Panel**

Walk registered apps in order; for each enabled optional app with `on_panel` (at most 3), create a section (title label in `g_pal.accent`, `g_font.mono_s`, the app name; then a body box ~140 px tall passed to `on_panel`). Each section is clickable and launches its app. No sections: a centred `DECK//OS` in dim display font.

- [ ] **Step 2: WEATHER**

Port the tile widget to `on_panel(self, section)`: icon, temperature, condition, place, H/L. Keep its 5 s refresh timer hung off `section` so it dies with the home screen.

- [ ] **Step 3: Verify**

Screenshots: home with defaults (`/tmp/home_panel.ppm`), with `--set app_weather=0` (`/tmp/home_empty_panel.ppm`). Firmware `idf.py build`.
Expected: panel shows WEATHER; with it off, the DECK//OS mark. No leftover `on_tile` references (`grep -rn on_tile components apps` is empty).

- [ ] **Step 4: Commit** `Home live panel: on_panel sections from enabled apps; WEATHER moves into it`

### Task 6: SETTINGS > APPLICATIONS

**Files:**
- Modify: `apps/app_sys/app_sys.c`, `apps/app_sys/app_sys.h`, `main/main.c`, `sim/main.c`

**Interfaces:**
- Consumes: `deck_app_set_enabled`, `deck_app_get/count`, `deck_launcher_set_manage`.
- Produces: `void app_sys_open_applications(const deck_app_t *focus);` registered with `deck_launcher_set_manage()` in both mains.

- [ ] **Step 1: Rename and tabs**

`.name = "SETTINGS"`, keep `.hotkey = HID_LETTER('S')` and `ICON_COG`. Add a tab row (`small_button` tabs as in `app_files.c`) `SETTINGS | APPLICATIONS`; the SETTINGS tab is the existing three-column body unchanged.

- [ ] **Step 2: APPLICATIONS list**

One scrollable column: core apps first, then optional apps in registration order. Row: icon, name, `desc` (dim), and on the right `add_switch()` for optional apps or a dim `CORE` label. Switch change calls `deck_app_set_enabled`; on false, put the switch back and set the row's desc to `Could not save. Try again.` until the next change. After a successful disable, if `deck_shell_current() == app`, call `deck_shell_home()` (not reachable from SETTINGS today; it guards future callers).

- [ ] **Step 3: Open-on-app**

`app_sys_open_applications(focus)` launches SETTINGS on the APPLICATIONS tab with `focus`'s switch focused and scrolled into view.

- [ ] **Step 4: Verify**

Headless: open APPS, tap the greyed PLAYER tile (`--tap` at its centre), screenshot (`/tmp/applications.ppm`): APPLICATIONS tab with PLAYER's switch focused. Toggle it with `--key` enter, Esc twice, Alt+A, screenshot: PLAYER now normal, hint moved to the next greyed app or gone.

- [ ] **Step 5: Commit** `SETTINGS: APPLICATIONS tab with a switch per optional app; greyed tiles open it`

### Task 7: Docs and screenshots

**Files:**
- Modify: `README.md`, `ROADMAP.md`, `docs/screenshots/home.png`, `docs/screenshots/system.png`; Create: `docs/screenshots/apps.png`, `docs/screenshots/applications.png`; Delete: `docs/screenshots/tools.png`, `docs/screenshots/media.png`

- [ ] **Step 1:** Render the screenshots headless and convert `.ppm` to `.png` (`magick in.ppm out.png`).
- [ ] **Step 2:** README: home layout, APPS, SETTINGS > APPLICATIONS, shortcut table (Alt+1..6, Alt+A, RF WATCH on Alt+X), drop group screenshots. ROADMAP: a `v1.5: App toggles and the new home` section with the hardware checkpoint as an open item.
- [ ] **Step 3:** `grep -rn $'\xe2\x80\x94' README.md ROADMAP.md docs/superpowers` (searches for the em dash, U+2014) prints nothing. Commit `Docs: new home, APPS and APPLICATIONS`.

### Task 8: Hardware check and beta

- [ ] **Step 1: Backup.** With the Tab5 on USB: `esptool.py --chip esp32p4 -p /dev/cu.usbmodem* read_flash 0 0x1000000 ~/tab5-backups/tab5-$(date +%Y%m%d-%H%M).bin`. Expected: a 16 MB file. Restore command if needed: `esptool.py --chip esp32p4 -p /dev/cu.usbmodem* write_flash 0 <file>`.
- [ ] **Step 2: Flash and check.** `idf.py -p /dev/cu.usbmodem* flash monitor`. Check: home looks like the sim; toggle each optional app; WEATHER off stops fetch log lines; PLAYER off while playing stops audio and the indicator, Alt+P does nothing (Review Focus 1); reboot keeps the toggles.
- [ ] **Step 3: Beta.** Write notes, then `tools/release.sh 1.5.0-beta.1 <notes.md> "v1.5.0-beta.1: app toggles and a cleaner home (beta)" --prerelease`. Expected: `released v1.5.0-beta.1`, three assets on the release.
