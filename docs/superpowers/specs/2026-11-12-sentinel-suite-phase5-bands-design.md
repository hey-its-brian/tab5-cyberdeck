# Sentinel suite, phase 5: SUB-GHZ and 2.4 GHZ

Status: written 2026-11-12 after Brian said "Start phase 5". The scope is
his, from the phase 1 design (spectrum analyzer, IoT survey, TPMS, jam
detection) plus the two bench findings recorded in the roadmaps. The
decisions below are assumptions and are his to overrule.

## What the user gets

**SUB-GHZ** (tile 3), two views:

- **SPECTRUM**: the 433 MHz band as the CC1101 sweeps it (21 channels,
  433.0 to 435.0 MHz): the latest sweep with peak hold, the last minute or
  so as a waterfall, and for the selected channel its floor, mean, peak and how
  often it was busy.
- **BURSTS**: the CC1101 stops sweeping and listens to one frequency
  (433.92 MHz unless changed), timing every transmission it hears: when,
  how long, how strong, how long since the last one. Bursts that repeat are
  grouped into emitters ("every 43 s, 9 ms, -71 dBm"). This is the site
  survey for the band, and it is how a tyre sensor, a weather station or a
  remote shows up.

**2.4 GHZ** (tile 4): the band as the nRF24 sees it (126 channels of
carrier detect, 2400 to 2525 MHz): the latest report, the last sixty
reports (about five minutes) as a waterfall, where Wi-Fi channels 1, 6 and 11 and the three
Bluetooth advertising channels sit, and which of 1, 6 and 11 carries the
least energy (the WI-FI tool answers the same question by counting access
points; this one measures).

In both tools:

- `S` runs a **site survey**: 60 s of both bands, then
  `sentinel/site-<time>.csv` with every channel's floor, mean, peak and
  busy share, and the position when there is a fix.
- `B` tells the node that what it hears now is normal here (it learns its
  baseline again).
- A line says what the jam watch thinks.

**Jam watch**, in the background in any app. Two things raise a warning:

| Sign | Rule |
|---|---|
| 433 MHz band up | 11 or more of the 21 channels 8 dB over their baseline in two sweeps running |
| 2.4 GHz saturated | 40 or more of the 126 channels with a carrier in 6 of 8 sweeps, two reports running |

and the deck adds what it knows: when 2.4 GHz is saturated and Bluetooth
advertisements have fallen to a quarter of their usual rate, or a band
alarm coincides with the GPS losing satellites or signal, the warning says
"likely jamming" and names both. One band sign alone says "possible".

## Decisions (assumptions)

1. **No TPMS decoder in this phase.** US sensors transmit at 315 MHz and
   this module is a 433 MHz one, so nothing here could check a decoder
   against a real frame, and a decoder written from memory of other
   people's protocols is a guess. BURSTS finds TPMS-shaped transmissions
   (a few milliseconds, repeating) at 433.92 MHz without claiming to read
   them. Decoding stays on the roadmap, to be built against a known sensor.
2. **No Zigbee or Thread yet.** That is a third radio stack on the C5
   (802.15.4) sharing one antenna with Wi-Fi and Bluetooth. It stays on
   the roadmap as its own item. The site survey covers the two bands the
   board has dedicated radios for.
