# Sentinel Suite Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** RF WATCH opens into a SAFE or RED TEAM session with a tile chooser, the board link runs protocol v2 through a core shared by every host, events and captures land on SD, and warnings reach the user anywhere in DECK//OS.

**Architecture:** A portable C99 library, `sentinel_core`, lives in the `rf-sentinel` repo and holds the protocol, state, host link state machine, arm lease, CSV and pcap formatting. The C5 firmware, the CYD host and DECK//OS all build it; DECK//OS pulls `rf-sentinel` in as a git submodule at `external/rf-sentinel`. Device code keeps only pins, ports, files and screens.

**Tech Stack:** C99 and C++ (C5 firmware), ESP-IDF 5.5, LVGL 9, PlatformIO (CYD host), CMake and ctest, the desktop simulator in `sim/`.

**Spec:** `docs/superpowers/specs/2026-11-11-sentinel-suite-phase1-design.md`

## Global Constraints

- No em dashes anywhere. Grep for the character before each commit.
- No attribution or co-author lines in commit messages. Commit on branches; push nothing to GitHub.
- `tab5-cyberdeck` work is on `cl/sentinel-useful-93ca39`. `rf-sentinel` work is on `feature/sentinel-core`, cut from `hardware/tab5-board` (b6e10c3), done inside the submodule checkout `external/rf-sentinel`.
- Jamming is never built. Phase 1 adds no transmit feature at all; `CMD,TX` only answers `ERR`.
- Protocol v1 sentences and field orders do not change; the CYD host must keep working at 115200.
- Constants: `SENTINEL_PROTO_VERSION "2"`, `SENTINEL_LINE_MAX 512`, `SENTINEL_PKT_MAX 256`, `SENTINEL_ARM_LEASE_MS 5000`, `SENTINEL_ARM_REFRESH_MS 1500`, `SENTINEL_BAUD_DEFAULT 115200`, `SENTINEL_BAUD_FAST 921600`, node confirm window 3000 ms, host confirm window 2000 ms.
- Sim: `cmake -S sim -B sim/build`. All host checks: `tools/check.sh`. Firmware: `tools/check.sh --firmware`. C5: `. ~/esp/esp-idf-v5.5/export.sh && idf.py -C external/rf-sentinel build`.
- Ports: Tab5 on `/dev/cu.usbmodem201201`, C5 on `/dev/cu.usbmodem21101`; confirm each with `esptool chip_id` before flashing.

## Review Focus

1. The node reboots while the host is at 921600 (a flash, a brownout): the host returns to 115200 without help and the link recovers. (Task 3, `host_link_loss_returns_to_base`.)
2. Leaving RF WATCH in RED TEAM by Alt+Esc or an Alt+letter jump instead of Esc: the lease is still dropped. (Task 8, headless key script plus the `[sim] sentinel arm 0` line.)
3. No SD card, or the card pulled mid-session: logging drops and counts, nothing crashes or blocks the UART reader. (Task 6, `store_without_root`.)
4. A `PKT` with broken base64, or more than 256 bytes: counted as a bad frame, the next frame still parses. (Task 2, `pkt_rejects_bad_payload`.)
5. The millisecond clock wrapping during a lease (uptime past 49.7 days): the lease neither sticks nor ends early. (Task 2, `arm_survives_clock_wrap`.)

---

### Task 1: Extract the core, behaviour unchanged

**Files:**
- `tab5-cyberdeck`: add submodule `external/rf-sentinel`; modify `CMakeLists.txt`, `sim/CMakeLists.txt`, `components/deck_sentinel/CMakeLists.txt`, `components/deck_sentinel/include/deck_sentinel.h`, `.github/workflows/check.yml` (checkout with `submodules: recursive`); delete `components/deck_sentinel/sentinel_proto.c`, `include/sentinel_proto.h`, `sim/sentinel_test.c`.
- `rf-sentinel`: create `sentinel_core/CMakeLists.txt`, `library.json`, `include/sentinel_core.h`, `include/sentinel_proto.h`, `src/sentinel_proto.c`, `test/CMakeLists.txt`, `test/proto_test.c`.

