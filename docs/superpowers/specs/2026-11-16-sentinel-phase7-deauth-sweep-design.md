# SENTINEL phase 7: the deauth sweep (2026-11-16)

Phase 6 proved that one frame can be sent at one chosen client, and that
the detector fires on it. This phase turns that single shot into a tool
you would actually reach for, and adds the half that was missing: what an
attacker walks away with.

## Why

The owner wants to walk the whole path someone might take against his own
network, end to end, including the hashed material that comes back and
what can be done with it offline. A test that knocks off one device and
reports "8 frames sent" does not answer the questions he actually has,
which are: which of my devices fall over when someone does this, and what
does the attacker now hold?

Phase 6's screen was also named badly. "DEAUTH TEST" reads like
scaffolding from a beta that nobody removed. The thing is not a test of
the deck. It is a tool pointed at a network.

## What was decided before, and still holds

From 2026-11-11 and 2026-11-13, unchanged:

- Jamming is never built.
- Anything that transmits is aimed at one chosen target, never broadcast.
- Transmit runs only in OFFENSIVE mode, while the node confirms the arm
  lease. The node checks the lease itself; no screen can talk it into
  transmitting without one.
- No ownership proof is asked for. Every run states in plain words that
  the network must be the user's own, or tested with the owner's full
  permission and knowledge, and the user confirms.
- One screen, one purpose. Ask why am I on this screen.

The targeted-only rule is the one that shapes this phase most, so it is
worth restating why it survives contact with "sweep the network": every
frame the sweep sends still names one real client, taken from the list the
node heard on that network. There is no broadcast frame anywhere in it.

## The name

**OFFENSIVE > DEAUTH SWEEP.** "Sweep" says what it does: it walks the
clients of one network. SELF TEST keeps its name, because a self test
genuinely is one.

Choosing the name was the author's call from a shortlist (DEAUTH SWEEP,
KNOCK OFF, EVICT) and the owner accepted it.

## The sweep

### What it is

Point it at one network. It takes the client list the node already builds,
and works each client in turn: one burst, a pause long enough for the
device to come back, then on to the next. Clients that have given up a
usable handshake drop out of the rotation. It keeps going round until
every client has yielded one, or a cap stops it, or the operator does.

Round-robin rather than hammering one device, for two reasons. It spreads
the disturbance, and it means a device that is slow to reconnect gets its
time without blocking the others.

### What each client's row says

The point of a sweep over a single shot is attribution, so the row has to
earn it. Per client:

- **SIG** the signal, as the client list already reports it.
- **DROPPED** whether this device actually fell over.
  - `yes` its traffic stopped after a burst and then resumed, or a fresh
    handshake was seen from it.
  - `no` its traffic carried on through the burst without a gap.
  - `quiet` it was not transmitting when we hit it, so there is nothing to
    compare and the honest answer is that we cannot tell.
- **HANDSHAKE** what was captured: `PMKID`, or which of `M1 M2 M3 M4` have
  been seen, with a dot for the ones that have not.

`quiet` is deliberately not folded into `no`. A tool that reports "no" for
a device it never had evidence about is lying quietly, and this one gets
used to make decisions about the owner's own network.

A client counts as **caught** once there is enough for hashcat: a PMKID,
or M1 and M2 together. All four messages are not required, and waiting for
them would make the sweep run long for nothing.

### Persistence and the caps

The owner asked for it to keep working a client rather than one pass and
done. It does, with a ceiling:

- One burst per client per pass. A burst is now two-directional: see
  "Both halves of the burst" below. The caps are counted in total frames
  sent, not in bursts, so that doubling the frames per burst does not
  quietly double the ceiling.
- A reconnect window after each burst, long enough for the device to come
  back and complete a handshake.
- Up to a fixed number of passes, and an overall time limit for the run.
- A client drops out as soon as it is caught.

The caps are the author's call, not the owner's request. The reason is not
caution for its own sake: repetition without a ceiling against one device
stops being a targeted test and becomes denial of service, which is the
line this project drew on purpose. The numbers are shown on the screen and
are the owner's to change.

The arm lease does real work here too. It needs the screen, and it dies
within 5 seconds of leaving it, so a sweep cannot be walked away from and
left transmitting.

### Both halves of the burst