3. **A band-wide step is one event.** The node now looks at the whole sweep
   first. If half the band moves together it says so once ("433 MHz band up
   14 dB on 20 of 21 channels"), holds its per-channel alerts, and stops
   learning until the band is back, so a jammer never becomes the baseline.
   A band-wide drop is reported too ("went quiet"): that is the front end
   being switched off, or an antenna being unplugged.
4. **The 2.4 GHz band no longer warns per channel.** Measured in this flat:
   about 40 channels show some activity in every report and that is just
   Wi-Fi. "2462 MHz busy" twice a minute told nobody anything. The only
   2.4 GHz warning is saturation, which normal use does not come near
   (the most seen here was 4 saturated channels; the rule needs 40).
5. **The deck tells the node when to learn.** The deck switches the
   CC1101's front end on when RF WATCH starts, which moves every level at
   once. It now sends `CMD,LEARN` after that, once, and `B` sends it on
   request. Learning is never automatic after an alarm.
6. **BURSTS takes the CC1101 from the sweep.** While that view is open
   there are no 433 MHz sweep rows and no 433 MHz alerts. Leaving the view
   hands it back; so does the deck going quiet. The 2.4 GHz sweep and the
   C5's own radio carry on.
7. **Jam detection can only be tested one way here.** Nothing in this
   project may jam, so the rules rest on unit tests and on the bench
   showing they stay silent in a busy flat.

## Protocol additions (version 2, additive)

| Direction | Sentence | Meaning |
|---|---|---|
| node to host | `HELLO,...,sub=1` | this node has the band watch, `LEARN` and the burst listener |
| node to host | `BND,<sub or nrf>,<0 normal, 1 up, 2 low>,<channels>,<dB>` | a band's state, on change and every 10 s |
| node to host | `SBD,<kHz>,<floor dBm>,<bursts>` | the burst listener is on this frequency; every 2 s. `SBD,0,0,0` once when it stops |
| node to host | `BRS,<kHz>,<ms long>,<peak dBm>,<ms since the last one ended>,<0 or 1>` | one burst, when it ends; `1` for a carrier still up, sent every 5 s while it lasts |
| host to node | `CMD,LEARN` | what you hear now is normal: start both baselines again |
| host to node | `CMD,SUB,sweep` / `CMD,SUB,dwell,<kHz>` | sweep the band, or listen to one frequency (433000 to 435000) |

Fixed on the way: the first sweep after boot reported +120 dBm on
433.0 MHz (the RSSI conversion wrapped below -128).

## As built (2026-11-12)

- Measured on the node: with everything else it does, a 433 MHz sweep comes
  round about twice a second and a 2.4 GHz report every five seconds or so.
  So the waterfalls hold about a minute and a half and about five minutes,
  the baseline's warm-up is about 40 s, "two sweeps running" is a second
  and "two reports running" is ten.
- Found on the bench and by review, and fixed:
  - The burst listener was deaf after its first moment. The CC1101 leaves
    receive when it takes noise for a packet, and its signal strength
    reading then freezes. The listener now asks for a sync word (so noise is
    rarely a packet) and puts the chip back into receive whenever it finds
    it has left.
  - Once it could hear, it called single noise spikes bursts: about one a
    second, a millisecond long, 5 to 9 dB over the floor. A burst now needs
    four strong samples (two milliseconds on the node).
  - A signal that rises and stays is taken for the new floor after about
    half a minute, instead of being a carrier that hides every burst for
    good.
  - While the listener has the radio the band's state is not known, so an
    alarm is not left standing; and band rows are sent every 10 s whatever
    the state, so a lost "back to normal" cannot leave a host believing an
    alarm.
  - The jam verdict is dropped when the link is lost.
  - Statistics are exact over any length of run (a float running sum stops
    adding -100 exactly after eleven hours).
  - A survey with nothing swept is not written; a band with nothing swept
    has blank rows, not zeros.
- The link at 921600 baud lost a pair of frames every few minutes. The
  Tab5's UART interrupt fired with 8 bytes of FIFO left, which at that rate
  is 87 microseconds. It now fires with 96 left, and stays in RAM so a
  flash write cannot hold it off.
- Switching the CC1101's front end off and on from the Tab5 (the 2026-11-11
  case) moved nothing measurable on this board, so the "band went quiet"
  rule could not be seen on hardware and rests on its tests.

## Where the code goes

- `sentinel_core`: `sentinel_bandwatch.h` (the node's baselines and band
  rules, moved out of the firmware so they can be tested),
  `sentinel_burst.h` (the burst timer and the host's emitter grouping),
  `sentinel_bandhost.h` (the host's history, statistics, site survey rows
  and the jam watch). The new sentences and commands; `LEARN` and the
  CC1101's mode in the link machine.
- C5 firmware: the main loop uses the band watch, gains the dwell mode,
  and sweeps the nRF24 in slices while dwelling so the listener is rarely
  away for more than a few milliseconds.
- `deck_sentinel`: history kept in the background like the Wi-Fi tables;
  the site file.
- `apps/app_rf/rf_bands.c`: both tools (they share the chart).
- `sim/sentinel_sim.c`: a remote and a periodic sensor for BURSTS, and a
  jam scene that can be switched on for screenshots.

## Testing

- Host tests for every rule above, with the bench's real sweeps as the
  "must stay silent" case.
- Hardware: both tools open, the sweep and the dwell switch back and forth,
  a site file read off the card, ten minutes in the flat with no jam
  warning, and the deck restarted with the node powered (the case that
  used to raise 26 warnings). Whether BURSTS hears anything depends on what
  transmits at 433.92 MHz near the bench.