**Interfaces produced:**
- `sentinel_core.h`: the constants, `sentinel_event_t`, `sentinel_ap_t`, `sentinel_state_t` exactly as in today's `deck_sentinel.h`.
- `sentinel_proto.h`: today's functions, with `bool sentinel_link_up(const sentinel_state_t *s, uint32_t now_ms)` replacing `sentinel_link()` so the core needs no platform clock. `deck_sentinel.h` keeps `static inline bool sentinel_link(const sentinel_state_t *s)`.
- `sentinel_core/CMakeLists.txt`: `idf_component_register(SRC_DIRS src INCLUDE_DIRS include)` under `ESP_PLATFORM`; otherwise `add_library(sentinel_core STATIC ...)`, and tests when `SENTINEL_CORE_TESTS` is ON (default ON when top level).

- [ ] Step 1: `git -c protocol.file.allow=always submodule add /Volumes/development/2-software_dev/rf-sentinel external/rf-sentinel`, set the URL in `.gitmodules` to `https://github.com/hey-its-brian/rf-sentinel.git`, create `feature/sentinel-core` in the submodule.
- [ ] Step 2: Move parser, state header and test into `sentinel_core/`; the test keeps every existing assertion, with `sentinel_link_up(&s, s_now)`.
- [ ] Step 3: Standalone check: `cmake -S external/rf-sentinel/sentinel_core -B /tmp-build && ctest` passes.
- [ ] Step 4: Point the sim (`add_subdirectory(${ROOT}/external/rf-sentinel/sentinel_core sentinel_core)`, link `sentinel_core`) and the firmware (`EXTRA_COMPONENT_DIRS`, `deck_sentinel REQUIRES sentinel_core`) at it.
- [ ] Step 5: `tools/check.sh --firmware` prints PASS with the same test count as before.
- [ ] Step 6: Commit in the submodule, then in `tab5-cyberdeck`.

### Task 2: Core protocol v2, lease, base64, commands, CSV, pcap

**Files (rf-sentinel):** modify `include/sentinel_core.h`, `include/sentinel_proto.h`, `src/sentinel_proto.c`; create `include/sentinel_arm.h`, `sentinel_b64.h`, `sentinel_cmd.h`, `sentinel_log.h`, `sentinel_pcap.h` and their `src/*.c`; tests `test/v2_test.c`, `test/log_test.c`.

**Interfaces produced:**

