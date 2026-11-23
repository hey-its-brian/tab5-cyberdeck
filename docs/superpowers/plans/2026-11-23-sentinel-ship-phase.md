# SENTINEL Ship Phase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `cl/sentinel-useful-93ca39` deliverable over the air as v1.5.0-beta.2: a WIRELESS TOOLS setting (NET TOOLS or SENTINEL), the usability review finished inside SENTINEL, the open stability items settled, sim tests and the CI command green, a signed release candidate built locally.

**Architecture:** One boot-time choice (`wireless`, 0 or 1) decides which of two apps is registered and which home grid is built; nothing SENTINEL-related runs when NET TOOLS is chosen. NET TOOLS is the SENTINEL frame started without a log screen. The notice service gains an ALERT level with its rules in a pure file so they can be host-tested. SENTINEL's screens gain a `hold` hook so routes other than BACK (the alert line, a link drop) cannot end a drive, a capture or a burst list without asking.

**Tech Stack:** C99, ESP-IDF 5.5 (`~/esp/esp-idf-v5.5`), LVGL 9, the desktop simulator (`sim/`, SDL2, ctest), `tools/check.sh`, `tools/release.sh`.

**Spec:** `docs/superpowers/specs/2026-11-23-sentinel-ship-phase-design.md`

## Global Constraints

- No em dashes in any file (grep the changed files for the character U+2014 before each commit). No Claude or co-author lines in commits. Nothing pushed, tagged or released.
- Every commit leaves `tools/check.sh` green (sim configure, sim build, ctest).
- NET TOOLS mode: `app_sentinel()` is never registered, so `sentinel_start()`, `hal_ext5v(true)`, the `sentinel` and `sentinel_log` tasks and the E07 lines are never touched. The home grid is exactly `TERMINAL, NOTES, SETTINGS, CALC, FILES, APPS`.
- SENTINEL mode: the home grid is `TERMINAL, NOTES, SETTINGS, APPS, FILES, SENTINEL`; CALC is optional under APPS.
- `wireless` defaults to `hal_cfg_get_i32("app_rf", 0) != 0`.
- Nothing on a SENTINEL screen under 20 px; SENTINEL draws no data in `g_pal.accent`.
- Text on screen is in the project's voice: capitals for labels and states, sentences for explanations, no chip names, no jargon.
- Version string `1.5.0-beta.2` in both `CMakeLists.txt` (`PROJECT_VER`) and `components/deck_core/include/deck_shell.h` (`DECK_VERSION`), set by `release.sh`, not by hand.

## Review Focus

1. A deck updating from v1.5.0-beta.1 with `app_rf` unset and `app_net=0` (NET TOOLS switched off): NET TOOLS mode, NET TOOLS greyed under APPS, nothing on the bus. Test: `sim_list_net_off` in Task 1.
2. The WIRELESS TOOLS button pressed twice (back to the saved value): no RESTART button shown, nothing changes. Test: Task 3 step 1.
3. An ALERT posted while a WARN banner is up, then a WARN posted one second later: the ALERT banner stays, the WARN is counted only. Test: `alert_keeps_banner_over_warn` in Task 4.
4. The link drops in the middle of a drive with the WARDRIVE screen open, and returns a minute later: the drive is still on, rows resume, no dialog appeared. Test: Task 7 step 1 (sim scene with `SIM_RF_DROP`).
5. The alert line's L pressed on WARDRIVE while a drive runs, CANCEL chosen: still on WARDRIVE, the drive still on. Test: Task 7 step 1.

---

### Task 1: The wireless choice, both mains, `--list`, mode tests

**Files:**
- Create: `components/deck_core/include/deck_wireless.h`, `components/deck_core/deck_wireless.c`
- Modify: `main/main.c:50-72`, `sim/main.c:252-370`, `sim/CMakeLists.txt` (add `deck_wireless.c` to `deck_sim`, two tests), `components/deck_hal/include/deck_hal.h`, `components/deck_hal/hal_tab5.cpp`, `sim/hal_sim.c`, `apps/app_sentinel/CMakeLists.txt` (nothing yet; Task 2 adds `app_net.c`)
- Test: ctest cases `sim_list_net`, `sim_list_sentinel`, `sim_list_net_off` on `deck_sim --headless --list`

**Interfaces:**
- Produces: `bool deck_wireless_sentinel(void);` (reads `wireless`, default from `app_rf`), `void hal_restart(void);` (device: `esp_restart()`; sim: `exit(0)`), the `--list` option, and `deck_app_t *app_net(void);` declared in `apps/app_sentinel/app_sentinel.h`. In this task `app_net()` is a stub with the right name, id and flags and an `on_start` that returns false; Task 2 gives it screens. The three `sim_list` tests pass with the stub.

- [ ] **Step 1: Add the tests**

In `sim/CMakeLists.txt` after `add_test(NAME app_model ...)`:

