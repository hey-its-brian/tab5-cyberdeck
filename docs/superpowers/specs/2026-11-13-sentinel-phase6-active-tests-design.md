# SENTINEL phase 6: active tests (2026-11-13)

The last phase of the suite, and the first that transmits on purpose. Two
tools, in two different places, because they are two different jobs:

- **BOARD > SELF TEST** asks whether each radio still works. It reads
  registers back and checks that receivers hear something. Nothing
  transmits, so it is not in the OFFENSIVE area.
- **OFFENSIVE > DEAUTH TEST** sends a short, counted burst of
  deauthentication frames at one chosen client of one chosen network, so
  that the deauth detector built in phase 3 can be seen to fire on
  something real instead of resting on its unit tests.

## What was decided before, and still holds

From 2026-11-11, unchanged:

- Jamming is never built.
- Anything that transmits is aimed at one chosen target, never broadcast.
- Transmit tests run only in OFFENSIVE mode, while the node confirms the
  arm lease. The node checks the lease itself; no screen can talk it into
  transmitting without one.
- No ownership proof is asked for (no saved password, no join-to-prove, no
  lab SSID prefix). Professional testers run black-box tests with the
  owner's permission and without the key. Instead, every run states in
  plain words that the network must be the user's own or tested with the
  owner's full permission and knowledge, and the user confirms.

## The deauth test

### Three steps, three pages

Under OFFENSIVE > DEAUTH TEST, pushed one on top of the other, so BACK
walks out of the test the way it walked in:

1. **PICK THE NETWORK.** The access points the board already hears, as in
   WI-FI > NETWORKS. Nothing is sent.
2. **PICK THE CLIENT.** The node parks on that network's channel and
   listens for frames between it and its clients, and the page lists the
   addresses it hears, with signal and how recently. Still nothing is
   sent: a client that says nothing while you watch does not appear.
3. **THE RUN.** States the target in full, the permission sentence, and
   exactly what will happen: how many frames, over how long, in which
   direction. One button, and it asks once more before anything is sent.

### What one run is

- `SENTINEL_DEAUTH_FRAMES` frames (8), `SENTINEL_DEAUTH_GAP_MS` apart
  (20 ms): about a sixth of a second in all.
- One direction only: addressed from the access point to the chosen
  client. That is what a detector sees, and it is the lesser of the two
  halves of the usual pair (the other, forged from the client to the
  access point, is what makes a client drop faster). The purpose here is
  to be seen, not to disconnect, so only this half is built.
- Reason code 7, "class 3 frame received from a non-associated station",
  the ordinary one.
- There is no continuous mode and no repeat key: another run is another
  deliberate press, warning and all.
- Every run is written to the log with the network, the client, the
  channel and the number of frames the node reported sending.

### What the core refuses, whatever a screen asks

`sentinel_test.h`, with its own tests, so the rules can be pinned:

- The client address must be a real one: not all zeroes, not the
  broadcast address, not any multicast address (the low bit of the first
  octet), and not the access point's own address.
- The access point's address must be a real one by the same rules.
- The channel must be one the radio may use.
- A run that fails any of these is refused with a reason, and the reason
  is what the screen shows.

The node applies the same rules again after parsing, so a host that
skipped them changes nothing.

### The technical risk

ESP-IDF refuses to transmit deauthentication frames: `esp_wifi_80211_tx`
passes every raw frame through `ieee80211_raw_frame_sanity_check` in the
closed Wi-Fi library, which rejects this type. The symbol is a strong one
in `libnet80211.a` for the C5, so the firmware can define its own and link
with `-z muldefs`.

That override is firmware-wide: it lifts the check for every raw frame,
not only this test. It is therefore in a file of its own
(`main/raw_tx_gate.c`) that says so at the top, and the limits that
matter live above it, in the core's rules and the arm lease. If the
override turns out not to work on this chip, the test says so on screen
and the rest of the phase still stands.

## The self test

