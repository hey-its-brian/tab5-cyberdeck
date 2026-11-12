> Independent review by a separate agent, 2026-11-12, of RF WATCH at commit
> 18fd667. It was given the code and the simulator, not the author's
> opinions. Nothing in it has been acted on yet. The claims the next steps
> would rest on (the RED TEAM digit on the gate, banners skipped and the
> count cleared while RF WATCH is open, the dim colour, "MHz active" raised
> as a warning, B with no confirmation) were checked against the code and
> hold; the key swallowing it cites at deck_shell.c:314 is at line 290.

# RF WATCH usability and accessibility review

Review only: no repository file was edited (`git status` before and after is identical). Everything below marked **[seen]** was verified by looking at a screenshot I rendered from `sim/build/deck_sim`; **[code]** means inferred from source only. Code citations are relative to the repository root `/Users/brianmeyer/claude/tab5-cyberdeck/sentinel-useful-93ca39`. Screenshots are in the scratch directory listed at the end; in the findings I name them by number prefix (for example "shot 77").

Physical numbers used throughout: 1280x720 on a 5 inch diagonal is 294 ppi, 0.0865 mm per pixel. 9 mm is 104 px, 7 mm is 81 px.

---

## 1. Verdict

As an instrument for someone who already knows it, sitting at a desk with the keyboard, it works: the data is there, the navigation model is simple, WARDRIVE and BLUETOOTH are close to good. As a safeguarding tool that is "clear at a glance and easy to operate" in the hand, it is not there yet. Three things hold it back, in this order:

1. **Warnings do not land.** Inside RF WATCH there is no place that says "something is wrong now". With likely jamming in progress the tools page shows nothing, OVERVIEW's biggest word is a green ONLINE, the home tile says SENTINEL ONLINE in green, the screen saver shows a clock, and tapping the warning count takes you to the SAFE or RED TEAM question and clears the count. Everything from "weather station chirped" to "likely jamming" is the same yellow.
2. **Almost everything is written too small and too dim.** About 95 percent of the text is 16 px or 20 px monospace (capital height 0.95 mm and 1.2 mm), much of it in a grey at 3.45:1 contrast. Only the 36 px state words are comfortably readable at arm's length.
3. **It can be looked at by touch but not operated by touch, and several one-key actions are unguarded.** Nothing except navigation and Bluetooth row selection is tappable, touch targets that exist are 3 to 4 mm tall, and single keystrokes can clear a jam alarm (B), end a drive (Enter, Space, Esc, Del, BACK), drop a running survey, or arm RED TEAM by type-ahead.

My recommendation is a structural rework of the first two levels (gate and chooser) plus a common screen frame, described in section 5. The individual tools need less change than the shell around them.

---

## 2. Findings, most harmful first

### F1. A warning in progress is invisible on the tools page and understated everywhere else
- **Tools page during likely jamming [seen, shot 77]:** identical to a quiet day. Chip says SAFE, eight calm tiles. Cause: banners are skipped while RF WATCH is on screen (`components/deck_core/deck_notice.c:83`) and the unseen count is zeroed every 500 ms while the app is open (`apps/app_rf/app_rf.c:421-422`), and nothing replaces them inside the app.
- **OVERVIEW during likely jamming [seen, shot 79]:** the largest text is a green ONLINE; the 2.4 GHz status line is yellow, but it is also yellow on a normal day (see F3); the only statement of the problem is one 16 px yellow row in EVENTS, already pushed to third place by "433.9 MHz quiet". 
- **SUB-GHZ during 2.4 GHz jamming [seen, shot 78]:** a big green NORMAL with a small red "JAM WATCH LIKELY..." line under it. Technically right (433 is normal), reads as "normal".
- **WI-FI MONITOR during a deauth flood [seen, shot h2]:** one red cell of about 19x20 px in the waterfall, and a text line that shows a non-zero count for one sweep only (`apps/app_rf/rf_wifi.c:199-200`). The detector's verdict ("deauth flood: Afterlife ch 11") appears nowhere on that screen; nor does the evil twin.
- **Home tile during jamming [seen, shot a5]:** "SENTINEL ONLINE" in green, "2.4 GHZ 101 BUSY" in dim grey (`apps/app_rf/app_rf.c:443-455`).

Why it matters: the user's question on every screen is "is anything wrong", and the product answers it only if he happens to open LOG.

Change: one alert strip owned by RF WATCH, shown on every RF WATCH screen (tools page and every tool) directly under the status bar, and mirrored on the home tile. Quiet: a thin line, "ALL CLEAR, LAST WARNING 14 MIN AGO". Active: full width, 28 px text, an icon, plain words and age: "LIKELY JAMMING: 2.4 GHZ SATURATED, BLUETOOTH DOWN FROM 32 TO 2 PER SECOND. 40 S AGO", coloured by severity, staying until the condition ends or it is acknowledged. Enter or a tap opens the tool that explains it. Each tile on the tools page gets a badge when its tool has something to say.

### F2. From a warning to its explanation takes four steps and loses the warning on the way
[seen, shots a8, b1] Tapping the warning count or a banner opens RF WATCH at "CHOOSE THIS SESSION'S MODE". The count is cleared the moment the app starts (`apps/app_rf/app_rf.c:350`), the gate asks an unrelated question, the tools page then shows nothing about it, and the user has to guess that LOG (key 6) is where it went.
Change: a tap on a banner or the count opens RF WATCH in SAFE straight to the alert (the LOG filtered to warnings, or the tool that owns the warning, with that row selected). The count clears when the warnings have been shown, not when the app starts.

### F3. One warning colour for everything, and yellow used for normal
- There are two levels, info and warn (`components/deck_core/include/deck_notice.h:21`). "Likely jamming", "deauth flood", "evil twin", "Flock device nearby", "drone", and "433.9 MHz active (-61 dBm)" are all the same yellow in LOG, EVENTS, the banner and the count [seen, shots 80, e1, a2, a5, a8]. The node raises "MHz active" as a warning for any channel that rises (`external/rf-sentinel/sentinel_core/src/sentinel_bandwatch.c:95`), so a neighbour's weather station produces a yellow row, a count and a banner every time it transmits. In the simulated scene the log is wall to wall "433.9 MHz active / quiet" [seen, shot e1] and the count reached 8 in 80 seconds [seen, shot a8].
- OVERVIEW paints the 2.4 GHz line yellow whenever any channel reaches 6 of 8 (`apps/app_rf/rf_overview.c:257`), which ordinary Wi-Fi does constantly, and the Bluetooth line yellow whenever any tracker label exists (`rf_overview.c:146`). Both were yellow in every normal capture [seen, shot 06] and looked the same during jamming [seen, shot 79].
- BLUETOOTH paints "TRACKERS 3" red because a surveillance device exists (`apps/app_rf/rf_ble.c:93`) [seen, shot 92].

