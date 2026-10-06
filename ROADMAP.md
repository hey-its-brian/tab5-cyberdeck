# Roadmap

Each version ends with something flashable and usable.

## v0.1: Shell and cyberpunk UI (done)

- [x] Tab5 bring-up via M5Stack's BSP (all panel revisions), landscape through PPA
- [x] Tab5 Keyboard in HID mode with hot-plug, routed to hotkeys, modules and LVGL
- [x] Boot POST, HUD status bar, launcher, glitch transitions, scanlines
- [x] SYSTEM module: brightness, accents, toggles, RTC clock, hardware readout
- [x] Desktop simulator with headless screenshots
- [ ] Verify on hardware: boot time under 3 s, touch and keyboard on every screen

## v0.2: Markdown (read, write, preview) (done)

- [x] OTA-ready partition table (two 6 MB app slots) so OTA (v0.5) can update without USB
- [x] md4c parser mapped to LVGL: headings, emphasis, lists, task lists, quotes, fenced code, tables, rules
- [x] File browser for `/sdcard/notes`: new, rename, delete (internal flash fallback without a card)
- [x] Monospace editor: autosave, `Ctrl+S`, save on exit, on-screen keyboard when no keyboard is attached
- [x] `Ctrl+P` toggles edit and preview; `Ctrl+T` side-by-side split with live preview
- [ ] Line numbers in the editor
- [x] Checkpoint on hardware: write a note on the keyboard, preview it, power-cycle, it is still there

## v0.3: Calculator (done)

- [x] Recursive-descent engine (doubles): `( ) ^ % !`, sin..round, pi/e/`ans`, DEG/RAD; 56 host unit tests
- [x] History tape with Up/Down recall; an operator on an empty line continues from `ans`
- [x] Programmer mode: hex/bin/oct literals and readout, `& | xor ~ << >>`, hex keys auto-prefix `0x`
- [x] Touch keypad (SCI/PROG pages) that hides while typing; Ctrl+K toggles
- [x] Checkpoint in the simulator: `(0xFF << 2) | 3` = 1023 and `2^10 / 3` = 341.333333333, from keyboard and touch
- [x] Checkpoint on hardware

## v0.4: Wi-Fi, clock sync and weather (done)

- [x] ESP32-C6 Wi-Fi over SDIO (ESP-Hosted 1.4.0 matched to the factory C6 firmware); verified on hardware
- [x] Wi-Fi setup in SYSTEM: scan, join, hidden networks, forget; credentials in NVS
- [x] SNTP clock sync written back to the RTC; 17 timezones applied before the RTC is read
- [x] HTTPS with the cert bundle; status bar NET indicator, boot POST and stats line go live
- [x] WEATHER module: Open-Meteo city search, conditions, 24 h trace, 7-day strip, offline cache, F/C
- [x] Checkpoint on hardware: join Wi-Fi, clock corrects itself, weather loads, survives a Wi-Fi drop

## v0.5: OTA updates (done)

- [x] A/B app slots (layout in place since v0.2)
- [x] Check GitHub Releases from SYSTEM (stable or beta channel), show notes, download, verify, swap slot
- [x] Rollback: a new build must confirm itself within 15 s, otherwise the bootloader reverts
- [x] Wrong-image guard (project name checked before writing); status bar UPD indicator, auto-check once per boot
- [x] Checkpoint on hardware: 0.5.0 updated itself to a test 0.5.1 over Wi-Fi (1.7 MB in about 10 s), and a deliberately crashing 0.5.2 rolled back on its own, twice
- [x] Fixed: screen strobing during the download (DSI interrupts masked while flash is written)
- [ ] Optional: push a build from the Mac over the local network during development

## Web flasher (GitHub Pages)

- [x] Page on GitHub Pages using ESP Web Tools (served from the site, no CDN): plug in, INSTALL in Chrome or Edge
- [x] Deploy workflow runs on every published release: bundles that release's full image and writes the manifest
- [x] `tools/release.sh` cuts a release (version bump, build, both images, tag, GitHub release)
- [ ] Checkpoint: install from the page onto the Tab5 in Chrome
- [ ] Optional: GitHub Actions builds the firmware on each tag

## v0.6: SSH terminal (done)

- [x] libssh2 (BSD) on ESP-IDF mbedTLS; wolfSSH ruled out (GPLv3 would make the firmware GPL)
- [x] libvterm (MIT) xterm-256color emulation, dirty-cell grid renderer, 2000-line PSRAM scrollback
- [x] Full key passthrough incl. `Esc`; `Alt+Esc` returns to the deck; touch bar for F-keys, PgUp/PgDn, Home/End, sticky Ctrl
- [x] Host profiles, known_hosts pinning with change warning, device-generated ECDSA P-256 key with the public key on screen
- [x] Checkpoint: full-screen apps verified in the simulator (top, vim) and SSH verified on hardware