Phase 6 sent only the half addressed from the access point to the client,
and said so in its own "Not done" note: whether a client actually drops was
unknown, and the forged half from the client to the access point "is what
makes a client drop faster". Today's hardware run confirmed the frames
leave the radio (`TXD,8,8` twice), which is not the same as confirming that
anything fell over.

That gap matters here in a way it did not in phase 6. Every column of the
sweep except SIG depends on devices actually dropping: DROPPED, HANDSHAKE,
the caught count, and the entire premise that a reconnect yields M1 and M2
or a PMKID. A one-directional burst that disturbs nothing would make the
sweep report `no` for every row, and the owner could not tell a resilient
network from a tool that was too gentle.

So phase 7 adds the second half. The owner decided this during review.

- The new frame is a deauthentication addressed to the access point with
  the chosen client as its source, the mirror of the phase 6 frame.
- It is still targeted. One named client, one named access point, taken
  from the list the node heard. No broadcast or group address appears, in
  either direction.
- It needs its own builder in `sentinel_core`, its own targeting rules, and
  its own mutation tests, in the same shape as the phase 6 rules: every
  rule gets a test that fails when the rule is removed.
- It forges a source address. That is worth naming plainly rather than
  leaving implicit, and it is the reason this stays behind the arm lease,
  the OFFENSIVE mode, and the permission sentence.

A null result is still a real result and the screen must be able to express
it. "Nothing on my network dropped, with both halves and the caps spent" is
a finding the owner specifically wants to be able to reach.

### Protected frames, PMF and WPA3

`sentinel_wbs_t` already carries `pmf` and `sec`, and the sweep has to use
them, because on a network with protected management frames required an
unprotected deauthentication is ignored by specification. Nothing will
drop, no matter how many passes run.

The owner decided during review: surface it, and let the run proceed.

- The network pick page shows security and whether PMF is required, so the
  choice is informed before anything is sent.
- A sweep against a PMF-required network still runs. Rows report honestly
  that nothing dropped.
- On WPA3-SAE there is no four-way handshake material worth capturing, so
  the HANDSHAKE column needs a state meaning "not usable on this network"
  rather than sitting at dots as though a catch were still coming, or worse
  claiming `PMKID` about material that will never crack.

The reasoning for running anyway rather than refusing: discovering that his
own network shrugs the whole thing off is the reassuring answer the owner is
testing for. A refusal would hide the demonstration that the protection
works, and would have the deck decide for him what is worth testing.

Note that a transition-mode network can have some clients protected and
some not, so this is per-run context rather than a single verdict, and the
rows remain the place the truth is told.

### What the log and the alert bar do during a run

The deck's own deauth detector hears the sweep's own frames, and
`sentinel_wifimon_frame` will raise a flood alert on them. Five clients
over three passes is fifteen bursts through an eight deep event ring and
the alert bar added in phase 6, which would leave the bar carrying nothing
but the deck's own transmissions during the one run the owner is watching
closest.

The owner decided during review: attribute, never suppress.

- The detector keeps running and keeps logging. It is not muted, not for
  our own frames and not for the duration of a run. A detector switched off
  while the operator is paying close attention to the radio is the worst
  possible moment to be blind, and someone else deauthing on that channel
  mid-run must still reach him.
- Alerts raised while our own sweep is transmitting are tagged as ours, so
  the bar reads as the sweep rather than an unknown attacker.
- The run logs once with a per-client summary, rather than a warn event per
  burst. This is the author's call: the audit trail phase 6 established is
  kept, including every target, without fifteen rows saying the same thing.

### The screen

One screen, under OFFENSIVE, reached the way the phase 6 pages were:
pick the network, then the sweep itself. The client list page phase 6
needed as a separate step is gone, because picking one client is no longer
the point.

```
 HOME            DEAUTH SWEEP                      BACK
 Knock each client off and catch the handshake.
 ------------------------------------------------------
  CLIENT              SIG   DROPPED   HANDSHAKE
  10:2C:B1:C3:8C:8B   -47   yes       M1 M2 M3 M4
  8C:90:2D:23:C0:4C   -53   yes       PMKID
  10:2C:B1:41:32:83   -50   no        .  .  .  .
  10:2C:B1:40:F5:C5   -64   yes       M1 M2 .  .
  10:2C:B1:5B:4C:9F   -56   quiet     .  .  .  .
 ------------------------------------------------------
  pass 3    armed 4.2s    caught 2 of 5
  cap-wifi-20261116-170210.pcap
 ------------------------------------------------------
  ENTER  START THE SWEEP              S  STOP
```