`CMD,TX,SELFTEST` (armed or not: it transmits nothing), answered with one
`TST` row per radio: a name, PASS, FAIL or SKIPPED, and a line of detail.

| Radio | What is checked |
|---|---|
| 433 MHZ (CC1101) | the part and version registers read back as the chip they should be, and the signal estimator returns a level in a sane range |
| 2.4 GHZ (nRF24) | a register written and read back, and the carrier detector readable |
| GPS | sentences arriving, and how long ago the last one was |
| WI-FI | the driver is up and a channel can be set |
| BLUETOOTH | the scanner is running and advertisements are arriving |

No transmit loopback on the nRF24: a carrier is a jammer.

## The protocol

New, in `docs/protocol.md`:

- `CMD,TX,selftest` runs the self tests. `TST,<radio>,<result>,<detail>`
  rows come back, then `TST,done,<passed>,<failed>`.
- `CMD,STA,<bssid>,<channel>` parks the Wi-Fi monitor and lists clients of
  that access point; `CMD,STA,off` stops. `STA,<mac>,<rssi>,<frames>,<age ms>`
  rows come back.
- `CMD,TX,deauth,<bssid>,<station>,<channel>` runs one burst. The node
  answers `TXD,<sent>,<asked>` or `ERR,<reason>`.

## Build order

1. The core's rules and the protocol, with tests.
2. The node: the self tests, the station listener, then the burst.
3. The deck: BOARD > SELF TEST, then the three pages of the deauth test.
4. Checks, the simulator, then both boards.

## As built (2026-11-13)

- `sentinel_core/sentinel_test.h` holds the rules, the frame builder and
  the self test's row format. Eight mutations of it were tried, one per
  rule, and each one broke a test: that is what the rules being "pinned"
  means here.
- `SENTINEL_DEAUTH_BURST`, not `FRAMES`: `sentinel_wifimon.h` already had
  `SENTINEL_DEAUTH_FRAMES` for how many a detector must see to call it a
  flood, which is the other side of the same coin.
- `CMD,TX` now keeps its whole argument, so a test carries its target. It
  used to keep the first field only, and `v2_test.c` says so.
- The client listener became a mode rather than a command of its own:
  `CMD,STA` names the access point and `CMD,MODE,sta` starts it, the way
  the honeypot works. The first attempt had `CMD,STA` change the mode
  itself, and the deck and the board then spent their time putting each
  other back, because the host keeps a list of modes it will ask for and
  `sta` was not on it.
- The technical risk was real. ESP-IDF refused every frame until
  `main/raw_tx_gate.c` replaced `ieee80211_raw_frame_sanity_check`, and
  the first attempt at that silently did nothing: with only `-z muldefs`
  the linker never pulls in an archive member nothing has referenced, so
  the library's version won. `-u ieee80211_raw_frame_sanity_check` asks
  for the symbol before any library is searched, and the map then shows
  our four-byte version placed and the library's 0x212-byte one dropped.
- Both new screens are built from the same frame as the rest, so the
  three pages of the test are pushed and popped like NETWORKS' list and
  detail. One bug came out of that: pressing Enter on a row redrew the
  list the next page had just replaced, which asserted inside LVGL. The
  other list screens guard against it by checking their own "am I on the
  detail page" flag after the call; this one now compares the step.
- Checked in the simulator: the self test's five rows with one failure,
  all three pages of the deauth test, the permission dialog with the
  target in it, a run, and the deck's own deauth detector firing on that
  run a second later.

## Not done

- The C5 was not on USB while this was built, so the node's half of phase
  6 has never run on hardware. Everything the deck shows about it is
  therefore from the simulator's pretend node. The first thing to do with
  a cable is flash 0.8.0 and watch a real self test, then a real burst
  with the deck's detector watching.
- Whether a client actually drops is unknown and may well be no: only the
  half of the usual pair that a detector reads is sent.
- The deauth detector's own thresholds have never been tuned against a
  real burst, which is the whole reason this phase exists.
