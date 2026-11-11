# Sentinel Suite Phase 2 (Bluetooth) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The BLUETOOTH tile lists every BLE advertiser, flags trackers and surveillance devices, warns when a tracker stays with the user, and records advertisements to a pcap file, with detection running in the background.

**Architecture:** The C5 scans passively with NimBLE (observer only), classifies and aggregates with `sentinel_core`, and reports devices as `BDV` rows. Hosts keep a table and run the following rule from the core. Capture rebuilds link-layer frames and sends them as `PKT,ble`.

**Tech Stack:** C99 core with ctest, ESP-IDF 5.5 NimBLE on ESP32-C5, LVGL 9 on DECK//OS, the simulator.

**Spec:** `docs/superpowers/specs/2026-11-11-sentinel-suite-phase2-bluetooth-design.md`

## Global Constraints

- Everything in this phase is receive-only: passive scan, no scan requests, no connections. NimBLE is built with the observer role alone.
- No em dashes; no attribution lines in commits; nothing pushed to GitHub. Branches as in phase 1.
- Protocol stays version 2; every change is additive, and a host that ignores `BLE`, `BDV` and `CAP` keeps working.
- Constants: node table 48 devices, host table 96, a report when a device is new, its kind changes, its signal moves 8 dB, or 10 s pass; node drops a device silent for 30 s, host for 60 s. Following: 10 min and 300 m with a fix, 30 min without one, a 120 s gap restarts the clock. Mine list 8 addresses. Capture needs 460800 baud or more.
- `rm external/rf-sentinel/sdkconfig` before the first C5 build: `sdkconfig.defaults` only applies to symbols the old file does not mention.

## Review Focus