```c
/* sentinel_arm.h */
typedef struct { bool armed; uint32_t until_ms; } sentinel_arm_t;
void     sentinel_arm_set(sentinel_arm_t *a, bool on, uint32_t now_ms, uint32_t lease_ms);
bool     sentinel_arm_active(const sentinel_arm_t *a, uint32_t now_ms);
uint32_t sentinel_arm_left_ms(const sentinel_arm_t *a, uint32_t now_ms);
bool     sentinel_arm_expire(sentinel_arm_t *a, uint32_t now_ms); /* true once when a lease has run out */

/* sentinel_core.h additions */
typedef enum { SENTINEL_LINK_WIFI, SENTINEL_LINK_BLE, SENTINEL_LINK_154, SENTINEL_LINK_N } sentinel_link_t;
typedef struct { sentinel_link_t link; uint8_t ch; int8_t rssi; uint32_t us; uint16_t orig_len, len; uint8_t data[SENTINEL_PKT_MAX]; } sentinel_pkt_t;
typedef struct {
    void (*event)(void *user, const sentinel_event_t *e);
    void (*packet)(void *user, const sentinel_pkt_t *p);
    void *user;
} sentinel_sink_t;
/* sentinel_state_t gains: char mode[8]; sentinel_arm_t arm; uint32_t node_baud, host_baud, evt_seq, pkts; */

/* sentinel_proto.h changes */
bool  sentinel_proto_line(sentinel_state_t *s, char *line, uint32_t now_ms, const sentinel_sink_t *sink);
int   sentinel_proto_feed(sentinel_lines_t *l, sentinel_state_t *s, const char *data, size_t n, uint32_t now_ms, const sentinel_sink_t *sink);
char *sentinel_proto_check(char *line, bool checksum_optional); /* payload after "$RFS," or NULL */
int   sentinel_proto_pkt(char *out, size_t n, const sentinel_pkt_t *p); /* "PKT,..." payload */
void  sentinel_push_event(sentinel_state_t *s, bool warn, const char *src, const char *text, uint32_t now_ms, const sentinel_sink_t *sink);
const char *sentinel_link_name(sentinel_link_t link);

/* sentinel_b64.h */
size_t sentinel_b64_encode(char *out, size_t n, const uint8_t *in, size_t len); /* 0 if it does not fit */
int    sentinel_b64_decode(uint8_t *out, size_t n, const char *in);            /* bytes, or -1 */

/* sentinel_cmd.h (node side) */
typedef enum { SENTINEL_CMD_NONE, SENTINEL_CMD_PING, SENTINEL_CMD_HELLO, SENTINEL_CMD_QUIET, SENTINEL_CMD_STREAM,
               SENTINEL_CMD_RATE, SENTINEL_CMD_BAUD, SENTINEL_CMD_MODE, SENTINEL_CMD_ARM, SENTINEL_CMD_TX,
               SENTINEL_CMD_UNKNOWN } sentinel_cmd_id_t;
typedef struct { sentinel_cmd_id_t id; long value; char arg[24]; bool valid; } sentinel_cmd_t;
sentinel_cmd_t sentinel_cmd_parse(const char *payload);
bool           sentinel_baud_ok(long rate);

/* sentinel_log.h */
#define SENTINEL_CSV_HEADER "time,level,source,text,lat,lon,fix\n"
int sentinel_log_csv(char *out, size_t n, const struct tm *t, const sentinel_event_t *e, uint8_t fix, double lat, double lon);
int sentinel_log_events_name(char *out, size_t n, const struct tm *t);                       /* events-20261111.csv */
int sentinel_log_capture_name(char *out, size_t n, sentinel_link_t link, const struct tm *t); /* cap-wifi-20261111-163000.pcap */

/* sentinel_pcap.h */
uint32_t sentinel_pcap_linktype(sentinel_link_t link); /* wifi 105, ble 251, 154 195 */
size_t   sentinel_pcap_header(uint8_t out[24], uint32_t linktype);
size_t   sentinel_pcap_record(uint8_t out[16], uint32_t sec, uint32_t usec, uint32_t caplen, uint32_t origlen);
typedef struct { bool set; uint32_t wall_s, node_us, last_us; } sentinel_pcap_clock_t;
void     sentinel_pcap_time(sentinel_pcap_clock_t *c, uint32_t wall_s_now, uint32_t node_us, uint32_t *sec, uint32_t *usec);
```

Parser behaviour: `BAUD,<rate>` sets `node_baud`; `MODE,<tool>` sets `mode`; `ARM,<a>,<ms>` calls `sentinel_arm_set(&s->arm, a, now, ms)`; `ERR,<text>` pushes an info event from `node`; `PKT` decodes and calls `sink->packet`; every pushed event bumps `evt_seq` and calls `sink->event`. CSV quotes a field holding a comma, quote or newline and doubles quotes; lat and lon are empty without a fix.

- [ ] Step 1: Write failing tests, including: `arm_expires_once` (set at 1000 for 5000: active at 5999, not at 6000, `expire` true once), `arm_survives_clock_wrap` (set at 0xFFFFF000), `b64_round_trip` for lengths 0..5 and 256, `b64_rejects_garbage`, `cmd_parse_table` (`CMD,RATE,50` invalid, `CMD,BAUD,921600` valid, `CMD,BAUD,9600` invalid, `CMD,ARM,1`, `CMD,MODE,watch`, `CMD,TX,PROBE` arg `PROBE`, `CMD,NOPE` unknown, `STAT,1` none), `v2_sentences_update_state`, `err_becomes_event`, `sink_sees_every_event`, `pkt_round_trip` (frame built by `sentinel_proto_pkt` parses to the same bytes), `pkt_rejects_bad_payload`, `csv_quotes_and_position`, `pcap_bytes` (header `d4 c3 b2 a1 02 00 04 00`, snaplen 256, linktype 105), `pcap_clock_anchors_and_reanchors`.
- [ ] Step 2: Run, see them fail to compile or fail.
- [ ] Step 3: Implement.
- [ ] Step 4: `ctest` passes; `tools/check.sh` passes (deck callers updated for the extra `sink` argument with `NULL`).
- [ ] Step 5: Commit both repos.

