# Sentinel suite, phase 1: shared core, mode gate, chooser, link v2, logging, alerts

Status: design approved in conversation, 2026-11-11. Spec written while Brian
was away, on his instruction to proceed; review it after the fact.

## Why

RF WATCH was a live readout of the RF Sentinel board. Brian redefined it on
2026-11-11 as a safeguarding suite: it watches and alerts, and it actively
tests equipment the user owns or has permission to test. The old rule
"receive-only, never transmits" is replaced by:

- Jamming is never built.
- Anything that transmits is aimed at one chosen target, never broadcast.
- Transmit tools exist only in RED TEAM mode, chosen when the app is opened.
- Wardriving and every detector are record-only.

The suite will run on more devices than the Tab5, so everything that is not
a screen or a pin moves into a portable core in the `rf-sentinel` repo.

## The whole suite, in phases

1. **Foundation (this spec).**
2. Bluetooth: scan, AirTag and tracker alert, Flock and Axon detect, capture.
3. Wi-Fi watching: packet monitor waterfall, channel analyzer, drone detect,
   attack detectors, honeypot.
4. Wardriving: all radios with GPS, record-only, WiGLE format.
5. Sub-GHz and 2.4 GHz: spectrum analyzer, IoT site survey, TPMS (433.92 MHz
   only on this CC1101 module), jam detection.
6. Active tests: targeted deauth, per-radio self tests.

Decisions already made for phase 6, recorded so they are not reopened: the
deauth test is scan, pick one network, pick one client, read a permission
warning on every run (`deck_modal_confirm_danger`), then a counted burst that
is logged. No ownership proof (no stored list, no password, no SSID rule):
professional testers run black-box tests with permission and without the
key. Never the broadcast address, no continuous mode.

## Scope of phase 1

In: the shared core and its three consumers; protocol v2 (baud, mode, arm
lease, transmit gate, packet sentence); the mode gate and tile chooser in
RF WATCH; the event log and capture writer on SD; a notice service with a
status bar counter and a banner; simulator and host tests; a check on the
Tab5 with the board.

Out: every tool behind the WI-FI, SUB-GHZ, 2.4 GHZ and BLUETOOTH tiles (they
show as offline), any transmit feature, sounds, thresholds in SETTINGS.

## 1. Repositories

`rf-sentinel` (branch `feature/sentinel-core`, cut from `hardware/tab5-board`):

```
sentinel_core/        portable C99, no hardware, no LVGL
  include/  src/  test/  CMakeLists.txt  library.json
main/                 C5 firmware, frames and commands through the core
host-cyd/             CYD host, parses through the core
```

`sentinel_core/CMakeLists.txt` is an ESP-IDF component when `ESP_PLATFORM`
is set and a plain CMake library with ctest tests otherwise. `library.json`
makes it a PlatformIO library for `host-cyd`.

`tab5-cyberdeck` (branch `cl/sentinel-useful-93ca39`):

```
external/rf-sentinel          git submodule (the whole rf-sentinel repo)
components/deck_sentinel      Tab5 glue: EXT5V, pins, UART, writer task, SD files
apps/app_rf                   DECK//OS screens only
components/deck_core          gains deck_notice (status bar counter, banner, flag)
```

The top `CMakeLists.txt` adds `external/rf-sentinel/sentinel_core` to
`EXTRA_COMPONENT_DIRS`; the simulator adds it with `add_subdirectory`, which
also brings the core's tests into `tools/check.sh`. The submodule lives
outside `components/` because the rf-sentinel root is itself an ESP-IDF
project and would be picked up as a broken component.

The core holds: the `$RFS` frame builder and parser, the state record, the
command parser the node uses, the arm lease, base64, the CSV line formatter,
the pcap writer. It never opens a file or a port; callers pass bytes in and
get bytes out.

## 2. Protocol v2

`HELLO` reports protocol `2`. Everything in v1 is unchanged, so the CYD host
(which never negotiates) keeps working at 115200.

| Direction | Sentence | Meaning |
|---|---|---|
| host to node | `CMD,BAUD,<rate>` | 115200, 230400, 460800 or 921600. The node answers `BAUD,<rate>` at the old rate, drains, then switches. |
| host to node | `CMD,MODE,<tool>` | Which tool owns the radios. Phase 1 knows `watch` only. Answer: `MODE,<tool>` or `ERR,unknown mode`. |
| host to node | `CMD,ARM,1` / `CMD,ARM,0` | Take or drop the arm lease. Answer: `ARM,<0 or 1>,<ms left>`. |
| host to node | `CMD,TX,<test>,...` | Any transmit test. Unarmed: `ERR,not armed`. Armed: `ERR,no such test` until phase 6. |
| node to host | `ARM,0,0` and `EVT,info,node,disarmed: lease expired` | Sent once when a lease runs out. |
| node to host | `PKT,<link>,<ch>,<rssi>,<us>,<orig_len>,<base64>` | One captured frame. `link` is `wifi`, `ble` or `154`. At most 256 payload bytes. |