```cmake
add_test(NAME sim_list_net COMMAND deck_sim --headless --no-boot --list --set wireless=0)
set_tests_properties(sim_list_net PROPERTIES PASS_REGULAR_EXPRESSION
    "apps: term notes calc weather player files net sys\nhome: TERMINAL NOTES SETTINGS CALC FILES APPS\ngroup: WEATHER PLAYER \"NET TOOLS\"\n")
add_test(NAME sim_list_sentinel COMMAND deck_sim --headless --no-boot --list --set wireless=1)
set_tests_properties(sim_list_sentinel PROPERTIES PASS_REGULAR_EXPRESSION
    "apps: term notes calc weather player files sentinel sys\nhome: TERMINAL NOTES SETTINGS APPS FILES SENTINEL\ngroup: WEATHER PLAYER CALC\n")
add_test(NAME sim_list_net_off COMMAND deck_sim --headless --no-boot --list --set app_net=0)
set_tests_properties(sim_list_net_off PROPERTIES PASS_REGULAR_EXPRESSION
    "apps: term notes calc weather player files net sys\nhome: TERMINAL NOTES SETTINGS CALC FILES APPS\ngroup: WEATHER PLAYER \"NET TOOLS\" \\(off\\)\n")
```

`--list` prints exactly three lines: `apps:` the registered ids in order; `home:` the home tile names; `group:` the APPS group's app names, a name with a space in quotes, ` (off)` after a disabled one. Then it exits 0 before `deck_shell_start`.

- [ ] **Step 2: Run them to see them fail**

Run: `cmake -S sim -B sim/build && cmake --build sim/build -j 8 && ctest --test-dir sim/build -R sim_list`
Expected: all three FAIL (`--list` unknown, no output).

- [ ] **Step 3: Implement `deck_wireless_sentinel()` and `hal_restart()`**

`deck_wireless.c`: `return hal_cfg_get_i32("wireless", hal_cfg_get_i32("app_rf", 0) != 0) != 0;`. `hal_restart()` in `hal_tab5.cpp` calls `esp_restart()`; in `hal_sim.c` it calls `exit(0)`.

- [ ] **Step 4: Both mains choose at boot**

In `main/main.c` and `sim/main.c`, replace the registration and home blocks with the same code:

```c
bool sentinel = deck_wireless_sentinel();
app_calc()->optional = sentinel; /* CALC is a home tile without SENTINEL, under APPS with it */
deck_app_register(app_term());
deck_app_register(app_notes());
deck_app_register(app_calc());
deck_app_register(app_weather());
deck_app_register(app_player());
deck_app_register(app_files());
deck_app_register(sentinel ? app_sentinel() : app_net());
deck_app_register(app_sys());
/* ... deck_app_boot(), sync backend as before ... */
deck_home_add_app(app_term());
deck_home_add_app(app_notes());
deck_home_add_app(app_sys());
if (sentinel) {
    deck_home_add_apps("APPS", ICON_APPS);
    deck_home_add_app(app_files());
    deck_home_add_app(app_sentinel());
} else {
    deck_home_add_app(app_calc());
    deck_home_add_app(app_files());
    deck_home_add_apps("APPS", ICON_APPS);
}
```

Until Task 2 exists, `app_net()` is a stub in `apps/app_sentinel/app_net.c` returning a `deck_app_t` with `.name = "NET TOOLS", .tagline = "WI-FI + LAN", .icon = ICON_LAN, .hotkey = HID_LETTER('R'), .id = "net", .desc = "Wi-Fi scan, ping, LAN sweep, speed test", .optional = true, .default_on = true` and an `on_start` that returns false. Add `app_net.c` to `apps/app_sentinel/CMakeLists.txt` SRCS.

In `sim/main.c` add `--list` (sets `list = true`); after the home block, when `list` is set, print the three lines and `return 0`.

- [ ] **Step 5: Run the tests and the whole suite**

Run: `tools/check.sh`
Expected: `PASS`, 24 tests.

- [ ] **Step 6: Commit**

```bash
git add components/deck_core/include/deck_wireless.h components/deck_core/deck_wireless.c components/deck_hal main/main.c sim apps/app_sentinel/CMakeLists.txt apps/app_sentinel/app_net.c apps/app_sentinel/app_sentinel.h
git commit -m "WIRELESS TOOLS: NET TOOLS or SENTINEL chosen at boot; the sim lists what each mode registers"
```

---

### Task 2: NET TOOLS as an app on the SENTINEL frame

**Files:**
- Modify: `apps/app_sentinel/app_net.c` (the stub becomes the app), `apps/app_sentinel/sn_frame.c:960-972` (`sn_frame_start`), `:873-916` (`show`: chip and strip optional), `:898` (LEAVE label), `apps/app_sentinel/sn_ui.h`, `apps/app_sentinel/sn_wifi.c:616` (export `sn_networks_screen`), `apps/app_sentinel/sn_net.c:556-563`
- Test: `sim_list_net` (now passes), plus headless screenshots `sim/build/net_home.ppm`, `sim/build/net_ping.ppm`