Why it matters: when yellow is always on, yellow stops meaning anything, and the count in the status bar becomes noise within a day.

Change: three levels with fixed meaning and a glyph each, used identically in every tool: NOTE (white, no count, no banner: tracker first heard, 433 activity, fix gained), WARNING (yellow triangle: tracker staying with you, open twin, drone, surveillance device), ALERT (red filled octagon or bar, stays on screen: likely jamming, deauth flood, karma, beacon flood). Status lines are coloured only when they are abnormal against the learned baseline, never for "busy".

### F4. The most serious banner can be dropped, and nothing reaches a sleeping or pocketed deck
- Banners are first come, first shown, with a 10 s quiet period (`components/deck_core/deck_notice.c:85`, `deck_notice.h:23-24`). In the jam scene the banner shown was "2.4 GHz band saturated: 101 of 126 channels"; the "likely jamming" verdict one second later fell in the quiet period and never got a banner; the next banner was a drone [seen, shots a5, a7, a8].
- With the saver up during jamming the screen is a clock and a date, nothing else [seen, shot a12]. Banners are skipped while the screen sleeps or the saver runs (`deck_notice.c:84`), and by design nothing makes a sound (`deck_notice.h:5`).

Why it matters: the warnings this tool exists for (a tracker that has stayed with you for ten minutes, jamming) arrive while the deck is in a bag or propped and idle. Today they are a number in a status bar that is not being drawn.

Change: a higher severity always replaces a lower one on the banner and ignores the quiet period; ALERT banners stay until acknowledged. The saver shows the unseen warnings under the clock ("3 WARNINGS, LATEST: TRACKER WITH YOU 12 MIN") and an ALERT wakes the screen. Add an opt-in sound or keyboard backlight flash for ALERT level; silence should be a setting, not the only behaviour.

### F5. Text size and contrast
Font sizes are set in `components/deck_ui/deck_theme.c:93-100`; palette at `deck_theme.c:53-64`.

| Font | px | Capital height | Angle at 35 cm | at 50 cm | Used for |
|---|---|---|---|---|---|
| mono_s | 16 | 0.95 mm (measured 11 px) | 9.5 arcmin | 6.7 | hint lines, almost all status and explanatory text, tile taglines, gate warning text |
| disp_s | 18 | 1.1 mm | 10.7 | 7.5 | tabs, panel titles |
| mono_m | 20 | 1.2 mm | 11.9 | 8.3 | list rows, banner |
| disp_m | 24 | 1.45 mm | 14.3 | 10.0 | state words in bands, tile names |
| disp_l | 36 | 2.2 mm | 21.4 | 15.0 | big counts, WARDRIVE state |

The usual human factors floor is about 16 arcmin of character height, 20 or more preferred. Only the 36 px face meets it in the hand; nothing below 36 px meets it propped on a desk. For comparison, 16 px here is smaller than the smallest caption text a phone uses (about 1.2 mm capitals).

