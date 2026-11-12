# Sentinel suite, phase 4: wardriving

Status: written 2026-11-12 after Brian said "let's start phase 4". The
scope is his, from the phase 1 design: all radios with GPS, record-only, in
a format WiGLE reads. The decisions below are assumptions and are his to
overrule.

## What the user gets

Behind a new WARDRIVE tile, one screen and one key. `ENTER` starts a drive;
`ENTER` again, or leaving the tool, ends it. While it runs the node hops
every Wi-Fi channel and scans Bluetooth beside it, and the deck writes two
files under `sentinel/` on the SD card:

- `wigle-YYYYMMDD-HHMMSS.csv`: Wi-Fi access points and Bluetooth LE devices
  with where the deck was when it heard them, in WiGLE's own CSV format
  (WigleWifi-1.6). The file uploads to wigle.net as it is.
- `survey-YYYYMMDD-HHMMSS.csv`: the 433 MHz band and the 2.4 GHz band as
  the CC1101 and the nRF24 saw them, one row every 5 s with the position.
  WiGLE has no row type for raw band energy, so this is a file of its own,
  for a spreadsheet or a map.

The screen shows whether rows are being written (and why not, when they are
not: no fix, no SD card, node too old), the counts (networks, Bluetooth
devices, rows, sightings skipped and for which reason), the distance
covered, the time, the position, and the last few things recorded.

Nothing transmits. It works in a SAFE session and needs no arm lease.

## Decisions (assumptions)

1. **Passive only.** The node listens for beacons while it hops; it sends
   no probe requests. Every phone-based wardriver scans actively. Listening
   alone hears a little less per pass and keeps the "record-only" promise
   literal: a drive is undetectable and breaks no SAFE rule.
2. **A row needs a position and a time, both from the GPS.** No fix, an
   HDOP above 10, a GPS that stopped talking, or no date yet: the sighting
   is counted as skipped and not written. A row with a guessed position is
   worse than no row. `FirstSeen` is GPS time (UTC), never the deck's
   clock, so a deck that has not seen NTP still writes correct rows.
3. **One row per device per place.** A device gets a row when it is first
   heard with a fix, and another each time it is heard again after the deck
   has moved 25 m from where its last row was written. Standing still
   writes each network once; driving past writes a row every 25 m, which is
   what WiGLE's position estimate wants. The deck remembers the last 2048
   devices for this; in a longer drive an old device heard again gets a new
   row, which WiGLE merges.
4. **Networks that ask not to be mapped are not written.** A network name
   ending `_nomap` or containing `_optout` is skipped and counted. Those
   are the opt-outs the location services of Google and Microsoft honour.
5. **Bluetooth addresses that rotate are not written.** A phone, a watch or
   an earbud uses a private address that changes every few minutes: it
   cannot be found again, so it adds nothing to a map, and it belongs to a
   person. Public addresses and static random addresses (beacons, fixed
   equipment, trackers) are written.
6. **A drive belongs to the WARDRIVE screen.** Leaving the tool ends the
   drive and closes the files, because another tool takes the node's radio.
   Screen sleep does not end it: a drive is long, and the screen is most of
   the battery. The files are synced to the card every 5 s, so pulling the
   power loses a few seconds at most.
7. **SD card only**, like captures: the internal partition is 3.8 MB.
8. **Not in this phase.** Uploading to WiGLE from the deck (it would keep
   an account token on the device; the file is one copy away over the
   portal), a map view, LoRa, cell towers.

## Protocol additions (version 2, additive)

| Direction | Sentence | Meaning |
|---|---|---|
| host to node | `CMD,MODE,drive` | hop every Wi-Fi channel with the Bluetooth scan running beside it |
| node to host | `MODE,drive` | granted |
| node to host | `HELLO,...,drv=1` | this node has the drive tool |
| node to host | `GPS,...,<utc>,<date>` | one more field: the date as `ddmmyy`, `0` before the GPS gives one |

In `drive` the node sends the same `WBS` and `BDV` rows as in `wifi` and
`ble`, and no `WCH` rows: nobody is looking at a waterfall. The node's
access point table makes room by dropping an entry it has already reported
before one it has not, so a street of new networks is not lost to a full
table.

## The WiGLE file

```
WigleWifi-1.6,appRelease=<deck version>,model=<host>,release=<node firmware>,device=RF Sentinel,display=,board=<board>,brand=<brand>,star=Sol,body=3,subBody=0
MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type
a4:c3:f0:11:22:33,Afterlife,[WPA2][ESS],2026-11-12 15:04:05,11,2462,-63,40.741895,-73.989308,12,6.0,,,WIFI
c1:22:33:44:55:66,Tile,[BLE],2026-11-12 15:04:07,0,7936,-71,40.741895,-73.989308,12,6.0,,,BLE
```

- `AuthMode` is what the beacon proved and no more: `[ESS]` for an open
  network, `[WEP][ESS]`, `[WPA][ESS]`, `[WPA2][ESS]`, `[WPA3][ESS]`. The
  node does not parse the cipher or the key management, so it does not
  claim them.
- `AccuracyMeters` is HDOP times 5 m, the usual rule of thumb. The module
  does not report an accuracy of its own.
- A comma in a name becomes `_`, as WiGLE's own app writes it.
- Bluetooth rows: channel 0, `Frequency` 7936 (the Bluetooth class
  "uncategorized", which is what WiGLE's page shows for it), `MfgrId` the
  company number from the advertisement when there is one.

## Where the code goes

- `sentinel_core` (rf-sentinel): `sentinel_drive.h` decides which sightings
  become rows and keeps the counts; `sentinel_wigle.h` writes the lines.
  Both are plain C with host tests. The GPS date, `drive` in the command
  parser and the link machine.
- C5 firmware: the `drive` tool, the date from RMC, `drv=1`.
- `deck_sentinel`: the drive session (rows go through the writer task's
  queue like events), `sentinel_store` opens, syncs and closes the files.
- `apps/app_rf/rf_drive.c`: the screen. The chooser becomes four columns
  with WARDRIVE as tile 7 and TESTS (phase 6) as tile 8.
- `sim/sentinel_sim.c`: the pretend deck walks down a street so the
  simulator writes real files.

## Testing

- Host tests for the row rules (fix, distance, opt-outs, private
  addresses, a full table), the WiGLE lines byte for byte, the frequency of
  every channel, the date handling.
- The simulator writes both files; they are read back in the check.
- Hardware: both boards, a drive started and stopped, files read off the
  card, Bluetooth and Wi-Fi both present in one file. Moving 25 m needs
  the deck carried outside, and an upload to wigle.net needs Brian's
  account: both are his to do.