**Interfaces:**
- Consumes: `sn_frame_start(parent, home, log)` with `log == NULL` meaning: no mode chip, no alert line, the home page's button says `LEAVE <home->name>`.
- Produces: `extern const sn_screen_t sn_networks_screen;` (the NETWORKS leaf, moved out of `s_kids` in sn_wifi.c and referenced from both menus) and `extern const sn_screen_t sn_net_home;` in `sn_net.c`: `.name = "NET TOOLS"`, `.purpose = "Test the network the deck has joined: networks, hosts, devices, speed."`, kids `{&sn_networks_screen, &s_ping, &s_dns, &s_trace, &s_port, &s_lan, &s_speed}` (7). The SENTINEL `sn_net_menu` keeps its six.

- [ ] **Step 1: Frame without a log**

In `show()`: build `s_fr.chip` only when `s_fr.log`; build the strip block only when `s_fr.log` (leave `s_fr.strip` NULL otherwise). Guard `strip_update()`, `strip_set()`, `strip_open()` and the `L` key with `if (!s_fr.strip) return;`. The BACK label at depth 1: `"LEAVE SENTINEL"` becomes a buffer `"LEAVE %s"` with `s_fr.stack[0]->name`. `tiles_update()` already works without a board (every NET TOOLS kid lacks `SN_BOARD`).

- [ ] **Step 2: The app**

`app_net.c`: `on_start` calls `sn_frame_start(parent, &sn_net_home, NULL)`; `on_exit` nothing (no lease); `on_stop` `sn_frame_stop()`; `on_key` `sn_frame_key`; `on_back` `sn_frame_back`; no `on_enable`, no `on_panel`. The description and names from Task 1.

- [ ] **Step 3: Screenshots**

Run: `sim/build/deck_sim --headless --no-boot --wifi --set wireless=0 --key 500:alt+r --shot 1500:sim/build/net_home.ppm --key 2000:2 --shot 3000:sim/build/net_ping.ppm`
Expected: `net_home.ppm` shows `NET TOOLS` as the crumb, seven tiles, no mode chip, no alert line, a `LEAVE NET TOOLS` button; `net_ping.ppm` shows `NET TOOLS  >  PING`.

- [ ] **Step 4: `tools/check.sh`**

Expected: `PASS`, `sim_list_net` now passing.

- [ ] **Step 5: Commit**

```bash
git add apps/app_sentinel
git commit -m "NET TOOLS is the SENTINEL frame with the network screens, and no board"
```

---

### Task 3: The WIRELESS TOOLS row in SETTINGS

**Files:**
- Modify: `apps/app_sys/app_sys.c` (`build_applications`, a new row above the optional apps; a `wireless_clicked` and `restart_clicked`)
- Test: headless screenshots `sim/build/settings_wireless.ppm`, `sim/build/settings_wireless_changed.ppm`

**Interfaces:**
- Consumes: `deck_wireless_sentinel()`, `hal_restart()`, `deck_modal_confirm_danger(title, message, yes_label, cb, user)`.

- [ ] **Step 1: The row**

A row above the optional apps' switches, 64 px like `app_row`: title `WIRELESS TOOLS`, description (`g_font.mono_s`, `g_pal.dim`, like the other rows) `"NET TOOLS: Wi-Fi scan, ping, DNS, traceroute, LAN devices and a speed test on the deck's own Wi-Fi. SENTINEL: the radio board suite, with those tools inside it. Takes effect when the deck restarts."`, and a `small_button` at the right reading `NET TOOLS` or `SENTINEL` (from `deck_wireless_sentinel()`). Clicking it writes `hal_cfg_set_i32("wireless", !current)` and updates the label. A second `small_button` `RESTART` is shown only while the saved value differs from the value at build time (`s_wireless_at_build`); clicking it opens `deck_modal_confirm_danger("RESTART THE DECK?", "The deck restarts now and comes back with the other wireless tools.", "RESTART", ...)`, and yes calls `hal_restart()`.

- [ ] **Step 2: Screenshots**

Run: `sim/build/deck_sim --headless --no-boot --set wireless=0 --key 500:alt+s --key 1000:tab --key 1200:enter --shot 2000:sim/build/settings_wireless.ppm`
Expected: the APPLICATIONS tab with the WIRELESS TOOLS row above WEATHER, button `NET TOOLS`, no RESTART. Then tap the button (`--tap` at its coordinates) and shoot `settings_wireless_changed.ppm`: button `SENTINEL`, RESTART beside it. Tap again: RESTART gone.

- [ ] **Step 3: `tools/check.sh`, commit**

```bash
git add apps/app_sys/app_sys.c
git commit -m "SETTINGS > APPLICATIONS: WIRELESS TOOLS, NET TOOLS or SENTINEL, with a restart"
```