## v0.6.1: Keyboard light and screen sleep (done)

- [x] SYSTEM > KBD LIGHT: OFF / LOW / HIGH (the keyboard default was too bright)
- [x] SYSTEM > KBD THEME COLOR: keyboard LEDs take the accent color and follow accent changes
- [x] Screen sleep: `Alt+0` or tap the status bar clock; backlight and keyboard light off, any key or touch wakes (that press is swallowed)
- [x] Checkpoint: verified on hardware (installed over the air)

## v0.7: File portal

- [x] PORTAL module (05): ENGAGE starts a web file manager on the LAN at `http://deck.local` (mDNS) and the IP, with a QR code for phones
- [x] Browse, upload (drag and drop), download, rename, delete, new folder; `/notes` and `/music` created on first start
- [x] One-time 6 digit PIN per session, lockout after 5 wrong tries, auto-off after 15 min idle, LINK in the status bar while up
- [x] Security review fixes: Host/Origin checks (DNS rebinding), stalled uploads dropped, failed logins don't keep it alive, safe file replace, `/ssh` (private key, known_hosts) never exposed
- [x] `tools/portal_mock.py` serves the real page from the Mac for development and screenshots
- [x] Checkpoint: verified on hardware (PIN login and lockout, upload, download, engage and disengage); the player side lands with v0.8

## v0.7.1: Security hardening

- [x] Settings (NVS) encrypted with an HMAC key the deck burns into a free eFuse key block on first boot; plain fallback, never blocks boot or flashing
- [x] SSH device key moved from the SD card into encrypted settings (same key, SD copy wiped)
- [x] Signed OTA updates (RSA-3072, no Secure Boot, no eFuses): signed builds only install updates signed with the same key; USB always works
- [x] CI: actions pinned to commit SHAs, esp-web-tools tarball integrity check, manifest built with jq, job-scoped permissions
- [x] Checkpoint: beta installs from v0.7.0, SECURITY shows ENCRYPTED, settings survive restarts
- [ ] Checkpoint: the next signed update installs over the air
- Known issue: the one-time move of existing settings into encrypted storage lost them on the test deck (Wi-Fi had to be re-entered). Only decks upgrading from v0.7.0 or earlier run it, so it was left as is

## v0.8: Audio player (MP3)

- ES8388 codec to the built-in speaker and the 3.5 mm headphone jack (auto-switch on plug)
- MP3 decode (esp-audio-codec / Helix) streamed from SD, gapless queue
- PLAYER module: library by folder, now playing, seek, volume, shuffle/repeat
- Keeps playing in the background while you use other modules; mini controls in the status bar
- Checkpoint: an hour of playback while using NOTES and SSH, no dropouts

### Bluetooth audio (deferred)

Audio goes to the built-in speaker and the 3.5 mm jack for now. The ESP32-C6
is Bluetooth LE only, so Bluetooth headphones would need external hardware
(a transmitter on the jack, a USB Bluetooth audio dongle, or a classic ESP32
add-on). Revisit after v1.0.

## Later: Space weather

- SPACE module (or a WEATHER tab): planetary Kp index, geomagnetic storm level (G1 to G5), solar wind speed and Bz, X-ray flux and flare class, aurora chance for your latitude
- Data from NOAA SWPC's free JSON feeds over HTTPS (no API key), refreshed every 15 min
- Alerts in the status bar when a storm or a strong flare is in progress
- Checkpoint: numbers match the SWPC dashboard; an alert shows during an active storm

## Later: RF watch (receive-only)

A passive monitoring suite that listens and alerts, never transmits.

- External radio modules on the expansion port: nRF24 (2.4 GHz), CC1101 (sub-GHz), RFID/NFC reader
- Activity view per band, baseline of what is normal around you, alerts on anything new or unusual
- Event log to SD, alert in the status bar
- Hardware decision first: which modules, and how they share the expansion port with the keyboard (I2C on G0/G1)

## Later: Wi-Fi presence radar

- Detect movement and people nearby from changes in Wi-Fi signals (channel state information)
- Open question: CSI may not be reachable through ESP-Hosted on the built-in C6; may need a dedicated ESP32 module
- Radar-style view: presence, motion intensity, history

## v1.0: Polish

- Battery stats, idle sleep (timed, building on the v0.6.1 screen sleep)
- More accents, cross-module links (open a note from SSH, send a result to Notes)
- 24 h soak test for leaks