1. An advertisement whose length bytes lie (a field running past the 31 bytes, a zero length mid-way): classified from what is valid, nothing read past the end. (Task 1, `ad_fields_that_lie`.)
2. More devices than the table holds (a train station): the oldest is replaced, counts stay sane, no report is lost for a tracker. (Task 2, `agg_full_table_evicts_oldest`.)
3. A neighbour's tracker while the user sits still with a fix: never a warning. (Task 3, `stationary_is_not_following`.)
4. A tracker whose address rotates: a new device, a new clock, no crash, and the old entry ages out. (Task 3, `rotation_starts_over`.)
5. Leaving the BLUETOOTH tool with capture on: capture stops and the radio returns to `watch`. (Task 6, key script and the sim's `[sim] sentinel mode` lines.)

---

### Task 1: Core: advertisements, kinds, BDV, link-layer frames

**Files (rf-sentinel):** create `sentinel_core/include/sentinel_ble.h`, `src/sentinel_ble.c`, `test/ble_test.c`; modify `sentinel_core.h` (state: `ble_ok`, `capture`, `ble_devices`, `ble_rate`, `ble_trackers`, `ble_strongest`, `ble_reports`; sink: `device`), `sentinel_proto.c` (`BLE`, `BDV`, `CAP`, `ble=` in HELLO), `sentinel_cmd.[ch]` (`SENTINEL_CMD_CAP`), `sentinel_pcap.[ch]` (BLE link type 256, `sentinel_pcap_ble_phdr`).

```c
typedef enum { SENTINEL_BLE_UNKNOWN, SENTINEL_BLE_FINDMY, SENTINEL_BLE_AIRTAG, SENTINEL_BLE_FMTAG, SENTINEL_BLE_FMAPPLE,
               SENTINEL_BLE_SMARTTAG, SENTINEL_BLE_TILE, SENTINEL_BLE_CHIPOLO, SENTINEL_BLE_FMDN,
               SENTINEL_BLE_FLOCK, SENTINEL_BLE_AXON, SENTINEL_BLE_KIND_N } sentinel_ble_kind_t;
typedef struct { uint8_t addr[6]; bool pub; int8_t rssi; sentinel_ble_kind_t kind; uint16_t company, count;
                 char name[SENTINEL_BLE_NAME_MAX + 1]; } sentinel_bdv_t;   /* addr most significant byte first */
const char *sentinel_ble_kind_word(sentinel_ble_kind_t k);   /* "airtag": the word in BDV */
const char *sentinel_ble_kind_label(sentinel_ble_kind_t k);  /* "AirTag": for people */
sentinel_ble_kind_t sentinel_ble_kind_from_word(const char *w);
bool sentinel_ble_is_tracker(sentinel_ble_kind_t k);         /* airtag, fmtag, fmapple, smarttag, tile, chipolo, fmdn */
bool sentinel_ble_is_surveillance(sentinel_ble_kind_t k);    /* flock, axon */
const char *sentinel_ble_company(uint16_t id);               /* "Apple", or NULL */
void   sentinel_ble_classify(const uint8_t addr[6], bool pub, const uint8_t *adv, size_t len,
                             sentinel_ble_kind_t *kind, uint16_t *company, char *name, size_t name_n);
int    sentinel_proto_bdv(char *out, size_t n, const sentinel_bdv_t *d);
size_t sentinel_ble_ll_frame(uint8_t *out, size_t n, const uint8_t addr[6], bool pub, uint8_t pdu_type,
                             const uint8_t *adv, size_t len);     /* access address, header, AdvA, data, zero CRC */
void   sentinel_ble_addr_text(char out[18], const uint8_t addr[6]);
size_t sentinel_pcap_ble_phdr(uint8_t out[10], int8_t rssi);
```

- [ ] Tests first: one per fingerprint with a hand-built advertisement (`airtag_separated`, `findmy_with_owner`, `fm_subtypes`, `smarttag`, `tile`, `chipolo`, `fmdn_not_plain_eddystone`, `flock_by_name`, `axon_by_prefix_public_only`), `ad_fields_that_lie`, `bdv_round_trip` (a name with a comma), `ll_frame_bytes`, `ble_phdr_bytes`, `ble_and_cap_sentences`. Then implement, pass, commit.

### Task 2: Core: the node's table and report policy

**Files:** `include/sentinel_bleagg.h`, `src/sentinel_bleagg.c`, `test/bleagg_test.c`.

```c
void sentinel_bleagg_init(sentinel_bleagg_t *a);
void sentinel_bleagg_feed(sentinel_bleagg_t *a, const uint8_t addr[6], bool pub, int8_t rssi, const uint8_t *adv, size_t len, uint32_t now_ms);
bool sentinel_bleagg_next(sentinel_bleagg_t *a, uint32_t now_ms, sentinel_bdv_t *out);
typedef struct { uint16_t devices, rate, trackers; int8_t strongest; } sentinel_ble_summary_t;
void sentinel_bleagg_summary(sentinel_bleagg_t *a, uint32_t now_ms, sentinel_ble_summary_t *out);
```

- [ ] Tests: `agg_reports_new_then_waits`, `agg_reports_kind_and_signal_changes`, `agg_keeps_name_and_kind`, `agg_drops_silent`, `agg_full_table_evicts_oldest`, `agg_summary_rate`. Implement, pass, commit.

### Task 3: Core: host table, track watch, link machine wants

**Files:** `include/sentinel_blehost.h`, `src/sentinel_blehost.c`, `test/track_test.c`; modify `sentinel_host.[ch]`, `test/host_test.c`.

```c
void  sentinel_bletab_update(sentinel_bletab_t *t, const sentinel_bdv_t *d, uint32_t now_ms);
void  sentinel_bletab_expire(sentinel_bletab_t *t, uint32_t now_ms);
void  sentinel_bletab_sort(sentinel_bletab_t *t);                       /* strongest first */
typedef enum { SENTINEL_TRACK_NONE, SENTINEL_TRACK_NEW, SENTINEL_TRACK_FOLLOWING, SENTINEL_TRACK_SURVEILLANCE } sentinel_track_event_t;
sentinel_track_event_t sentinel_trackwatch_feed(sentinel_trackwatch_t *w, const sentinel_bdv_t *d, uint32_t now_ms, bool fix, double lat, double lon);
bool  sentinel_trackwatch_mine(const sentinel_trackwatch_t *w, const uint8_t addr[6]);
void  sentinel_trackwatch_set_mine(sentinel_trackwatch_t *w, const uint8_t addr[6], bool mine);
const sentinel_track_t *sentinel_trackwatch_find(const sentinel_trackwatch_t *w, const uint8_t addr[6]);
float sentinel_distance_m(double lat1, double lon1, double lat2, double lon2);
void  sentinel_blehost_device(sentinel_blehost_t *b, sentinel_state_t *s, const sentinel_bdv_t *d, uint32_t now_ms, const sentinel_sink_t *sink);
void  sentinel_host_mode(sentinel_host_t *h, const char *mode);
void  sentinel_host_capture(sentinel_host_t *h, bool on);
```

- [ ] Tests: `new_tracker_is_info_once`, `following_needs_time_and_distance`, `stationary_is_not_following`, `no_fix_takes_thirty_minutes`, `gap_restarts_the_clock`, `mine_is_silent`, `rotation_starts_over`, `surveillance_warns_once_then_after_ten_minutes`, `blehost_pushes_events` (text and level), `host_asks_mode_and_capture_until_granted`. Implement, pass, commit.

### Task 4: C5 firmware

**Files:** create `components/blescan/` (`blescan.c`, `include/blescan.h`, `CMakeLists.txt`); modify `sdkconfig.defaults`, `main/main.cpp`, `main/proto.[h,cpp]` (`proto_send_quiet`), `main/CMakeLists.txt`.

`blescan_start()` brings NimBLE up and starts a passive scan (30 ms window in 100 ms shared with Wi-Fi; continuous in `MODE,ble`). The gap callback feeds a `sentinel_bleagg_t` under a mutex and, while capturing, sends `PKT,ble`. `main.cpp` sends `BLE` every 2 s and up to four due `BDV` rows per pass, handles `MODE,ble|watch` (the Wi-Fi task skips its scan in `ble`) and `CAP`.

- [ ] Implement; `rm sdkconfig`; build; commit.

### Task 5: Deck glue and simulator

**Files:** `components/deck_sentinel/include/deck_sentinel.h`, `sentinel_tab5.c`, `sentinel_store.c` (BLE radio header), `sim/sentinel_sim.c`, `sim/store_test.c`.

```c
void sentinel_tool(const char *mode);                 /* "watch" or "ble" */
void sentinel_capture(bool on);
void sentinel_ble_table(sentinel_bletab_t *out);      /* a copy, expired devices dropped, strongest first */
bool sentinel_ble_track(const uint8_t addr[6], sentinel_track_t *out);
bool sentinel_ble_mine(const uint8_t addr[6]);
void sentinel_ble_set_mine(const uint8_t addr[6], bool mine);   /* saved as "rf_mine" */
```

- [ ] Implement; the sim's board plays a scene through `sentinel_bleagg`; `tools/check.sh`; commit.

### Task 6: The BLUETOOTH tool

**Files:** create `apps/app_rf/rf_ble.c`; modify `rf_ui.h`, `app_rf.c` (tile live), `rf_overview.c` (BLE chip), `apps/app_rf/CMakeLists.txt`.

Summary and selected-device panels on the left, the list on the right. Keys: up and down select, `F` cycles ALL / TRACKERS / SURVEILLANCE, `M` marks mine, `C` toggles capture, Esc returns to the chooser (which stops capture and returns the radio to `watch`).

- [ ] Implement; headless shots; key script for Review Focus 5; `tools/check.sh --firmware`; commit.

### Task 7: Hardware and documents

- [ ] Flash both; a temporary bench script opens the tool, reports the table and toggles capture; record what the room shows. Remove the script, flash the clean build.
- [ ] `docs/protocol.md`, both READMEs and ROADMAPs, a screenshot; commit both repos; push the rf-sentinel branch into the local checkout.