Baud fallback is symmetric. After switching, the node needs one valid command
within 3 s or it returns to 115200; the host asks for `HELLO` at the new rate
and returns to 115200 if none arrives within 2 s. The host also drops to
115200 whenever the link is lost, because a rebooted node starts there.

The arm lease is 5 s; the host refreshes it every 1.5 s while RED TEAM is
on. The node is the authority: it checks the lease on every transmit
command. The host shows "armed" only while the node's `ARM,1` answers keep
arriving, using the same lease code from the core.

`ERR` sentences are now kept as events (source `node`) so a refused command
is visible. `SENTINEL_LINE_MAX` grows from 320 to 512 for `PKT`.

## 3. RF WATCH on DECK//OS

Opening RF WATCH shows the **mode gate**: two tiles, SAFE (focused) and RED
TEAM, with one line under each. The choice holds until the app is left;
reopening asks again. Inside, Esc from a tool returns to the chooser, and
Esc from the chooser or the gate leaves the app.

- SAFE: no arm lease is ever requested.
- RED TEAM: the lease is held, the chooser header and the status bar show
  RED TEAM in the danger color. Leaving the app, switching RF WATCH off, the
  screen sleeping or the saver starting all drop the lease at once; after a
  sleep the session continues as SAFE.

The **chooser** has six tiles: OVERVIEW (today's dashboard, plus a line for
mode, arm state and baud), LOG (the event log from SD, newest first), and
WI-FI, SUB-GHZ, 2.4 GHZ, BLUETOOTH shown offline with the phase that brings
them. OVERVIEW gains `[T]`, which sends `CMD,TX,PROBE` so the gate's answer
shows up in EVENTS.

Background watching is unchanged by any of this: while RF WATCH is switched
on in SETTINGS the board is powered and read, whatever is on screen.

Files: `app_rf.c` (module, gate, chooser, navigation, background timer, home
tile), `rf_overview.c`, `rf_log.c`, `rf_ui.h`.

## 4. Logging

A writer task in `deck_sentinel` takes events and packets from a queue fed
by the reader task through the core's sink callbacks, so SD stalls never
block the UART. Files go under `<hal_storage_root()>/sentinel/`:

- `events-YYYYMMDD.csv`: `time,level,source,text,lat,lon,fix`. Local time
  from the deck clock; position only with a fix. Opened, appended and closed
  per event. Written to SD or to the internal FAT.
- `cap-<link>-YYYYMMDD-HHMMSS.pcap`: standard pcap, one file per link per
  session, opened on the first packet. SD only.

Deck-side happenings (link up and down, baud changes, armed, disarmed) are
events too. A full queue drops and counts. Phase 1 has no hardware packet
source; the simulator's fake board emits `PKT` so the path is exercised.

## 5. Alerts

`deck_notice` in `deck_core` (LVGL task only):

```c
void deck_notice_post(deck_notice_level_t level, const char *source, const char *text, deck_app_t *app);
unsigned deck_notice_unseen(void);
void deck_notice_seen(void);
void deck_notice_set_flag(const char *text);   /* NULL clears; "RED TEAM" */
```

A warning raises the unseen count, shown in the status bar as an indicator
that opens `app` when tapped, and shows a banner under the status bar for
4 s (at most one new banner every 10 s; none while `app` is on screen, the
screen sleeps or the saver runs). RF WATCH's background timer posts the
board's `warn` events and marks them seen while it is open. No sound, no
dialogs.

## 6. Testing

- Core (ctest, also run by `tools/check.sh`): the existing parser test, plus
  v2 sentences, the command parser, lease timing including clock wrap,
  base64, CSV quoting, pcap bytes.
- Simulator: the fake board answers `BAUD`, `MODE`, `ARM`, `TX`, lets a
  lease expire, and emits `PKT`; headless screenshots of the gate, the
  chooser in both modes, OVERVIEW, LOG and a banner over another app.
- Firmware: `tools/check.sh --firmware`; the C5 build; the CYD host build.
- Hardware: flash both; link comes up at 921600; SAFE answers `not armed`,
  RED TEAM answers `no such test`; leaving the app disarms; the event log
  appears on the SD card. The CYD host is built but not run.

## Risks

- 921600 baud on the M5-Bus may not hold; the fallback keeps the link at
  115200 and the result is reported either way.
- The submodule pins an rf-sentinel commit that is not on GitHub until that
  branch is pushed. Push `rf-sentinel` first, then `tab5-cyberdeck`.