The permission sentence from phase 6 still appears before the first frame,
and matters more now that a run touches every device on the network rather
than one the operator pointed at individually.

## Catching the handshake

### Almost all of this already exists

Worth stating plainly, because it changes how large this phase is:

- In client-list mode the node's sniffer callback already receives data
  frames with their payload. It hands them to `client_of` to learn who the
  clients are, then counts them and drops them.
- `CMD,CAP,<0|1>` is already a host command, and `wifimon_capture()`
  already installs the emit hook.
- Captured frames already travel as `PKT,<link>,<ch>,<rssi>,<us>,<orig_len>,<base64>`,
  which `sentinel_proto` already parses.
- The deck already routes those to `sentinel_store_packet()`, which writes
  `cap-wifi-YYYYMMDD-HHMMSS.pcap` on the card with correct pcap headers and
  timestamps, and survives the card going away.
- The link runs at 921600, well above the 460800 that captured frames need.

So there is no new protocol in this phase. Nothing is added to the sentence
set.

### What is new

On the node: recognize EAPOL inside a data frame and let it reach the emit
hook instead of being counted and dropped. An EAPOL frame is identified by
the LLC/SNAP header and ethertype `0x888E` after the 802.11 header, with
the header length depending on whether the frame is QoS and whether it
carries four addresses. The handshake is sent before keys are installed, so
these frames are not protected and need no decryption.

The ESP-IDF sniffer documentation settles the question of whether the
payload arrives intact: data frames are dumped to the application in full,
"including MPDU, AMPDU, and AMSDU", and only MIMO frames are reduced to a
length. So no spike is needed to find out whether EAPOL survives the
driver. One assumption follows from that same sentence and should be
written down rather than discovered later: because aggregated frames are
also delivered, an EAPOL frame could in principle arrive inside an AMSDU
rather than as a bare MPDU, which parses differently. In practice handshake
frames precede any block-ack session and arrive as plain MPDUs, so the
parser may assume that and reject what it does not understand rather than
guessing.

On the deck: read the EAPOL key-information bits of the frames it is
already handed, to know which of M1 to M4 each one is and whether M1 holds
a PMKID, and keep that per client for the row. The frames themselves need
no special handling to reach the card, because the existing packet path
already takes them there.

### What lands on the card, and what the screen may claim

A pcap, through the path that already exists. Wireshark opens it, and
`hcxpcapngtool` converts it for hashcat.

Beacons for the target network must be in the same file, so the capture
carries the SSID and converts without the operator supplying it by hand.

There are three places a captured frame is silently dropped between the air
and the file: the node's packet queue, which discards rather than waits; the
deck's store queue, which counts dropped packets; and
`sentinel_store_packet` itself when the card is absent or a write fails. So
a screen that reports a handshake from the frames it parsed can be claiming
a catch that is not in the file. That is the anti-goal about a capture that
looks fine and is not usable, reached from the other side.

Decided, as the author's call:

- The handshake state the screen shows is tracked where the file is
  written, not where the sentence is parsed, so the screen only claims what
  reached the card. `sentinel_store` has to report a written count and the
  open file's name back to the app; it currently exports neither.
- Capture during a sweep is narrowed to what the run needs: the target
  network's management frames, for the beacons, plus EAPOL. Parked on a busy
  channel, every beacon from every access point in range would otherwise
  compete with the EAPOL frames the run exists to collect, at roughly 370
  bytes per sentence through a shallow queue.
- A run that is dropping captured frames says so on the screen. A run with
  no card still runs and still shows handshake state, and says plainly that
  nothing was written, rather than naming a file that does not exist.
- `SENTINEL_PKT_MAX` stays at 256 and the protocol does not change. M1 and
  M2 together are about 155 bytes, and they are the catch threshold, so the
  material that matters fits. A long M3 with an encrypted GTK, and modern
  beacons carrying HT, VHT, HE and RSN elements, will be truncated with a
  correct larger `orig_len`, which is ordinary snap-length behaviour that
  the conversion tools read. Raising the cap would push
  `SENTINEL_LINE_MAX` past the size it was built for and would break any
  node and host not flashed as a pair, which is a worse trade than a
  truncated M3 the catch does not depend on.