### Task 3: Core host link state machine

**Files (rf-sentinel):** create `include/sentinel_host.h`, `src/sentinel_host.c`, `test/host_test.c`.

**Interfaces produced:**

```c
typedef struct {
    uint32_t baud;       /* what the port should be at */
    uint32_t want_baud;  /* SENTINEL_BAUD_DEFAULT = never negotiate */
    bool     arm_wanted;
    /* private */
    uint8_t  phase, fails; bool was_link, arm_dirty;
    uint32_t t_phase, t_hello, t_arm, frames_mark;
} sentinel_host_t;

#define SENTINEL_HOST_SEND_MAX 3
typedef struct {
    uint32_t set_baud;                     /* 0 = leave; apply BEFORE the sends below */
    char     send[SENTINEL_HOST_SEND_MAX][32];
    int      n_send;
    char     note[64]; bool note_warn;     /* a deck-side event to record, "" = none */
} sentinel_host_out_t;

void sentinel_host_init(sentinel_host_t *h, uint32_t want_baud);
void sentinel_host_arm(sentinel_host_t *h, bool on);
/* Call every 100 ms or so with the state the parser fills. May clear s->hello on link loss and sets s->host_baud. */
void sentinel_host_tick(sentinel_host_t *h, sentinel_state_t *s, uint32_t now_ms, sentinel_host_out_t *out);
```

Rules: ask `CMD,HELLO` every 5000 ms until `hello`. With link, `hello`, `proto >= 2` and `want_baud > baud`: send `CMD,BAUD,<want>`; switch on `node_baud == want` or after 300 ms; then `CMD,HELLO` every 500 ms until `frames` advances (fast, note "link at 921600 baud") or 2000 ms pass (send `CMD,BAUD,115200`, then drop to 115200 on the next tick, warn note, `fails++`). Link loss at any time: `set_baud` 115200 if not there, clear `hello`, warn note "link lost". After 2 fails, stop negotiating. While `arm_wanted` and link: `CMD,ARM,1` at once and every 1500 ms; when it turns off: `CMD,ARM,0` once.

- [ ] Step 1: Failing tests driven by a small fake node inside the test: `host_asks_hello_until_answered`, `host_negotiates_up` (ends with `baud == 921600`, `host_baud` in state), `host_skips_v1_node`, `host_falls_back_when_fast_is_deaf`, `host_link_loss_returns_to_base`, `host_gives_up_after_two_fails`, `host_arm_refreshes_and_drops` (three `CMD,ARM,1` in 3100 ms, one `CMD,ARM,0`).
- [ ] Step 2 to 4: fail, implement, pass. Step 5: commit.

### Task 4: C5 firmware on the core

**Files (rf-sentinel):** modify `CMakeLists.txt` (`EXTRA_COMPONENT_DIRS`, `PROJECT_VER "0.3.0"`), `main/CMakeLists.txt` (REQUIRES `sentinel_core`), `main/proto.h`, `main/proto.cpp`, `main/main.cpp`.

**Interfaces produced:** `proto_send` builds through `sentinel_proto_frame`; `proto_check` is `sentinel_proto_check(line, true)`; new `void proto_set_baud(uint32_t rate)` (waits for TX to drain, then `uart_set_baudrate`). `RFS_PROTO_VERSION` is `SENTINEL_PROTO_VERSION`; `RFS_FW_VERSION` "0.3.0".

`handle_cmd` switches on `sentinel_cmd_parse`. BAUD: answer, `proto_set_baud`, start a 3000 ms confirm window that any later valid command ends and whose expiry reverts to 115200. ARM: `sentinel_arm_set` with the 5000 ms lease, answer `ARM,<a>,<left>`. Main loop: `sentinel_arm_expire` sends `ARM,0,0` and the `disarmed: lease expired` event. TX: `ERR,not armed` or `ERR,no such test`. MODE: `watch` answers `MODE,watch`, anything else `ERR,unknown mode`.