Contrast against the panel colour (#0D0F1C):

| Colour | Ratio | Note |
|---|---|---|
| text #D8E1F0 | 14.5:1 | fine |
| dim #5F6884 | 3.45:1 | fails 4.5:1; carries hints, explanations, most status lines, the gate's RED TEAM warning, the honeypot name |
| danger #FF3B3B | 5.4:1 | the lowest of the state colours, and the most important; 3.7:1 as seen with protanopia |
| accent2 #FF2A6D | 5.3:1 | |
| warn, ok, accent | 13.5 to 14.6:1 | fine |
| selected row fill #171A30 on panel | 1.11:1 | the selection highlight, see F12 |

[seen] In every tool the dim 16 px text is the hardest thing on the screen and there is a lot of it; outdoors it will be gone. The RED TEAM permission sentence on the gate is in this style (shot 01), as is the honeypot's NAME value (shot 39).

Change: nothing under 20 px; 24 px for anything the user must read to operate (state lines, list rows, selected item detail, hints that remain); 36 px or more for the one state word per screen. Raise dim to about #8E98B5 (6.6:1) and stop using dim for content, keep it for units and labels. Pay for the larger type by deleting text (F9, F10), not by shrinking panels. Lists drop from 14 or 15 rows to 9 or 10; that is the right trade.

### F6. One-keystroke actions with no guard, and silent losses
- **B, "learn this as normal", during jamming [seen, shots 74, 75]:** one press turns a red "SATURATED 101 CH / JAM WATCH LIKELY" into green "NORMAL / JAM WATCH NONE SEEN" while the chart is still solid, and writes "jam watch: bands back to normal" into the log [seen, shot 80]. In the simulator it re-alarms a few seconds later; on hardware the baseline is being re-learned from the jammed band (`apps/app_rf/rf_bands.c:545-551`, `components/deck_sentinel/include/deck_sentinel.h:121-122`) [code for the hardware consequence]. B is next to N and V on the keyboard. Change: confirm dialog, CANCEL focused, wording that says what is lost ("TREAT WHAT IS ON THE AIR NOW AS NORMAL? THE NODE RELEARNS BOTH BANDS FOR 40 S AND STOPS REPORTING WHAT IT HEARS NOW."), and refuse or add a second warning while a jam state is active. Log it as "baseline reset by user", not "back to normal".
- **RED TEAM by type-ahead [seen, shot g0]:** Alt+X, Enter, 2 typed quickly (the habit for "open, SAFE, WI-FI") arms RED TEAM, because keys are swallowed during the 260 ms transition (`components/deck_core/deck_shell.c:314`) so the Enter is lost and the 2 lands on the gate, where a bare 2 means RED TEAM (`apps/app_rf/app_rf.c:221-226`). The hint does not mention the digit keys. Change: remove the digit shortcut for RED TEAM; require a deliberate act (section 5).
- **WARDRIVE [seen, shots d10, d11]:** Enter or Space toggles start and end with no confirmation; I woke the screen with Space, pressed Space once more, and a 5 minute drive was ENDED. Esc, Del and the BACK button also end it at once (`apps/app_rf/rf_drive.c:246`, `250-263`), as does any Alt shortcut to another app. Change: starting is one press; ending asks "END THE DRIVE? 00:05:17, 1112 ROWS" with CANCEL focused; leaving the screen does not end the drive (the driver already owns it), it shows as a status bar flag "DRIVE 00:05" like RED TEAM does.
- **Site survey [code]:** leaving the tool or switching to BURSTS during the minute discards the survey without a word (`apps/app_rf/rf_bands.c:386`, `487-494`); S during a survey is ignored silently (`rf_bands.c:541-544`); there is no cancel.
- **BURSTS [seen, shots 60, 62, 64]:** one arrow press retunes and wipes the burst list and the learned rhythms (15 bursts and an "every 7.0 s" emitter gone); switching to SPECTRUM and back does the same. Change: keep the history per frequency, or ask before clearing.
- **Honeypot [seen, shots 37, 39]:** Enter starts transmitting immediately. Given the gate this is defensible, but then the on-air state must be unmissable (F8).

### F7. Touch: you can look but not operate, and the targets are too small
[code, confirmed by the absence of any control in the screenshots] Tappable things are: gate tiles, tool tiles, the WI-FI and SUB-GHZ tabs, Bluetooth rows, log scrolling, BACK. Not reachable by touch: start or end a drive, site survey, learn, capture, Bluetooth filter and mark as mine, honeypot name, channel and start, channel selection in any chart, WI-FI network selection (rows are not clickable, `apps/app_rf/rf_wifi.c:457-466`, unlike `apps/app_rf/rf_ble.c:297-298`), burst tuning, log reload. With the keyboard detached (the status bar has a KBD indicator for exactly that) RF WATCH is read-only.

Sizes of what is tappable:

| Target | Size | In mm |
|---|---|---|
| BACK (status bar) | about 130x44 px (`components/deck_core/deck_statusbar.c:101`) | 11 x 3.8 |
| Tabs (WI-FI, SUB-GHZ) | 18 px text plus 10 px each side (`rf_wifi.c:494-496`) | about 3.3 tall |
| Bluetooth rows | 32 px pitch, no gap (`rf_ble.c:296`) | 2.8 tall |
| Dialog buttons | 52 px min (`components/deck_ui/deck_widgets.c:331`) | 4.5 tall |
| Warning count | about 65x44 px | 5.6 x 3.8 |
| Tool tiles | 290x250 | 25 x 22, fine |

Only the tiles reach 9 mm. In WARDRIVE, the tool meant for walking and a car, the only control is a key on a thumb keyboard.

Change: replace the bottom hint line with an action bar: a row of 64 px tall (5.5 mm, the most the layout can spare; 80 px if the hint text goes) buttons that are both the touch control and the key hint: `[S] SURVEY`, `[B] RELEARN`, `[C] CAPTURE`, `[F] FILTER: ALL`. Tabs become full buttons of the same height. Rows get 48 px pitch. Charts take a tap or drag to move the cursor. WARDRIVE gets one large START / END button (at least 200x100 px).

### F8. You cannot tell where you are or what mode you are in
- The status bar says "APPS // RF WATCH" at every level: gate, tools page and every tool [seen, all shots]. OVERVIEW never shows the word OVERVIEW; 2.4 GHZ is headed "2.4 GHZ BAND NRF24"; SUB-GHZ is headed "433 MHZ BAND CC1101" while its tile says SUB-GHZ. The BACK button says BACK whether it goes to the tools page or out of RF WATCH (and ends the session).
- SAFE is shown only on the tools page chip and one 16 px line in OVERVIEW. RED TEAM has a red status bar flag, which is good.
- **RED TEAM ends silently when the screen sleeps [seen, shots f2, f4]:** I started the honeypot, let the saver come up, woke the deck. The honeypot screen now says "RED TEAM ONLY ... LEAVE RF WATCH AND REOPEN IT IN RED TEAM", the chip says SAFE, the flag is gone, and nothing says what happened (`apps/app_rf/app_rf.c:424-426`). The behaviour is right; the silence is not.
- **Honeypot ON AIR [seen, shot 39]:** the single transmitting state in the product is the words "ON AIR" in 16 px red, the lowest-contrast state colour, same size as the lines around it. The brightest line in that panel is "RADIO: HONEYPOT ON AIR" only because line 5 is styled as a headline for other views (`apps/app_rf/rf_wifi.c:424`, `352`); in the OFF state that same bright line reads "RADIO: HOPPING 38 CHANNELS, BLUETOOTH WAITING", the least important text on the screen [seen, shot 37]. The hint still offers [N] NAME and [K] CHANNEL while on air, where both are ignored silently (`rf_wifi.c:536-548`) [seen, shot 40].

Change: status bar path "RF WATCH // WI-FI // NETWORKS"; BACK labelled with its destination ("TOOLS", "LEAVE"); a mode chip (SAFE green, RED TEAM red) in the same place on every tool; on wake after a forced disarm, a dialog: "RED TEAM ENDED WHEN THE SCREEN SLEPT. HONEYPOT OFF AIR. 1 JOIN LOGGED." Honeypot on air: a 36 px red "ON AIR" with the name and channel at 24 px beside it, a red frame on the panel, and the status bar flag changed to "ON AIR".

### F9. OVERVIEW is a bring-up page, not an overview
[seen, shots 06, 34, 79] Six panels, about 40 values, nearly all 16 px. It gives equal weight to "FRAMES 410 BAD 0", "GDO0 0 GDO2 1 NRF IRQ IDLE", "HEAP 204K", "PROTO 2" (`apps/app_rf/rf_overview.c:125-142`) and to "3 TRACKERS". Its keys are [H] HELLO, [P] PING, [T] TEST THE TX GATE (`rf_overview.c:166`), which are protocol debugging; T in RED TEAM produces the event "node no such test" [seen, shot 34]. It duplicates smaller, worse versions of the two spectrum charts and the Wi-Fi list. It does not answer "is anything wrong".
Change: split it. The glanceable part becomes the tools page itself (section 5). The link, pins, heap, frame counters and H/P/T keys move to a BOARD page for when the hardware misbehaves.

### F10. Language
- **One device, five names:** "THE C5" (`apps/app_rf/app_rf.c:247`, `rf_wifi.c:93`, `rf_ble.c:96`, `rf_overview.c:121`), "THE NODE" (`rf_drive.c:47`, `rf_bands.c:175`), "THE BOARD" (`rf_drive.c:46`), "SENTINEL" (home tile), "RF WATCH". Pick "RADIO BOARD" for the hardware and use it everywhere.
- **Chip names as labels:** CC1101, NRF24, GDO0, EXT5V, RX_EN. Use what they do: 433 MHZ RADIO, 2.4 GHZ RADIO.
- **Private units:** "4 OF 8", "SOLID 10", "(8/8)", "REPORTS 24", "CARRIER IN 100 % OF 23 REPORTS", "LEASE 4.2 S" [seen, shots 71, 34]. "OF 8" is hits in 8 sweeps; show it as a percentage ("BUSY 50 %"). "LEASE 4.2 S" counting down beside "NODE ARMED" looks like a fuse; say "ARMED" and keep the lease for the BOARD page.
- **Roadmap words on user screens:** the TESTS tile is tagged "PHASE 6" in warning yellow (`app_rf.c:58`); pressing 8 or tapping it only moves the selection and says nothing [seen, shot 32]. Say "NOT BUILT YET" on the tile, or hide it.
- **Labels that do not say what the key does:** "[S] A NAME, A MINUTE, THEN A FILE" (`rf_bands.c:185`); "[T] TEST THE TX GATE"; "[ENTER] START OR STOP" (say which, from state: "[ENTER] GO ON AIR" / "[ENTER] TAKE OFF AIR"); "[ENTER] START OR END THE DRIVE" likewise; "[K] CHANNEL" for the honeypot because C is capture elsewhere.
- **Standing paragraphs in all caps, dim, 16 px:** the honeypot "WHAT IT IS FOR" panel (a third of the screen, `rf_wifi.c:444-450`), "BUSY: 8 DB OVER THE CHANNEL'S OWN RUNNING LEVEL..." (`rf_bands.c:235`), the burst shape glossary (`rf_bands.c:422`), the wardrive rules (`rf_drive.c:232`), "SATURATED MEANS 40 CHANNELS SOLID: BUSY WI-FI DOES NOT COME NEAR" (`rf_bands.c:261`). They are true and well written, and they are documentation. Move them behind a HELP key or button per tool (a dialog, mixed case, 24 px) and give the space to bigger live values.
- **Mixed registers:** screens are capitals, events are lower case sentences ("likely jamming: 2.4 GHz saturated, BLE 29 to 3 adv/s"), which are in fact easier to read; "ADV/S" on one screen and "ADVERTS/S" on another; "BLE" and "BLUETOOTH"; "GNSS" and "GPS" on the same page.
- **Two different surveys:** SITE SURVEY (`site-<time>.csv`, S key) and the wardrive's "SURVEY ROWS" (`survey-<time>.csv`). Call the second "BAND ROWS" or "SPECTRUM TRACK".
- **"NAME CLASHES"** in NETWORKS means suspected evil twin; the selected-row text says "MAY BE AN IMPOSTOR" and the event says "evil twin?". One term.

### F11. Colour carrying meaning alone, and colour collisions
- **accent2 magenta (#FF2A6D) is indistinguishable from danger red (#FF3B3B)** at 1.02:1 between them. It is used for neutral things: the source column in LOG (`apps/app_rf/rf_log.c:93`), the CH column and peak-hold bars in OVERVIEW (`rf_overview.c:349`, `363`), peak marks in the band charts (`rf_bands.c:129`). In LOG the reddest thing on every row is the word "sub" [seen, shots 80, e1]. Use a neutral (text or a blue-grey) for these.
- **Accent themes collide with state colours [seen, shots i2 and i3]:** NOMAD's accent is the warn yellow exactly, MILITECH's accent is the ok green exactly and its accent2 is the warn yellow (`components/deck_ui/deck_theme.c:16-21`). Under NOMAD a harmless FINDMY row is the same yellow as an AIRTAG tracker row and "CAPTURE ON" looks like a warning; under MILITECH "recognised, harmless" and "MINE" are the same green. State colours must not be reused as accents: either RF WATCH ignores the accent for data, or the themes get accents outside the yellow, green, red set.
- **Yellow as annotation:** the Bluetooth advertising channel marks in 2.4 GHZ are warn yellow (`rf_bands.c:119`); "recently heard" drones are yellow, stale ones dim (`rf_wifi.c:309`).
- **Green as "best":** quietest Wi-Fi channel in green (`rf_bands.c:274`). Harmless, but it is the "ok" colour doing a second job.
- **Red-green colour blindness:** simulated, ok green and warn yellow become nearly the same yellow, and danger red becomes a dark olive (3.7:1). Places where the level is colour only: LOG and EVENTS rows, warn against info (`rf_log.c:91`, `rf_overview.c:277`); the four chip outlines in OVERVIEW, green ok, red failed, grey unknown (`rf_overview.c:130-134`); the deauth cell in the Wi-Fi waterfall (`rf_wifi.c:124`); JAM WATCH line colour (words do back this one up). Add a glyph or word for each: a level column in LOG ("!" and "!!"), a tick or cross in the chips, a marker above a deauth column plus a text line that persists.
- Places that get it right: Bluetooth rows carry the label word as well as the colour; band state words (NORMAL, BAND UP, SATURATED) are words first.

### F12. Selection is nearly invisible
The selected row and selected chart column are drawn as #171A30 on #0D0F1C, 1.11:1 (`rf_wifi.c:461`, `rf_ble.c:293`, `rf_bands.c:126`). [seen, shots 15, 51, 13] In lists it is a faint band; in the charts the cursor is a dark column you find by hunting, and the selected frequency is not marked on the axis in the band charts. On the gate and the tools page the selection is only an outline changing from teal or green or red to cyan [seen, shots 01, 02]; on the gate the unselected SAFE tile still looks more "lit" than the selected RED TEAM tile because of its green wash.
Change: selected row gets an accent bar on the left edge, a 2 px accent outline and brighter text; chart cursor is a full-height accent line with its frequency label on the axis; selected tile gets a thick outline and a filled tab.

### F13. Offline and not-ready states are inconsistent
- WARDRIVE does it properly: "CANNOT START" at 36 px with the reason under it [seen, shot c7] (the reason is 16 px dim and should be 24 px).
- BLUETOOTH with no link says "LISTENING..." in the list while a dim 16 px line fifth down says "WAITING FOR THE C5" [seen, shot c5] (`rf_ble.c:189`, `96`).
- WI-FI with no link is an empty chart and the same dim line [seen, shot c2].
- The tools page with no link shows eight normal tiles and "WAITING FOR THE C5" in dim 16 px [seen, shot c1 chooser]; "BOARD OFF" is styled the same way (`app_rf.c:246-247`).
Change: one pattern everywhere: when the board is off or not linked, the tool's main area is replaced by a centred 36 px state ("NO LINK TO THE RADIO BOARD") and one line on what to do; tiles on the tools page are dimmed with the reason.

### F14. Lists
- **Bluetooth re-sorts by signal twice a second** [seen, shots 92 against 93: TILE, AXON and the Govee row change places within two seconds]. Rows move under the eye and under the thumb. And strongest-first is the wrong order for a safeguarding tool. Change: surveillance and trackers pinned to the top, then the rest by signal with hysteresis (re-rank at most every 5 s, or only when the difference exceeds 6 dB).
- **The filter state is hidden** in a dim 16 px line in the other panel ("SHOWING SURVEILLANCE :: 2 OF 13", `rf_ble.c:118`) while the list heading still reads "DEVICES STRONGEST FIRST" [seen, shot 95]. Put it in the list heading as a chip: "SURVEILLANCE ONLY, 2 OF 13".
- **WI-FI NETWORKS** has no filter, no touch selection, no "this one is mine" (BLUETOOTH has all three), so the user's own guest network is permanently yellow.
- **LOG** has no filter for warnings only, cannot show any day but today, and shows the level by colour alone. Given F3, "warnings only" is the view the user actually wants first. R for reload is unnecessary: it reloads itself at the top.

### F15. Feedback for running operations
- **Site survey running** is a dim 16 px line, `SITE SURVEY "kitchen" 59 S LEFT` [seen, shot 56]. Finished, the file name wraps mid-word in the same dim style [seen, shot 58]. A minute-long measurement deserves a progress bar across the chart panel and a 24 px result line ("SAVED: site-...-kitchen.csv").
- **Learn** feedback replaces the survey line for four seconds in dim grey [seen, shot 53] (`rf_bands.c:174-175`).
- **Capture** is a 16 px line that turns accent colour [seen, shots 13, 96]; nothing says where the file went or what it is called, on start or on stop.
- **Honeypot in SAFE:** Enter does nothing; the reason is on screen but small; the HONEYPOT tab looks as available as the others [seen, shot 19]. Mark the tab as locked in SAFE.

### F16. Smaller defects seen while testing
- The display font's zero has a slash, so a big "0" count renders as a box with a diagonal, which reads as a "blocked" symbol: "0 JOINS", "0 SATS USED", "0 WI-FI ACCESS POINTS" [seen, shots 19, c1 tool, c7]. Use an unslashed zero in the display face for counts.
- "NOTHING HAS REPEATED YET" inherits the colour of the last emitter row, so it is sometimes cyan and sometimes grey (`rf_bands.c:320`) [seen, shots 62 against 64].
- A drive reopened after Esc shows ENDED with "00:00:00" beside a real distance, because the duration lives in the screen (`rf_drive.c:99-100`) [seen, shot d8].
- WARDRIVE panels jump 18 px up and down as the explanation line changes between one and two lines [seen, shots d4 against d5].
- "1 JOINS" [seen, shot 39]; "(8/8)" wraps to its own line in 2.4 GHZ [seen, shot 71].
- LOG's summary line starts with the full storage path and wraps (`rf_log.c:64`) [seen, shot e0; on the device the path is shorter].
- The waterfalls are mostly empty for the first minute or two, and the 433 MHz waterfall paints the noise floor as a solid teal block so that a real signal is one slightly brighter cell [seen, shots 11, 51, 58]. Map the floor to the panel colour.
- Alt+H, the shortcut list, has no RF WATCH section (`components/deck_core/deck_shell.c:204-244`) [code].

---

## 3. Cross-cutting inconsistencies

### Keys per screen (from each `key()` handler and hint line)

| Key | Gate | Tools page | OVERVIEW | WI-FI | SUB-GHZ | 2.4 GHZ | BLUETOOTH | WARDRIVE | LOG |
|---|---|---|---|---|---|---|---|---|---|
| digits | 1 SAFE, **2 RED TEAM** | 1 to 8 open tool | none | 1 to 4 view | 1 to 2 view | none | none | none | none |
| Enter | start selected | open | none | honeypot **on or off air** | BURSTS: reset frequency | none | none | **start or end drive** | none |
| Space | same as Enter | same as Enter | none | none | none | none | none | same as Enter | none |
| Tab | other tile | next tile | none | none (does not switch tabs) | none | none | none | none | none |
| Left, Right | choose | choose | none | MONITOR: channel | channel, or BURSTS: retune (clears list) | channel | none | none | none |
| Up, Down | none | choose | none | NETWORKS: select | none | none | select | none | scroll |
| C | | | | capture | | | capture | | |
| K | | | | honeypot channel | | | | | |
| N | | | | honeypot name | | | | | |
| S | | | | | site survey | site survey | | | |
| B | | | | | relearn baseline | relearn baseline | | | |
| F, M | | | | | | | filter, mine | | |
| R | | | | | | | | | reload |
| H, P, T | | | hello, ping, probe | | | | | | |
| Esc, Del, BACK | leave | leave, ends session | tools | tools | tools (drops survey) | tools (drops survey) | tools (ends capture) | tools (**ends drive**) | tools |

Real problems this shows:
- **The same digit means three things in three consecutive screens** (mode, tool, view), which is what makes the RED TEAM type-ahead slip possible, and digits cannot jump between tools from inside a tool.
- **Enter is the "do the risky thing" key in two tools, a harmless reset in a third, and nothing elsewhere.** Space mirrors Enter in three screens and not in the fourth.
- **Things drawn as tabs do not respond to Tab.**
- **Esc is always "tools" but its side effects differ** (nothing, ends capture, ends drive, drops survey, takes honeypot off air) and only WARDRIVE's hint says so.
- **"Channel" is on K in one place** because C was taken.
- **Lists differ:** BLUETOOTH has select by key and touch, filter, mark as mine; NETWORKS has select by key only; DRONES and BURSTS have no selection at all.

### Other inconsistencies
- Hint line wording for the same key: "[ESC] BACK" (gate), "[ESC] LEAVE RF WATCH (ENDS THE SESSION)" (tools page), "[ESC] TOOLS" (tools), and the on-screen button always says BACK.
- The hint line also carries colour legends and disclaimers ("RED: DEAUTH FRAMES HEARD", "YELLOW MARKS: BLUETOOTH ADVERTISING", "A LABEL IS A MATCH, NOT PROOF", "A SHAPE IS A GUESS, NOT A DECODE", "THE SCREEN CAN SLEEP: THE DRIVE GOES ON"), so it runs to 100 or more characters of dim 16 px text and is the first thing to become unreadable. Legends belong beside the thing they explain.
- Headline state: WARDRIVE 36 px, bands 24 px, WI-FI and BLUETOOTH none (a count instead), OVERVIEW the link word.
- Tabs exist in WI-FI and SUB-GHZ, not in 2.4 GHZ, which is the same tool with one view.
- "SAFE / RED TEAM" uses green and red; the same red is then "surveillance device", "name clash", "ON AIR", "SATURATED", "ROWS DID NOT REACH THE CARD" and (via accent2) "the log's source column".

---

## 4. What works and should be kept

- **The safety model itself:** Enter straight through always lands on SAFE; RED TEAM has a persistent red flag in the status bar; leaving, sleeping or switching off disarms; leaving the honeypot view takes it off air. All verified [seen, shots 31, 43, f4].
- **WARDRIVE's layout:** one 36 px state word with colour and a plain reason, two big counts, "NOTHING LOST ON THE WAY TO THE CARD", the not-written counts that show each privacy rule at work, and the drive surviving the saver [seen, shots d2, d4, d10]. This is the pattern every tool should follow.
- **Band state words** (NORMAL, BAND UP n DB, SATURATED n CH) as words plus colour, and the JAM WATCH line on both band screens.
- **BLUETOOTH's list**: label word plus colour, the SELECTED panel's plain sentences ("ITS OWNER IS NEAR IT: NOT A TRACKING RISK", "WITH YOU 12 MIN, YOU MOVED 300 M"), MARK AS MINE with its undo stated, selection following the device by address.
- **NETWORKS' explanation of an open twin** in plain words [seen, shot h4], and "QUIETEST OF THE THREE: CHANNEL 11" as a direct answer rather than a chart to interpret.
- **BURSTS**: readable 20 px table, EMITTERS with "EVERY 7.0 S", the honest "A SHAPE IS A GUESS".
- **Honest empty states** ("NOTHING HEARD YET", "NO REMOTE ID HEARD", "A DRONE THAT BROADCASTS NOTHING IS NOT SEEN").
- **Back is uniform:** Esc, Del and the BACK button all go tool to tools page to out, and the tools page remembers the last tool [seen, shots 42, 43].
- **The banner** itself is legible (20 px, icon, source, text) and does not steal the keyboard; the waking key does nothing else.
- **Prompts** are 28 px, state their rules in the title ("ENTER FOR NONE, ESC CANCELS"), and get an on-screen keyboard when the physical one is absent.
- **LOG** rows are mixed case at 20 px with position; reading older rows is not yanked away by new ones.

---

## 5. Structural changes I would make

### 5.1 Drop the gate; make RED TEAM a deliberate act from inside
The gate costs every session a step and a decision for a mode that is used rarely, it is where the type-ahead arming happens, it is where a warning tap lands, and it forces "LEAVE RF WATCH AND REOPEN IT IN RED TEAM" onto the one screen that needs it. Instead:
- RF WATCH always opens SAFE, straight to the status page below.
- RED TEAM is entered from the TESTS tile (which becomes the home of everything that transmits: the honeypot moves there from WI-FI). Entering shows the permission sentence at readable size with CANCEL focused and a one-second hold-off on Enter (the shell's `deck_modal_confirm_danger` already does this). Leaving TESTS disarms. The red status bar flag stays as it is.
- Result: no dead HONEYPOT tab in SAFE sessions, no mode to remember, and nothing that transmits lives among tools that only listen.

### 5.2 Merge the tools page and OVERVIEW into one status page
```
[< LEAVE]  RF WATCH                         13:02   [SAFE]  KBD SD NET 78%
+--------------------------------------------------------------------------+
| !! LIKELY JAMMING  2.4 GHZ SATURATED, BLUETOOTH DOWN 32 TO 2   40 S AGO  |   <- alert strip, 28 px,
+--------------------------------------------------------------------------+      "ALL CLEAR" when quiet
+-----------------+ +-----------------+ +-----------------+ +--------------+
| 1 WI-FI         | | 2 BLUETOOTH     | | 3 SPECTRUM   !! | | 4 WARDRIVE   |
| 23 NETWORKS     | | 13 DEVICES      | | 433 NORMAL      | | READY        |
| ! 1 OPEN TWIN   | | ! 2 SURVEILLANCE| | 2.4 SATURATED   | | GPS FIX 9    |
+-----------------+ +-----------------+ +-----------------+ +--------------+
+-----------------+ +-----------------+ +-----------------+ +--------------+
| 5 LOG           | | 6 TESTS         | | 7 BOARD         | |  (free)      |
| 4 WARNINGS TODAY| | RED TEAM ONLY   | | ONLINE, GPS FIX |                |
+-----------------+ +-----------------+ +-----------------+
```
Each tile shows its tool's live headline at 24 px and a badge when it has a warning, so the page is both the menu and the overview. The alert strip is the same component on every tool screen.

### 5.3 Regroup the tools
- **SPECTRUM** replaces SUB-GHZ and 2.4 GHZ: tabs 433 MHZ, 433 BURSTS, 2.4 GHZ. They already share one source file, one chart, the same keys, the survey and the jam watch.
- **WI-FI**: MONITOR, NETWORKS, DRONES. Honeypot leaves.
- **TESTS**: the RED TEAM area: HONEYPOT now, the targeted tests later.
- **BOARD**: link, firmware, radios present, pins, heap, frame counters, HELLO, PING, probe. What OVERVIEW's LINK panel is today.
- **LOG**: add WARNINGS ONLY as the default view and a day picker.

### 5.4 One frame for every tool
```
[< TOOLS]  RF WATCH // SPECTRUM // 2.4 GHZ            [SAFE]  ...
[ alert strip ]
[ 433 MHZ ][ 433 BURSTS ][ 2.4 GHZ ]                      tabs as 64 px buttons, Tab key cycles
+--------------------------+ +-------------------------------------------+
| SATURATED  (36 px)       | |  chart or list, rows 48 px, 24 px text     |
| 101 of 126 channels busy | |                                           |
| selected: 2460 MHZ ...   | |                                           |
+--------------------------+ +-------------------------------------------+
[ S  SITE SURVEY ] [ B  RELEARN ] [ ?  HELP ]             action bar: touch buttons that are the key hints
```
Rules that go with it: one state word per screen at 36 px, word plus colour plus glyph; nothing under 20 px; explanations behind HELP; destructive or long-running actions confirm with what will be lost; the same key means the same thing everywhere (Tab cycles views, Enter is the primary action named on its button, digits jump tools from anywhere via the tools page only).

### 5.5 If only three things get done
1. The alert strip with three severity levels, inside RF WATCH and on the saver (F1 to F4).
2. Type scale and dim colour (F5), paid for by moving the paragraphs behind HELP (F10).
3. Confirmations on B, on ending a drive, and removal of the bare "2" for RED TEAM (F6).

---

## 6. Coverage and limits

Exercised in the simulator: gate (both selections, Enter, digit, tap, Esc), tools page (linked, not linked, RED TEAM, during jamming, TESTS pressed), OVERVIEW (not linked, normal, RED TEAM, after T, during jamming, two other accent themes), WI-FI (all four views, channel cursor, capture, row selection, deauth and evil twin scenes, honeypot in SAFE, off, on air, name prompt, after a forced disarm), SUB-GHZ (spectrum, learn, survey prompt, running and written, bursts, retune, tab by touch, during jamming), 2.4 GHZ (normal, jam, B during jam, after), BLUETOOTH (list, selection by key and touch, mine, both filters, capture, not linked), WARDRIVE (cannot start, waiting for a fix, recording, ended, across the saver, ended by a second Space, reopened), LOG (normal, scrolled, during jamming), warnings as seen from the home screen (banner, count, tap on count, saver), back by Esc, Del and BACK tap.

Not verified: real panel brightness, glare and viewing angle (contrast figures are computed from the palette, not measured); the physical keyboard's layout (if digits or arrows need a modifier on it, every digit shortcut costs two presses); "BOARD OFF" (the simulator always powers the board); SD card missing; POSSIBLE (yellow) jam level; firmware-too-old messages; hardware behaviour of B during real jamming (simulator re-alarms after a few seconds). The scanline effect is faint in the renders and I did not count it against legibility.

Files reviewed (absolute): `/Users/brianmeyer/claude/tab5-cyberdeck/sentinel-useful-93ca39/apps/app_rf/app_rf.c`, `rf_overview.c`, `rf_wifi.c`, `rf_bands.c`, `rf_ble.c`, `rf_drive.c`, `rf_log.c`, `rf_ui.h` in the same directory; `/Users/brianmeyer/claude/tab5-cyberdeck/sentinel-useful-93ca39/components/deck_core/deck_shell.c`, `deck_statusbar.c`, `deck_notice.c`, `deck_modal.c`, `deck_saver.c`, `include/deck_notice.h`; `/Users/brianmeyer/claude/tab5-cyberdeck/sentinel-useful-93ca39/components/deck_ui/deck_theme.c`, `deck_widgets.c`; `/Users/brianmeyer/claude/tab5-cyberdeck/sentinel-useful-93ca39/README.md` (RF WATCH sections).

Side effects: the simulator wrote survey, capture, drive and event files under `sim/sdcard/sentinel/` (git-ignored, as the brief allowed). One stray windowed simulator started by a `--help` attempt was killed. The intermediate `.ppm` files were removed from the scratch directory; the PNGs remain (14 MB).

---

## 7. Screenshots produced

All 1280x720 PNG. "(not inspected)" marks frames rendered in the same runs that I did not open; findings rest only on inspected ones.

Gate, tools page, OVERVIEW
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/01_gate.png : gate, SAFE selected
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/02_gate_red_selected.png : gate, RED TEAM selected (outline only)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/03_chooser_early_nolink.png : tools page at 2.5 s, slow link
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/04_chooser_linked.png : tools page, SAFE, linked
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/05_overview_early.png : OVERVIEW at 9 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/06_overview_40s.png : OVERVIEW, normal, yellow lines on a quiet day

WI-FI, SAFE
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/10_wifi_monitor_first.png : MONITOR first view (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/11_wifi_monitor_30s.png : MONITOR at 30 s, half-empty waterfall
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/12_wifi_monitor_ch_moved.png : MONITOR, cursor moved (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/13_wifi_monitor_capture.png : MONITOR, cursor on channel 9, capture on
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/14_wifi_networks.png : NETWORKS (not inspected at full size; source of zoom z1)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/15_wifi_networks_sel.png : NETWORKS, fifth row selected, faint highlight
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/16_wifi_drones.png : DRONES, empty (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/17_wifi_drones_90s.png : DRONES, one drone
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/18_wifi_honeypot_safe.png : HONEYPOT in SAFE (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/19_wifi_honeypot_safe_enter.png : HONEYPOT in SAFE after Enter, RED TEAM ONLY
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/20_wifi_honeypot_name_prompt.png : honeypot name prompt
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/21_wifi_after_prompt_esc.png : after Esc in the prompt (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/22_back_to_chooser_from_wifi.png : Esc from WI-FI (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/h0_wifi_monitor_62s.png : MONITOR at 62 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/h1_wifi_monitor_66s.png : MONITOR at 66 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/h2_wifi_monitor_75s.png : MONITOR after a deauth flood, one red cell
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/h3_wifi_networks_twin.png : NETWORKS with the open twin (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/h4_wifi_networks_twin_sel.png : NETWORKS, twin selected, impostor text
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/h5_wifi_monitor_200s.png : MONITOR at 200 s (not inspected)

RED TEAM
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/30_red_chooser_early.png : tools page, RED TEAM, early (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/31_red_chooser.png : tools page, RED TEAM, armed, status bar flag
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/32_red_chooser_tests_pressed.png : key 8 on TESTS: selection only
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/33_red_overview.png : OVERVIEW in RED TEAM (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/34_red_overview_T.png : OVERVIEW in RED TEAM after T, "no such test", lease countdown
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/35_red_after_del.png : after Del from a tool (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/36_red_honeypot_off.png : honeypot off (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/37_red_honeypot_ch.png : honeypot off, channel changed to 36
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/38_red_honeypot_starting.png : honeypot just started (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/39_red_honeypot_on_60s.png : honeypot ON AIR, one join
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/40_red_honeypot_on_N.png : N pressed while on air: nothing happens
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/41_red_monitor_after_honeypot.png : MONITOR after leaving the honeypot view (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/42_red_tap_back.png : tap on BACK from a tool: tools page
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/43_red_tap_back_again.png : tap on BACK again: APPS, flag gone
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/f0_red_honeypot_on.png : honeypot on before the saver (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/f1_red_saver.png : saver during RED TEAM (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/f2_red_after_wake.png : after wake: honeypot screen says RED TEAM ONLY, no flag
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/f3_red_after_wake_later.png : same, later (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/f4_chooser_after_sleep.png : tools page after wake: chip silently SAFE
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/f5_overview_after_sleep.png : OVERVIEW after wake (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g0_typeahead_redteam.png : Alt+X, Enter, 2 typed fast: RED TEAM armed
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g1_after_esc.png : Esc after that (not inspected)

SUB-GHZ
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/50_sub_first.png : SPECTRUM first view (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/51_sub_40s.png : SPECTRUM, normal
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/52_sub_sel_moved.png : cursor moved (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/53_sub_learn.png : after B: learning message
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/54_sub_after_learn.png : four seconds later (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/55_sub_survey_prompt.png : site name prompt
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/56_sub_survey_running.png : survey running, 59 s left
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/57_sub_survey_S_again.png : S during a survey (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/58_sub_survey_done.png : survey written, wrapped file name
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/59_bursts_first.png : BURSTS first view (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/60_bursts_later.png : BURSTS with 15 bursts and two emitters
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/61_bursts_tuned.png : BURSTS after retune (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/62_bursts_S_pressed.png : BURSTS after retune: list wiped, cyan empty-state text
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/63_sub_back_spectrum.png : back to SPECTRUM (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/64_sub_tap_tab2.png : BURSTS reached by tapping the tab, list empty again
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/78_sub_during_jam.png : SUB-GHZ during 2.4 GHz jamming: big green NORMAL, small red line

2.4 GHZ and the jam scene (SIM_RF_JAM=60)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/70_nrf_first.png : first view (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/71_nrf_50s.png : normal
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/72_nrf_sel_moved.png : cursor moved (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/73_nrf_jam_70s.png : jam at 70 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/74_nrf_jam_100s.png : SATURATED 101 CH, JAM WATCH LIKELY
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/75_nrf_jam_B.png : one second after B: green NORMAL, NONE SEEN, band still solid
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/76_nrf_jam_after_B_150s.png : alarm back at 150 s
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/77_chooser_during_jam.png : tools page during jamming: nothing shown
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/79_overview_during_jam.png : OVERVIEW during jamming: green ONLINE
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/80_log_during_jam.png : LOG during jamming: every warning the same yellow, source column in red-like magenta

BLUETOOTH
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/90_ble_first.png : first view (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/91_ble_30s.png : list at 30 s (source of zoom z0)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/92_ble_sel3.png : fourth row selected
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/93_ble_marked_mine.png : marked as mine; rows have reordered
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/94_ble_filter_trackers.png : TRACKERS filter (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/95_ble_filter_surv.png : SURVEILLANCE filter, state shown only in a dim line
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/96_ble_capture.png : capture on (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/97_ble_tap_row.png : row selected by tap, capture on
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/98_ble_700s.png : saver over BLUETOOTH at 700 s: clock only
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/99_ble_back.png : after waking and Esc (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/z0_ble_left_zoom.png : 3x zoom of the left panel, to judge 16 px text
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/z1_net_sel_zoom.png : 2x zoom of two NETWORKS rows

Warnings seen from outside RF WATCH (SIM_RF_JAM=60, then 20)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a0_home.png : home at 3 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a1_home_12s.png : home at 12 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a2_home_14s.png : home, banner "Axon device nearby", count 1
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a3_home_40s.png : home at 40 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a4_home_62s.png : home at 62 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a5_home_64s.png : home, banner "2.4 GHz band saturated", tile says SENTINEL ONLINE in green
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a6_home_66s.png : home at 66 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a7_home_68s.png : home at 68 s: no banner, count 7, "likely jamming" never bannered
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a8_home_80s.png : home at 80 s: next banner is a drone, count 8
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a9_after_tap_alert.png : my tap missed the count (landed on KBD); shows the same home state
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a10_chooser_after_alert.png : from the missed-tap run (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a11_home_after.png : from the missed-tap run (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/a12_saver_during_jam.png : saver during jamming: clock and date only
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/b0_home_30s.png : home before tapping the count (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/b1_after_tap_alert.png : after tapping the count: the mode gate, count gone
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/b2_chooser_after_alert.png : tools page after that (not inspected)

Not-linked states (tool opened 1.2 s after start)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c1_chooser_1150.png : tools page, "WAITING FOR THE C5"
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c1_tool_1700.png : OVERVIEW, NO LINK
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c1_tool_2400.png : OVERVIEW just after linking (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c2_chooser_1150.png : duplicate of the tools page (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c2_tool_1700.png : WI-FI MONITOR, no link
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c2_tool_2400.png : WI-FI just after linking (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c3_chooser_1150.png : duplicate of the tools page (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c3_tool_1700.png : SUB-GHZ, no link (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c3_tool_2400.png : SUB-GHZ just after linking (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c5_chooser_1150.png : duplicate of the tools page (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c5_tool_1700.png : BLUETOOTH, no link, list says LISTENING...
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c5_tool_2400.png : BLUETOOTH just after linking (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c7_chooser_1150.png : duplicate of the tools page (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c7_tool_1700.png : WARDRIVE, CANNOT START, no link
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/c7_tool_2400.png : WARDRIVE just after linking (not inspected)

WARDRIVE
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d0_drive_ready_nofix.png : READY, no fix (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d1_drive_starting.png : STARTING (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d2_drive_waiting_fix.png : WAITING FOR A FIX
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d3_drive_recording_20s.png : RECORDING at 20 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d4_drive_recording_120s.png : RECORDING at two minutes
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d5_drive_ended.png : ENDED; panels shifted down 18 px
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d6_drive_second.png : second drive recording (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d7_drive_esc_while_recording.png : Esc while recording (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d8_drive_reopened.png : reopened after Esc: ENDED with 00:00:00
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d9_drive_after_saver.png : saver during a drive (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d10_drive_wake.png : woken with Space: still RECORDING at 00:05:17
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/d11_drive_space_again.png : one more Space: ENDED, no confirmation

LOG
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/e0_log.png : LOG, top
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/e1_log_scrolled.png : LOG scrolled: "433.9 MHz active / quiet" flood
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/e2_log_scrolled_60s.png : still scrolled at 60 s (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/e3_log_reload.png : after R (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/e4_left_rfwatch.png : after Esc twice (not inspected)

Touch path and accent themes
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g2_gate_esc.png : Esc at the gate (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g3_gate_tap_red.png : tap on the RED TEAM tile (not inspected; the simulator log confirmed it armed)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g4_tap_tests.png : tap on TESTS (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g5_tap_wifi.png : tap on the WI-FI tile (not inspected; the simulator log confirmed it opened)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g6_tap_drones_tab.png : tap on the DRONES tab (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/g7.png : tap in the list area (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i2_ble_accent2.png : BLUETOOTH, NOMAD accent (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i2_ble_capture_accent2.png : BLUETOOTH, NOMAD accent: FINDMY same yellow as trackers, CAPTURE ON yellow
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i2_overview_accent2.png : OVERVIEW, NOMAD accent: headings and bars in warning yellow
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i2_nrf_accent2.png : 2.4 GHZ, NOMAD accent (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i3_ble_accent3.png : BLUETOOTH, MILITECH accent (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i3_ble_capture_accent3.png : BLUETOOTH, MILITECH accent: FINDMY and CAPTURE ON in the "ok" green
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i3_overview_accent3.png : OVERVIEW, MILITECH accent (not inspected)
- /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/i3_nrf_accent3.png : 2.4 GHZ, MILITECH accent (not inspected)

Also in that directory: /private/tmp/claude-501/-Users-brianmeyer-claude-tab5-cyberdeck-sentinel-useful-93ca39/d1b23c8e-409a-430a-acde-4982c4074fd0/scratchpad/rfreview/git_before.txt (the `git status --short` snapshot used to confirm the repository was not changed).