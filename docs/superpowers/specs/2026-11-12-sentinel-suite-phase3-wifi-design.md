# Sentinel suite, phase 3: Wi-Fi watching

Status: written 2026-11-12 after Brian said "Start phase 3". The feature
list is his (packet monitor with a waterfall, channel analyzer, drone
detect, a honeypot for detecting attacks, detectors for common vectors).
The decisions below are assumptions and are his to overrule.

## What the user gets

Behind the WI-FI tile, four views:

- **MONITOR**: a waterfall of traffic, one column per channel (2.4 GHz 1 to
  13, 5 GHz 36 to 165), newest row on top, plus the last sweep as bars with
  frame counts by kind.
- **NETWORKS**: every access point heard, with channel, signal, security
  (open, WEP, WPA, WPA2, WPA3, and whether management frame protection is
  required) and how busy its channel is: the channel analyzer.
- **DRONES**: aircraft broadcasting Remote ID, with the ID and, when the
  broadcast carries it, position and altitude.
- **HONEYPOT** (RED TEAM only): an open access point with a name you
  choose and nothing behind it, which records who tries to join and whether
  anyone attacks it.

Detectors run whether or not the tile is open, and raise warnings that
reach the user in any app:

| Detector | Rule |
|---|---|
| Deauth flood | 10 deauthentication or disassociation frames for one network within 2 s of listening |
| Evil twin | a network name heard open from one access point and protected from another |
| Beacon flood | 30 access points never heard before within 10 s, after the first minute |
| Karma | one access point answering probes for 5 different names within 30 s |
| Drone | a Remote ID broadcast (Wi-Fi beacon or Bluetooth), or a DJI drone ID element |

`C` captures management frames to `sentinel/cap-wifi-<time>.pcap`.

## Decisions (assumptions)

1. **Detectors run in `watch`.** The C5 leaves promiscuous reception on, so
   its 15 s presence scan doubles as a slow sweep for the detectors (each
   channel is heard for about 120 ms every 15 s). Opening the WI-FI tile
   asks for `MODE,wifi`: the C5 hops all 38 channels continuously and the
   Bluetooth scan pauses. So background coverage is thin and foreground
   coverage is full; a sustained attack is caught either way, a single
   burst may be missed in the background.
2. **The node decides, with core code.** Detection needs every frame, and
   the link cannot carry every frame. `sentinel_wifimon` (in
   `sentinel_core`, tested on the host) parses frames on the node, keeps
   the tables, and raises the alerts as ordinary `EVT,warn` sentences.
   The host gets rows, not frames, unless capture is on.
3. **The honeypot transmits, so it lives in RED TEAM.** SAFE promises that
   nothing transmits, and an access point beacons. `MODE,hpot` is refused
   without the arm lease and ends when the lease does. The access point is
   open, has no IP stack behind it, and offers nothing: a station can
   associate and no more.
4. **Evil twin means a security mismatch, not a second access point.**
   Mesh systems and extenders reuse a name on many access points with the
   same security; that is normal. An open access point carrying the name
   of a protected network is the attack.
5. **Out of this phase.** Cameras by vendor prefix and skimmers by name
   (from the older roadmap) need a prefix list checked against the IEEE
   registry; they stay on the roadmap. Wardriving is phase 4.

## Protocol additions (version 2, additive)

| Direction | Sentence | Meaning |
|---|---|---|
| host to node | `CMD,MODE,wifi` | hop every channel, Bluetooth scan paused |
| host to node | `CMD,HPOT,<channel>,<name>` then `CMD,MODE,hpot` | set up, then start, the honeypot (armed only) |
| node to host | `WCH,<ch>,<ms>,<mgmt>,<data>,<ctrl>,<beacons>,<probes>,<deauths>,<max rssi>` | one dwell on one channel (`MODE,wifi`) |
| node to host | `WBS,<bssid>,<ch>,<rssi>,<sec>,<pmf>,<beacons>,<name>` | one access point: new, changed, or every 10 s |
| node to host | `DRN,<wifi or ble>,<mac>,<rssi>,<id>,<lat>,<lon>,<alt m>` | one Remote ID broadcast (position 0,0 when not given) |
| node to host | `HPT,<ch>,<stations>,<joins>,<probes>,<name>` | honeypot state, every 2 s while it runs |

`sec` is 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA3. `CAP,1` in `MODE,wifi`
streams management frames as `PKT,wifi`, dropping when the UART is full.

## Where the code goes

`sentinel_core`: `sentinel_wifi` (frame and element parsing, security,
channel list), `sentinel_odid` (Remote ID messages), `sentinel_wifimon`
(the node's tables and detectors), `sentinel_wifihost` (the host's access
point table, per-channel state, waterfall history, drone list).

`rf-sentinel` node: `components/wifimon` (promiscuous callback, channel
hop, honeypot access point), wired into `main.cpp`.

DECK//OS: `deck_sentinel` holds a `sentinel_wifihost_t` under its lock;
`apps/app_rf/rf_wifi.c` is the tool. The simulator's board plays a scene
through the same core code: hand-built beacons, a deauth burst, an open
twin, a drone.

## Testing

Core tests with hand-built frames for every detector and its near misses
(a mesh with one name, a single deauth, a busy but honest access point),
malformed elements, Remote ID decoding against known values. Simulator
screenshots of each view. Hardware: the real networks of the room listed
and counted. No attack or drone can be staged on the bench without phase 6,
so the detectors rest on their tests until then.