- [ ] Step 1: Implement. Step 2: `idf.py -C external/rf-sentinel build` succeeds for esp32c5. Step 3: commit.

### Task 5: CYD host on the core

**Files (rf-sentinel):** modify `host-cyd/platformio.ini` (`lib_deps` adds `sentinel_core=symlink://../sentinel_core`), `host-cyd/src/proto.cpp`.

`protoPoll()` feeds bytes to `sentinel_proto_feed` into a file-static `sentinel_state_t`, then copies into `Model` (fields map one to one; `Model` peaks keep their own rise-only rule, updated when `sub_sweeps` or `nrf_sweeps` changes; new ring entries are pushed with `Model::pushEvent`, found by `evt_seq`). `protoSend` uses `sentinel_proto_frame`. `ui.cpp` and `model.hpp` are untouched.

- [ ] Step 1: Implement. Step 2: `pio run -d external/rf-sentinel/host-cyd` succeeds. Step 3: commit. Not run on hardware.

### Task 6: Deck glue: link machine, writer task, SD files, fake board

**Files (tab5-cyberdeck):** modify `components/deck_sentinel/include/deck_sentinel.h`, `sentinel_tab5.c`, `CMakeLists.txt`, `sim/sentinel_sim.c`, `sim/CMakeLists.txt`; create `components/deck_sentinel/sentinel_store.c`, `include/sentinel_store.h`, `sim/store_test.c`.

**Interfaces produced:**

```c
/* deck_sentinel.h */
void sentinel_arm(bool on);                      /* hold or drop the node's arm lease */
void sentinel_note(bool warn, const char *text); /* a deck-side event, source "deck" */
static inline bool sentinel_armed(const sentinel_state_t *s);

/* sentinel_store.h: plain stdio on hal_storage_root(), callable from any one task */
typedef struct { uint32_t events, events_dropped, packets, packets_dropped; } sentinel_store_stats_t;
void sentinel_store_event(const sentinel_event_t *e, uint8_t fix, double lat, double lon);
void sentinel_store_packet(const sentinel_pkt_t *p);   /* SD only; opens the capture file on first use */
void sentinel_store_close(void);                       /* end the session's capture files */
void sentinel_store_stats(sentinel_store_stats_t *out);
int  sentinel_store_tail(char *out, size_t n);         /* tail of today's events file, starting at a line; bytes, 0 if none */
```

Device: the reader task runs `sentinel_host_tick` every pass, applies `set_baud` (`uart_set_baudrate`, `uart_flush_input`) before sending, and turns notes into `sentinel_push_event`. The sink callbacks copy into a FreeRTOS queue (depth 32, no wait; a full queue counts a drop); a writer task (priority 3, stack 6144) drains it into `sentinel_store_*`. UART RX ring grows to 8192. Sim: the fake board answers `CMD,BAUD`, `CMD,MODE`, `CMD,ARM`, `CMD,TX`, expires leases with `sentinel_arm_expire`, emits one `PKT,wifi` beacon every 5 s, prints `[sim] sentinel arm <0 or 1>` on changes, and calls the store directly from its sink.

- [ ] Step 1: Failing `store_test` (links `sentinel_store.c` with a stub `hal_storage_root`): `store_writes_header_once_and_appends`, `store_without_root` (root NULL: no crash, `events_dropped == 1`), `store_capture_is_valid_pcap` (24 + 16 + n bytes), `store_tail_starts_on_a_line`.
- [ ] Step 2 to 4: fail, implement, `tools/check.sh --firmware` passes. Step 5: commit.

### Task 7: deck_notice and the status bar

**Files:** create `components/deck_core/include/deck_notice.h`, `components/deck_core/deck_notice.c`; modify `deck_statusbar.c`, `components/deck_core/CMakeLists.txt`, `sim/CMakeLists.txt`.

**Interfaces produced:**