---

### Task 4: Notice rules and the ALERT level

**Files:**
- Create: `components/deck_core/include/deck_notice_rules.h`, `components/deck_core/deck_notice_rules.c`, `sim/notice_test.c`
- Modify: `components/deck_core/include/deck_notice.h`, `components/deck_core/deck_notice.c`, `components/deck_core/deck_statusbar.c:189-196`, `apps/app_sentinel/app_sentinel.c:106` and `:77`, `sim/CMakeLists.txt`
- Test: `sim/notice_test.c` (ctest `notice_rules`)

**Interfaces:**
- Produces, in `deck_notice_rules.h` (no LVGL):

```c
typedef enum { DECK_NOTICE_INFO, DECK_NOTICE_WARN, DECK_NOTICE_ALERT } deck_notice_level_t;
#define DECK_NOTICE_BANNER_MS       4000
#define DECK_NOTICE_ALERT_BANNER_MS 20000
#define DECK_NOTICE_QUIET_MS        10000
typedef struct {
    bool                banner_up;
    deck_notice_level_t banner_level;
    uint32_t            shown_at;   /* when the current or last banner went up */
    bool                shown_once;
    unsigned            unseen, unseen_alerts;
} deck_notice_rules_t;
typedef enum { DECK_NOTICE_NONE, DECK_NOTICE_BANNER, DECK_NOTICE_BANNER_WAKE } deck_notice_act_t;
/* What to do with a notice. Counts it (WARN and ALERT), decides the banner. */
deck_notice_act_t deck_notice_rules_post(deck_notice_rules_t *r, deck_notice_level_t level, bool app_on_screen, bool screen_dark, uint32_t now_ms);
void     deck_notice_rules_banner_gone(deck_notice_rules_t *r);
void     deck_notice_rules_seen(deck_notice_rules_t *r);
uint32_t deck_notice_rules_banner_ms(deck_notice_level_t level);
```
  Rules: INFO does nothing. WARN and ALERT add to `unseen`; ALERT also to `unseen_alerts`. A banner: never while `app_on_screen`. WARN: none while `screen_dark`; none within `DECK_NOTICE_QUIET_MS` of `shown_at`; none while an ALERT banner is up. ALERT: always a banner, `BANNER_WAKE` when `screen_dark`; replaces whatever is up.
- `deck_notice.h`: the enum moves to the rules header; adds `unsigned deck_notice_unseen_alerts(void);` and `const char *deck_notice_latest(void);` (the text of the newest WARN or ALERT, "" when none, cleared by `deck_notice_seen()`).

- [ ] **Step 1: Write `sim/notice_test.c`**

Tests, each a function with `expect()` like `apps_test.c`: `info_is_nothing`, `warn_counts_and_banners`, `warn_quiet_period` (a second WARN 5 s later: `NONE`, unseen 2), `warn_dark_counts_only`, `warn_skipped_on_screen`, `alert_ignores_quiet` (WARN at 0, ALERT at 1 s: `BANNER`), `alert_wakes_dark` (`BANNER_WAKE`), `alert_keeps_banner_over_warn` (ALERT at 0, WARN at 1 s: `NONE`, unseen 2, `banner_level == ALERT`), `banner_ms` (4000 and 20000), `seen_clears_both_counts`. Register it in `sim/CMakeLists.txt` as `notice_test` linking only `deck_notice_rules.c`.

- [ ] **Step 2: Run, expect failures to compile; implement the rules; run again**

Run: `cmake --build sim/build -j 8 && ctest --test-dir sim/build -R notice_rules --output-on-failure`
Expected: PASS.

- [ ] **Step 3: `deck_notice.c` draws what the rules say**

Replace `s_unseen`, `s_shown_at`, `s_shown_once` with a `deck_notice_rules_t`. `deck_notice_post()` calls the rules with `deck_shell_current() == app`, `deck_shell_sleeping() || deck_saver_active()`, `lv_tick_get()`; on `BANNER` or `BANNER_WAKE` it shows the banner; on `BANNER_WAKE` it first calls `deck_saver_wake()` and `deck_shell_wake()` (new in `deck_shell.h`: `void deck_shell_wake(void);`, the existing `wake_async` path, a no-op when not sleeping). The banner for an ALERT: outline and tag in `g_pal.danger`, the tag text `ALERT`, hidden after `deck_notice_rules_banner_ms(level)`. `hide_cb` and `banner_close` call `deck_notice_rules_banner_gone`. Keep `s_latest[64]`.

- [ ] **Step 4: The count and SENTINEL's posts**

`deck_statusbar.c`: the count is `g_pal.danger` when `deck_notice_unseen_alerts()` is non-zero, `g_pal.warn` otherwise. `app_sentinel.c:106`: post `sn_level_of(...) == SN_ALERT ? DECK_NOTICE_ALERT : DECK_NOTICE_WARN`. `app_sentinel.c:77`: delete the `deck_notice_seen()` call in `board_apply(false)`.