The deck does not attempt to recover the passphrase. That is not a judgment
about whether he should; it is that the P4 is the wrong machine for it and
the value of this tool is the handoff.

## Approaches

### Where the sweep loop lives

- **On the deck (chosen).** The deck sends one `CMD,TX,deauth` at a time,
  watches the client rows and the captured frames, decides who is next, and
  keeps asserting the lease. The node's primitive is untouched, still
  rule-checked per frame and now verified on hardware.
- **On the node.** A new sweep command that loops inside the node. Fewer
  sentences, but the node would then transmit autonomously for minutes off
  one command, which is exactly what the 5 second lease exists to prevent,
  and target selection would drift away from the per-frame rules the tests
  pin. Rejected.

### What the operator ends up holding

- **A pcap, via the existing path (chosen), plus per-client handshake state
  on the screen.** Costs almost nothing, because the path is built. The
  on-screen state is what makes it usable at the bench: a pcap cannot tell
  you whether you got something, and standing next to the deck that is the
  only question.
- **The node extracts the hashcat 22000 fields and the deck writes a
  `.22000`.** More convenient, no conversion step. Rejected for now because
  it is new crypto-field parsing with a bad failure mode: a wrong byte
  offset produces a file that hashcat rejects, or silently never cracks,
  and the deck cannot tell the operator which. Worth adding later, once
  there are real pcaps from this phase to validate an extractor against.
- **Both at once.** Rejected as the first step, for the reason above.

### How hard the burst pushes

- **Both halves, AP to client and the forged client to AP (chosen by the
  owner).** The only version that can tell a resilient network from a
  gentle tool.
- **One half, as phase 6 built it.** Less code, no forged source address,
  but a null result is ambiguous, and phase 6's own notes predicted that
  null result.

### Networks the test cannot work against

- **Show security and PMF, run anyway, say so in the rows (chosen by the
  owner).** The demonstration that the protection works is itself the
  answer he wants.
- **Refuse PMF-required networks.** Saves a screen of meaningless rows, but
  hides the finding and lets the deck decide what is worth testing.

### The deck's own detector during a run

- **Attribute, never suppress (chosen by the owner).** Tag alerts raised
  while our own sweep transmits; silence nothing.
- **Suppress for the duration.** A clean screen bought with a blind window
  at the exact moment the operator is watching the radio.
- **Leave it alone.** Honest, no new code, unreadable during the run that
  matters most.

### How "dropped" is decided

- **Compare traffic across the burst, and treat a fresh handshake as proof
  (chosen).** Uses what the node already reports.
- **Watch for the reassociation exchange only.** Cleaner in principle, but
  it misses devices that drop and do not come back while watching, and
  reports nothing useful about a device that was idle.

## Goals

- Point it at a network, not a device, and see which devices actually fall
  over, per device.
- Keep working the clients that have not yielded, within a visible ceiling.
- Come away with a capture the owner can analyze offline with ordinary
  tools.
- Say at the bench whether a usable handshake was caught, and for which
  client.
- A name that does not read as leftover scaffolding.

## Non-goals

- Broadcast or group-addressed deauthentication.
- Jamming, continuous transmit, or any count the host can raise without a
  ceiling.
- Recovering the passphrase on the deck.
- Any new protocol sentence.
- Attacking networks that are not the owner's, which no amount of UI can
  enforce and which the permission sentence addresses instead.

## Anti-goals

Things that would make this a failure even if it shipped and worked:

- A sweep that cannot say which device dropped. That is the whole reason
  not to send a broadcast frame, and losing attribution would make the
  feature pointless.
- Reporting `no` for a device that was never heard from. Guessing, in a
  tool used to make decisions about the owner's own network.
- A capture file that looks fine and is not usable, with no way to tell
  from the deck.
- Transmit that continues when the operator has walked away.
- Unbounded repetition against one device.

## Constraints

- The arm lease: `CMD,ARM,1` repeated, 5 second lease, refreshed at 1.5
  seconds. Checked by the node, not the screen.