```c
typedef enum { DECK_NOTICE_INFO, DECK_NOTICE_WARN } deck_notice_level_t;
void        deck_notice_post(deck_notice_level_t level, const char *source, const char *text, deck_app_t *app);
unsigned    deck_notice_unseen(void);
void        deck_notice_seen(void);
void        deck_notice_set_flag(const char *text); /* NULL clears */
const char *deck_notice_flag(void);                 /* NULL when none */
deck_app_t *deck_notice_app(void);
```

Banner: a panel on `lv_layer_top()` under the status bar, text `SOURCE :: text`, 4000 ms, at most one new banner per 10000 ms, none when `deck_shell_current() == app`, `deck_shell_sleeping()` or `deck_saver_active()`; tapping it opens `app`. Status bar: an `ICON_ALERT` indicator with the count in the warn color while `unseen > 0` (tap opens the app), and the flag text in the danger color while set.

- [ ] Step 1: Implement. Step 2: headless shot with a posted warning shows banner and counter (driven from Task 8's sim run). Step 3: commit.

### Task 8: RF WATCH: gate, chooser, OVERVIEW, LOG

**Files:** modify `apps/app_rf/app_rf.c`, `apps/app_rf/CMakeLists.txt`; create `apps/app_rf/rf_ui.h`, `rf_overview.c`, `rf_log.c`.

**Interfaces produced (`rf_ui.h`):**

```c
typedef enum { RF_MODE_SAFE, RF_MODE_RED } rf_mode_t;
rf_mode_t rf_mode(void);
lv_obj_t *rf_panel(lv_obj_t *parent, const char *title);
void rf_overview_build(lv_obj_t *parent); void rf_overview_destroy(void); bool rf_overview_key(const deck_key_t *k);
void rf_log_build(lv_obj_t *parent);      void rf_log_destroy(void);      bool rf_log_key(const deck_key_t *k);
```

`app_rf.c` owns three screens inside the app's parent: gate, chooser, tool. Keys: arrows move the selection, Enter opens, Esc from a tool returns to the chooser, Esc elsewhere is left to the shell. `on_stop` and `on_disable` call `sentinel_arm(false)` and clear the flag. A background LVGL timer (500 ms, created in `on_enable`, deleted in `on_disable`) posts new `warn` events by `evt_seq`, marks them seen while RF WATCH is on screen, sets the `RED TEAM` flag from `sentinel_armed`, and on `deck_shell_sleeping() || deck_saver_active()` in RED TEAM drops to SAFE with the note "disarmed: screen slept". Offline tiles carry `PHASE 2`, `PHASE 3`, `PHASE 5`, `PHASE 5` for BLUETOOTH, WI-FI, SUB-GHZ, 2.4 GHZ.

- [ ] Step 1: Implement. Step 2: headless shots: gate; chooser SAFE; chooser RED TEAM with the status bar flag; OVERVIEW; LOG; a banner over TERMINAL. Step 3: key script leaves RED TEAM by Alt+Esc and the sim prints `[sim] sentinel arm 0`. Step 4: `tools/check.sh --firmware` passes. Step 5: commit.

### Task 9: Documents

**Files:** `rf-sentinel`: `docs/protocol.md` (v2 table, lease, fallback), `README.md`, `ROADMAP.md` (the six phases, the rule that replaces receive-only). `tab5-cyberdeck`: `README.md`, `ROADMAP.md`.

- [ ] Step 1: Write. Step 2: grep both repos' changed files for em dashes. Step 3: commit both.

### Task 10: Hardware

- [ ] Step 1: `esptool chip_id` on both ports; flash the C5 (`idf.py -C external/rf-sentinel -p <c5> flash`) and the Tab5 (`idf.py -p <tab5> flash`).
- [ ] Step 2: Read both serial logs: HELLO with proto 2, the baud note, STAT at the fast rate.
- [ ] Step 3: Exercise SAFE and RED TEAM (through whatever input the bench allows) and record the `ERR` answers, the disarm on leaving, and the events file path.
- [ ] Step 4: Push `feature/sentinel-core` into the local checkout `/Volumes/development/2-software_dev/rf-sentinel` (a local branch, not GitHub). Write the hand-over note in the shared notes folder.
