# Sentinel suite, phase 2: Bluetooth

Status: written 2026-11-11 on Brian's instruction to move to phase 2 when
phase 1 was done. The feature list is his (BT scan, AirTag alert, Flock and
Axon detect, Bluetooth PCAP); the decisions below are assumptions made
while he was away and are his to overrule.

Everything in this phase listens. Nothing transmits: the scan is passive
(no scan requests), so the arm lease and RED TEAM play no part.

## What the user gets

Behind the BLUETOOTH tile:

- **Scan**: every BLE advertiser in earshot, strongest first, with address,
  signal, what it is (when recognised), vendor, name and how often it speaks.
- **Trackers**: AirTag and other Find My tags that are away from their
  owner, Samsung SmartTag, Tile, Chipolo, Google Find My Device tags. A
  tracker seen for the first time is an info event. A tracker that stays
  with you is a warning (rules below), which reaches you in any app.
- **Surveillance devices**: Flock (by advertised name) and Axon (by address
  prefix 00:25:DF), a warning on first sight.
- **Capture**: `C` records every advertisement to
  `sentinel/cap-ble-<time>.pcap`, which Wireshark opens.

Tracker and surveillance detection also run in the background, in SAFE or
RED TEAM, in any app, as long as RF WATCH is switched on.

## Decisions (assumptions)

1. **BLE scanning is part of `watch`.** An alert that only works while one
   screen is open is not an alert. In `watch` the C5 shares its radio
   between the Wi-Fi presence scan and a passive BLE scan (ESP-IDF software
   coexistence). Opening the BLUETOOTH tool asks for `MODE,ble`: BLE gets
   the radio full time and the Wi-Fi scan pauses; leaving returns to `watch`.
2. **The node aggregates, the host decides.** The C5 keeps a table of the
   devices it hears, classifies each advertisement with code from
   `sentinel_core`, and reports devices, not advertisements: a `BDV` row
   when a device appears, changes, or every 10 s. That fits a 115200 link.
   The host keeps its own table, and the following rule runs on the host,
   which has the clock, the position and the log.
3. **"Following" means it moved with you.** A tracker heard for 10 minutes
   during which the deck moved 300 m from where it was first heard is a
   warning. With no GPS fix, a tracker heard for 30 minutes is a warning.
   One warning per tracker per session. Only trackers that are away from
   their owner count for Find My: a tag travelling with its owner's phone
   sends a different, shorter advertisement and is listed, not alerted.
4. **Yours can be marked.** `M` on a selected device marks it as mine: no
   alerts for it. The list is kept in settings (8 addresses). Tags that
   change address need marking again; that is a limit of working passively.
5. **Fingerprints are data.** One table in `sentinel_core` (`sentinel_ble.c`).
   The Find My, SmartTag (0xFD5A), Tile (0xFEED), Chipolo (0xFE33) and
   Google (0xFEAA frames 0x40, 0x41) signatures are the published formats.
   Flock names and the Axon prefix come from community research and have
   not been checked against real units here.
6. **Capture is honest about what it has.** The BLE host stack reports an
   advertisement's sender, type, payload and signal, not the channel or the
   CRC. Captured frames are rebuilt link-layer packets with a zero CRC,
   written with the pcap radio header that marks the CRC as not checked.
   Capture runs only in `MODE,ble`, on a link at 460800 baud or faster.

## Protocol additions (still version 2, all additive)

| Direction | Sentence | Meaning |
|---|---|---|
| node to host | `HELLO` gains `ble=0/1` at the end | the BLE scanner started |
| node to host | `BLE,<devices>,<adverts per s>,<trackers>,<strongest dBm>` | every 2 s |
| node to host | `BDV,<addr>,<p or r>,<rssi>,<kind>,<company hex>,<count>,<name>` | one device; public or random address |
| host to node | `CMD,MODE,ble` / `CMD,MODE,watch` | who owns the 2.4 GHz radio |
| host to node | `CMD,CAP,1` / `CMD,CAP,0`, answered `CAP,<0 or 1>` | stream every advertisement as `PKT,ble` |

Kinds: `unknown`, `findmy` (with its owner), `airtag`, `fmtag`, `fmapple`
(separated third-party tag, separated Apple device), `smarttag`, `tile`,
`chipolo`, `fmdn`, `flock`, `axon`.

## Where the code goes

`sentinel_core` (portable, tested on the host):

- `sentinel_ble`: the advertisement parser, the fingerprint table and
  `sentinel_ble_classify()`, kind and vendor names, the `BDV` row as a
  struct and as text, the link-layer frame builder for capture.
- `sentinel_bleagg`: the node's device table and its reporting policy.
- `sentinel_trackwatch`: the following rule and the mine list.
- `sentinel_pcap`: BLE captures use link type 256 with its radio header.
- `sentinel_host`: the mode and capture the host wants, asked until granted.

`rf-sentinel` node: `components/blescan` (NimBLE, observer only), `MODE`
and `CAP` in `main.cpp`. `PKT` goes to the UART only, not the USB console.

DECK//OS: `deck_sentinel` keeps the host table and runs the track watch in
the reader task; `apps/app_rf/rf_ble.c` is the tool; the BLUETOOTH tile
goes live. The simulator's board plays a scene (phones, a tag that follows,
an Axon camera) through the same core code.

## Testing

Core tests with hand-built advertisements for every fingerprint, malformed
advertisement data, the report policy, the following rule with and without
a fix, marking, and address rotation. Simulator screenshots of the tool.
Hardware: the C5 lists the real devices in the room, the deck shows them,
capture writes frames. The following rule cannot be walked on the bench; it
is covered by the core tests only.