- Targeting rules live in `sentinel_core` and are enforced on the node after
  parsing: one real client, not broadcast, group, empty, or the access
  point itself, on a channel the radio uses.
- `SENTINEL_PKT_MAX` is 256 bytes. EAPOL frames fit.
- Captured frames need at least 460800 baud on the link.
- The SD card may be absent. The sweep still runs and still shows handshake
  state; only the file is missing, and the screen should say so rather than
  implying a capture was written.
- LVGL 9.5 on the P4, inside the existing `sn_frame` furniture.
- The sweep parks the monitor on the target network's channel, so no
  hopping during a run.
- Protected management frames: where PMF is required, unprotected
  deauthentication is ignored by specification and nothing will drop.
- Captured frames pass through three queues that drop rather than block,
  so "heard" and "written to the card" are different facts.
- `sentinel_host.c` currently gates capture on a capturing tool of `ble` or
  `wifi` only. The sweep needs mode `sta` and `CMD,CAP,1` at the same time,
  so that gate has to admit `sta` or capture is never actually asked for
  during a run.
- `sentinel_host_t.once` holds two queued single-shot commands. A sweep
  issues one per client per pass, so it needs a small ring, or the sweep
  issues them one at a time off its own clock.
- `STATIONS_MAX` is 12 and entries do not age out.

## What is left to the builder

- The cap numbers, the reconnect window, and the quiet threshold. Pick
  defaults that let a five client network finish in a couple of minutes,
  put them on the screen, and keep them in one place. Remember the caps
  count total frames, now that a burst has two halves.
- Where the EAPOL parsing lives. It is a candidate for `sentinel_core` with
  unit tests over captured bytes, which is how the rest of the frame
  handling in this project is pinned, and it would let the M1 to M4 and
  PMKID logic be tested without a radio.
- Whether the target's beacons are already reaching the card during a
  sweep, or whether capture has to start slightly before the parking.
- The exact wording of the rows, in the plain voice the rest of SENTINEL
  uses, including the "not usable on this network" state.
- How the sweep and the client list share the node's `sta` mode, which the
  sweep needs running throughout.

### Calls the author made, for the owner to check

These were settled rather than asked, because a sensible builder could
settle them and a wrong choice is recoverable:

- **One BSSID per run, not an SSID.** The pick page names the other BSSIDs
  sharing an SSID so the owner can sweep each in turn. With band steering a
  client knocked off 2.4 GHz often returns on 5 GHz, where a parked monitor
  cannot hear it, so a client that leaves and does not come back gets a row
  state saying it may have moved band rather than being scored as dropped.
  Hopping during a run was rejected because it would lose the handshake.
- **The client roster is frozen when the run starts**, which is what the
  caught count is counted against. A client first heard mid-run still gets
  a row and still gets swept, counted separately so the denominator does
  not move under the owner. A device that returns with a rotated address
  appears as a new row, and the screen says so rather than leaving two
  half-answered rows for one device.
- **Handshake state is tracked at the file**, not at the parse, and
  `SENTINEL_PKT_MAX` stays 256. Both are argued above.
- **One log entry per run** with a per-client summary, instead of a warn
  event per burst.

## Checkpoint

Phase 7 is done when, on the owner's own network:

1. The sweep enumerates the clients and works them in rotation, with the
   caps visible and honoured.
2. At least one client shows `dropped yes` with a handshake caught, and at
   least one row honestly shows `quiet` or `no` when that is the truth. If
   nothing on the network drops with both halves sent and the caps spent,
   that is a pass for the tool and a finding about the network, and the
   screen has to make which one it is unambiguous.
3. A `cap-wifi-*.pcap` on the card opens in Wireshark, shows the EAPOL
   frames and the target network's beacons, and converts with
   `hcxpcapngtool` into something hashcat accepts.
4. Leaving the screen stops transmit within the lease window.
5. A sweep against a PMF-required network reports honestly instead of
   implying a usable catch.
6. The deck's own bursts appear in the alert bar attributed to the sweep,
   and a deauth from somewhere else during a run still raises an alert.
7. `tools/check.sh --firmware` passes, with the new targeting rules for the
   client-to-AP frame covered by tests that fail when the rule is mutated.