- [ ] **Step 5: Sim check and commit**

Run: `SIM_RF_JAM=20 sim/build/deck_sim --headless --no-boot --set wireless=1 --set sn_board=1 --key 500:alt+n --shot 30000:sim/build/alert_banner.ppm`
Expected: NOTES on screen with a red ALERT banner, the status bar count red. `tools/check.sh`: PASS, 25 tests.

```bash
git add components/deck_core apps/app_sentinel/app_sentinel.c sim/notice_test.c sim/CMakeLists.txt
git commit -m "Notices have an ALERT level: it replaces a warning's banner, stays 20 s, and wakes the screen"
```

---

### Task 5: The saver shows the warnings

**Files:**
- Modify: `components/deck_core/deck_saver.c:104-131` (`open_saver`, `update`)
- Test: headless screenshot `sim/build/saver_warnings.ppm`

- [ ] **Step 1: Two lines under the date**

In `open_saver()` add `s_warn = deck_label(s_box, g_font.mono_l, g_pal.warn, "")` after `s_date`. In `update()`: with `deck_notice_unseen()` zero the label is hidden; otherwise `"%u WARNING%s.  LATEST: %s"` with `deck_notice_latest()` (upper-cased with the saver's own copy), in `g_pal.danger` when `deck_notice_unseen_alerts()` is non-zero.

- [ ] **Step 2: Screenshot**

Run: `SIM_RF_JAM=20 sim/build/deck_sim --headless --no-boot --set wireless=1 --set sn_board=1 --set saver=1 --shot 90000:sim/build/saver_warnings.ppm`
Expected: the clock with a red line of warnings under the date.

- [ ] **Step 3: `tools/check.sh`, commit**

```bash
git add components/deck_core/deck_saver.c
git commit -m "The screensaver says how many warnings are waiting, and the latest"
```

---

### Task 6: The frame: live colour, BACK names, hold, say yields

**Files:**
- Modify: `components/deck_ui/include/deck_theme.h` (`live` in `deck_palette_t`), `components/deck_ui/deck_theme.c:57` (`g_pal.live = lv_color_hex(0x4FC3F7)`), `apps/app_sentinel/sn_ui.h` (the `hold` hook, `SN_KEEP_TOOL` meaning), `apps/app_sentinel/sn_frame.c` (`show`, `tick`, `strip_open`, `strip_update`, `strip_set`, `sn_say`), `apps/app_sentinel/app_sentinel.c:200-202` (home tile line 20 px `g_pal.label`)

**Interfaces:**
- Produces in `sn_ui.h`:

```c
/* The frame is about to leave this leaf by a route that is not its own BACK
 * (the alert line opening LOG). Return true to keep the screen up; call
 * go() once the user has agreed, or never. */
bool (*hold)(sn_fn_t go);
```
  and `SN_KEEP_TOOL` now means: when the board goes away under this leaf the frame does not replace it with the blocked page; the leaf shows the problem itself (and is built even when the board is not linked at open).

- [ ] **Step 1: The colour**

`g_pal.live` added and set. In `sn_frame.c`, the quiet strip tag (`strip_set` with `!active && level == SN_NOTE`) uses `g_pal.live`. (Each area's uses of `g_pal.accent` for data move in Tasks 7 to 9.)

- [ ] **Step 2: BACK names its parent**

In `show()`, the label is `"LEAVE <home name>"` at depth 1 (Task 2), else `"BACK TO <stack[depth-2]->name>"` (a 32-byte buffer).

- [ ] **Step 3: The hold hook and the alert line**

Two statics: `open_log_now()` does what `strip_open()` does today. `strip_open()` becomes: `if (sc && !sc->kids && s_fr.built && sc->hold && sc->hold(open_log_now)) return; open_log_now();`. `sn_open()` is unchanged (it still calls `leaf_down`).

- [ ] **Step 4: `SN_KEEP_TOOL` in `show()` and `tick()`**

`show()`: a leaf with `SN_KEEP_TOOL` is built even when `problem` is set (`s_fr.blocked` stays false). `tick()`: the "board came or went" rebuild is skipped for `SN_KEEP_TOOL` leaves.

- [ ] **Step 5: `sn_say` yields to an ALERT**

In `strip_update()`, before honouring `s_fr.say`: compute the live ALERT (a band up, or an ALERT event within `ALERT_HOLD_MS`) first; if there is one, clear `s_fr.say[0]` and fall through. Restructure: compute `best/bl` and the band state first, then `if (s_fr.say[0] && bl < SN_ALERT && !band_up && say not expired) { strip_set(say); return; }`.

- [ ] **Step 6: The home tile line**

`app_sentinel.c:202`: `deck_label(body, g_font.mono_m, g_pal.label, "")`; line 201's head stays `mono_m`. `sn_frame.c:910`: the strip tag text in `g_font.mono_m`.

- [ ] **Step 7: `tools/check.sh`, screenshot `sentinel_home.ppm` (tag in blue), commit**

```bash
git add components/deck_ui apps/app_sentinel/sn_ui.h apps/app_sentinel/sn_frame.c apps/app_sentinel/app_sentinel.c
git commit -m "SENTINEL frame: a live colour that is never an accent, BACK names where it goes, a hold before other exits"
```

---

### Task 7: WARDRIVE survives a link drop, asks before other exits, big button; mine left out

**Files:**
- Modify: `apps/app_sentinel/sn_drive.c`, `components/deck_sentinel/sentinel_tab5.c:234-241` (`sink_device`), `sim/sentinel_sim.c` (the same guard where it feeds the drive log, and a `SIM_RF_DROP=<s>` scene: the pretend node goes silent for 60 s starting at that second)
- Test: headless run with `SIM_RF_DROP`

**Interfaces:**
- Consumes: `hold`, `SN_KEEP_TOOL`, `sentinel_trackwatch_mine(&s_ble.watch, addr)` (already in `sentinel_tab5.c` scope), `sn_button(parent, key, label, fn)`.

- [ ] **Step 1: The scene and the expectation**

Add `SIM_RF_DROP` to `sentinel_sim.c`: from that second, for 60 s, the pretend node sends nothing and answers nothing. Run (after the code steps):
`SIM_RF_DROP=40 sim/build/deck_sim --headless --no-boot --set wireless=1 --set sn_board=1 --key 500:alt+x --key 1500:5 --key 3000:enter --shot 50000:sim/build/drive_nolink.ppm --key 52000:l --key 53000:esc --shot 54000:sim/build/drive_held.ppm --shot 110000:sim/build/drive_back.ppm`
Expected: `drive_nolink.ppm` shows WARDRIVE itself with `! NO LINK TO THE BOARD`; `drive_held.ppm` is still WARDRIVE, drive on (the L opened the END THE DRIVE? dialog, Esc cancelled it); `drive_back.ppm` shows RECORDING again with the row count higher than at 50 s.

- [ ] **Step 2: The screen**

`sn_drive_screen.flags = SN_BOARD | SN_KEEP_TOOL`; add `.hold = hold`. `blocked()` gains `if (!sentinel_link(st)) return "No link to the radio board.";` before the SD check (so START is disabled without a link, and the running drive's own NO LINK branch still renders). `hold(go)`: if a drive is on, `s_drv.go = go; ask_end(true)` (and `end_yes` calls `s_drv.go ? s_drv.go() : sn_back()`); else return false. `destroy()` keeps ending the drive (a real leave, by BACK or by the shell, still ends it: that is the spec's accepted behaviour).

- [ ] **Step 3: The big button**

The `toggle` bar action goes; in its place `s_drv.big = sn_button(body, "ENTER", "START A DRIVE", toggle)` after the counts, `lv_obj_set_size(s_drv.big, 320, 100)`. Enter is handled in a new `key()`: `if (k->code == HID_ENTER) { toggle(); return true; }`. `render()` sets the button's label and colour through two new frame functions in `sn_ui.h`: `void sn_button_label(lv_obj_t *b, const char *text);` and `void sn_button_danger(lv_obj_t *b, bool on);` (the bar's `sn_action_label` and `sn_action_danger` call the same code).

- [ ] **Step 4: Mine devices stay out of a drive**

`sentinel_tab5.c` `sink_device`: `if (!sentinel_trackwatch_mine(&s_ble.watch, d->addr)) sentinel_drivelog_device(...)`. The same line in `sentinel_sim.c`.

- [ ] **Step 5: Run step 1, `tools/check.sh`, commit**

```bash
git add apps/app_sentinel/sn_drive.c components/deck_sentinel/sentinel_tab5.c sim/sentinel_sim.c
git commit -m "WARDRIVE: a drive outlives a link drop, asks before any exit, one big button; mine devices are not written"
```

---

### Task 8: Wi-Fi: capture names its file, NETWORKS filter and mine

**Files:**
- Modify: `components/deck_sentinel/include/sentinel_store.h` (`char cap_file[SENTINEL_LINK_N][48]` in the stats), `components/deck_sentinel/sentinel_store.c` (`capture_open` fills it), `sim/store_test.c`, `apps/app_sentinel/sn_wifi.c`
- Test: `store_test` case `capture_names_its_file`; headless screenshots `sim/build/capture_on.ppm`, `sim/build/networks_mine.ppm`

**Interfaces:**
- Produces: `sentinel_store_stats_t.cap_file[link]` (the current or last capture file's name, "" before any), and `wifi_mine` (a settings blob of up to 8 six-byte BSSIDs, kept in `sn_wifi.c`).

- [ ] **Step 1: Store test**

In `sim/store_test.c`: after `sentinel_store_packet()` of one Wi-Fi packet, `sentinel_store_stats()` has `cap_file[SENTINEL_LINK_WIFI]` matching `^cap-wifi-[0-9]{8}-[0-9]{6}\.pcap$` (check prefix and length). Run, fail, implement, pass.

- [ ] **Step 2: The capture screen**

`cap_render()`: while recording, `v[1]` reads `"FILE: sentinel/%s"` in `g_pal.text`; stopped with a file name, `"LAST FILE: sentinel/%s"`. `cap_toggle()` stopping: `sn_say(SN_NOTE, "RECORDING STOPPED: sentinel/<file>", 15000)`. `end()`: when `s_wf.cap_wanted` was true, the same `sn_say`. `RECORDING` in `g_pal.live`. The capture screens get `.hold`: stop the capture, say the file, return false. Also `s_wf.cap_wanted = false` in `end()`.

- [ ] **Step 3: NETWORKS filter and mine**

In `sn_wifi.c`: `s_wf.flagged_only` toggled by `sn_action(HID_LETTER('F'), "F", "SHOW: ALL", ...)` whose label flips to `"SHOW: FLAGGED"`; `net_list_render()` lists only `ap_level(a) != SN_NOTE` when set, and the summary says `"%d FLAGGED OF %d"`. `M` on the list (`sn_action(HID_LETTER('M'), "M", "MINE", net_mine)`) toggles the selected BSSID in `wifi_mine` (blob, 8 entries, oldest dropped); `ap_level()` returns `SN_NOTE` for a mine network, and its row text starts with `"MINE"` in place of the mark. The detail page's first line for a mine network: `"Marked as yours: never flagged."` in `g_pal.ok`. The help gains one sentence on F and M. Rows and heat that used `g_pal.accent` use `g_pal.live`.

- [ ] **Step 4: Screenshots, `tools/check.sh`, commit**

`networks_mine.ppm`: NETWORKS with one row marked MINE (`--key` sequence: alt+x, 1, 1, down, m). `capture_on.ppm`: WI-FI > CAPTURE recording with the file name.

```bash
git add components/deck_sentinel sim/store_test.c apps/app_sentinel/sn_wifi.c
git commit -m "WI-FI: a capture names its file; NETWORKS filters to the flagged and marks yours as mine"
```

---

### Task 9: Spectrum: TRANSMITTERS guards, survey wording, Bluetooth colour

**Files:**
- Modify: `apps/app_sentinel/sn_spectrum.c:387-445` (tune, build), `:480-545` (survey), `apps/app_sentinel/sn_ble.c` (any `g_pal.accent` data use)
- Test: headless screenshots `sim/build/tx_retune_ask.ppm`, `sim/build/survey_ready.ppm`

- [ ] **Step 1: Retune asks**

`tx_tune(d)`: when `s_sp.b.bursts.n > 0` (or any `em[i].used` with `count >= 2`), store the wanted step and `sn_confirm("RETUNE?", "<n> bursts and <m> transmitters heard here are lost.", "RETUNE", tx_tune_yes)`; else retune at once. Enter's label: `"RETUNE TO 433.92 (CLEARS THE LIST)"`. `tx_build()`: when `s_sv.on`, do not cancel; instead `s_sv.pending_tx = true` and `sn_confirm("STOP THE SURVEY?", "Listening to one frequency stops the sweep the survey needs. What it measured so far is thrown away.", "STOP SURVEY", ...)`; CANCEL goes back to the SPECTRUM menu. The frame gains `void sn_confirm_or(const char *title, const char *message, const char *yes_label, sn_fn_t yes, sn_fn_t no);` (`sn_confirm` calls it with `no == NULL`), and TRANSMITTERS passes `sn_back` as `no`. TRANSMITTERS gets `.hold`: with bursts listed, ask the same RETUNE-style question worded `"LEAVE TRANSMITTERS?"`.

- [ ] **Step 2: Survey wording and colour**

`sv_build()`: the C action reads `"STOP SURVEY"`; the confirm's title `"STOP THE SURVEY?"`, yes label `"STOP IT"`. `MEASURING` and the bar's indicator use `g_pal.live`. Waterfall heat colours in `sn_spectrum.c:99-100` and `sn_wifi.c:319-320` use `g_pal.live` instead of the accent.

- [ ] **Step 3: Bluetooth and the rest**

`sn_ble.c`, `sn_offense.c:322` (SENDING), `sn_board.c:238` (ASKING), `sn_net.c:205,315,451` (a tool working): `g_pal.live`. `grep -n "g_pal.accent" apps/app_sentinel/sn_*.c` afterwards shows only control chrome (buttons, tiles, rows, keycaps).

- [ ] **Step 4: Screenshots, `tools/check.sh`, commit**

```bash
git add apps/app_sentinel
git commit -m "SPECTRUM: retuning and leaving TRANSMITTERS ask first; the survey's stop is named; live colour everywhere"
```

---

### Task 10: SD and link-loss review

**Files:**
- Read: `components/deck_sentinel/sentinel_tab5.c`, `components/deck_sentinel/sentinel_store.c`, `external/rf-sentinel/sentinel_core/src/sentinel_host.c`, `apps/app_sentinel/sn_offense.c`, `sn_board.c`
- Modify: whatever the review finds; `sim/store_test.c` for store fixes
- Test: a store test per fix

- [ ] **Step 1: Dispatch a reviewer**

A `feature-dev:code-reviewer` subagent with the spec's part 3 list verbatim (card pulled during capture, drive, survey; card full; capture file that cannot be opened while frames keep coming; node restarting mid-drive, mid-capture, in OFFENSIVE; baud negotiation failing; board switched off under an open tool), asked for: file:line, the loss or the lie on screen, a one-line fix. Keep its report in the task's commit message body.

- [ ] **Step 2: Fix what is a loss or a lie**

For each finding that loses data or shows something false: a failing store test when it is in `sentinel_store.c`, then the fix. A finding that is only a counter is recorded in the spec's "As built".

- [ ] **Step 3: `tools/check.sh`, commit**

```bash
git commit -am "SD and link-loss paths: <one line per fix>"
```

---

### Task 11: Alt+H, documents, screenshots, the CI command

**Files:**
- Modify: `components/deck_core/deck_shell.c:192-240`, `README.md`, `ROADMAP.md`, `docs/screenshots/*.png`, the spec's "As built"
- Test: `tools/check.sh --firmware`

- [ ] **Step 1: Alt+H**

Replace the `NET TOOLS` column with one headed `SENTINEL` or `NET TOOLS` by `deck_wireless_sentinel()`: `"  1..8 open a tile  ARROWS choose  ENTER opens\n  ESC or DEL back one level  L open the log\n  every other key is on its button"` for SENTINEL; `"  1..7 open a tile  ARROWS choose  ENTER opens\n  ESC or DEL back one level"` for NET TOOLS. The `ANYWHERE` line lists `ALT+T N S C F  W M R X` unchanged (R and X are each present in one mode).

- [ ] **Step 2: Documents**

README: the home screen paragraph says a WIRELESS TOOLS setting picks NET TOOLS or SENTINEL, NET TOOLS is the default. ROADMAP: a `## v1.5.0-beta.2: ship` block above `## Next: Startup sound` listing the three parts as done items and the device checkpoint unticked. The spec's "As built" section: what was built, what the review found, measurements.

- [ ] **Step 3: Screenshots**

Headless shots converted with `python3 -c "from PIL import Image; ..."` (or `sips`) to PNG in `docs/screenshots/`: `net_home.png`, `settings_wireless.png`, `sentinel_alert_banner.png`, `saver_warnings.png`, `sentinel_wardrive.png` (replaced), `sentinel_networks.png` (replaced, with MINE), `sentinel_capture.png`.

- [ ] **Step 4: The CI command**

Run: `tools/check.sh --firmware`
Expected: `PASS` with the firmware build ok and no new warnings in `sim/build/check.log`.

- [ ] **Step 5: Commit**

```bash
git add components/deck_core/deck_shell.c README.md ROADMAP.md docs
git commit -m "Ship phase documents, Alt+H for SENTINEL and NET TOOLS, screenshots"
```

---

### Task 12: The release candidate

**Files:**
- Create: `docs/releases/v1.5.0-beta.2.md`
- Modify: `tools/release.sh` (`--local`)
- Test: the images exist and verify

- [ ] **Step 1: `--local`**

`release.sh`: accept `--local` as the 4th or 5th argument; with it, skip `git push`, `git tag`, the second push and `gh release create`, and instead copy the three files to `dist/` (git-ignored) and print their paths. The `--prerelease` check on the version string still runs.

- [ ] **Step 2: Notes**

`docs/releases/v1.5.0-beta.2.md` in the style of v1.5.0-beta.1's (headline, bullets, Install): the v1.5 home and toggles as before; the new WIRELESS TOOLS row and what SENTINEL is (one paragraph, with the radio board named); NET TOOLS the default, the channel chart needing the board; the ALERT banner and the saver's line for everyone. Commit the notes.

- [ ] **Step 3: Build and sign**

Run: `tools/release.sh 1.5.0-beta.2 docs/releases/v1.5.0-beta.2.md "v1.5.0-beta.2: wireless tools, ship" --prerelease --local`
Expected: the version commit `v1.5.0-beta.2`, `dist/tab5-cyberdeck-v1.5.0-beta.2-full.bin`, `-ota.bin`, `.elf`; `espsecure.py verify_signature` passed inside the script; `strings` finds the version.

- [ ] **Step 4: Commit the script change (the version commit is the script's own)**

```bash
git add tools/release.sh .gitignore
git commit -m "release.sh --local: build and sign without tagging, pushing or publishing"
```
