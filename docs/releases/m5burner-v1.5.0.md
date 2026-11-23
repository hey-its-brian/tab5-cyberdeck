# DECK//OS v1.5.0 on M5Burner

What to enter and upload when publishing (or updating) the DECK//OS firmware
in M5Burner. The same image the web flasher writes; nothing is built
specially for M5Burner.

## Listing fields

| Field | Value |
|---|---|
| Firmware name | DECK//OS |
| Device | M5Stack Tab5 (ESP32-P4) |
| Version | 1.5.0 |
| Category | Application (cyberdeck launcher) |
| Author | hey-its-brian |
| Source | https://github.com/hey-its-brian/tab5-cyberdeck |
| Licence | as in the repository (THIRD_PARTY.md lists the components) |

## The file

| | |
|---|---|
| File | `tab5-cyberdeck-v1.5.0-full.bin` |
| From | https://github.com/hey-its-brian/tab5-cyberdeck/releases/download/v1.5.0/tab5-cyberdeck-v1.5.0-full.bin |
| Size | 2,625,536 bytes |
| SHA-256 | `054fe8e721987534a958827a471bbfbd761ef273f473f4e8dfd7ab41e6e298bb` |
| Flash address | `0x0` (a merged image: bootloader, partition table, OTA data and the signed app) |
| Flash mode and size | DIO, 80 MHz, 16 MB, as the Tab5 ships |
| Erase before writing | Yes on a first install (the web flasher asks the same). Not needed to update: settings live in NVS at the same offset since v0.1 and survive |

Do not upload the `-ota.bin`: that is the app slot only, for the deck's own
over-the-air updater, and it is signed for that path.

## Short description (one line)

Cyberdeck firmware for the Tab5 and its keyboard: SSH and Ollama chat, markdown notes, a calculator, weather, a music player, a file manager with a LAN portal, network tools, and the SENTINEL radio-board suite. Updates itself over Wi-Fi.

## Description

DECK//OS turns the M5Stack Tab5 and the official Tab5 Keyboard into a small
cyberdeck: a touch-first, keyboard-friendly launcher with a cyberpunk HUD and
one module per job.

- TERMINAL: an SSH client (libssh2, xterm-256color) and an Ollama chat.
- NOTES: markdown notes with preview and split view, folders, and an
  optional sync with an Obsidian vault (Self-hosted LiveSync).
- CALC: scientific and programmer modes with a history tape.
- WEATHER: a forecast module and a live panel on the home screen.
- PLAYER: MP3s from the SD card with a spectrum, shuffle and repeat.
- FILES: browse the card, preview text and markdown, play music, and a LAN
  file portal reachable from any browser with a one-time PIN.
- NET TOOLS: Wi-Fi scan, ping, DNS, traceroute, port check, LAN devices and
  a speed test on the deck's own Wi-Fi.
- SENTINEL (optional, SETTINGS > APPLICATIONS > WIRELESS TOOLS): the suite
  for the RF Sentinel carrier board on the rear M5-Bus (a Stamp C5 with a
  433 MHz radio, a 2.4 GHz radio and a GPS). Wi-Fi, Bluetooth and the radio
  bands watched all the time, trackers that stay with you, wardriving,
  captures, a site survey and a jam watch. Without the board, leave it on
  NET TOOLS: nothing on the rear bus is powered.

After the first flash, updates arrive over Wi-Fi: the deck checks GitHub
releases at startup and hourly, offers the release notes in a popup, and
installs signed builds into the second app slot with rollback if the new
build does not come up. SETTINGS > UPDATE does the same by hand, with a
BETA switch for prereleases. A build installed from M5Burner takes these
updates too, so the M5Burner listing can lag behind the GitHub releases.

## What to know before flashing

- **Hardware.** Tab5 with the Tab5 Keyboard (the deck is usable by touch
  alone, but it is built around the keyboard). All three panel revisions
  (ILI9881C, ST7123, ST7121) are detected at runtime. A microSD card is
  optional and holds notes, music, captures and logs; without one, notes go
  to internal flash.
- **The ESP32-C6 stays stock.** Wi-Fi goes through the Tab5's C6 over
  ESP-Hosted, matched to M5Stack's factory C6 firmware (v1.4.1). DECK//OS
  never reflashes the C6. If another firmware reflashed it, Wi-Fi will not
  come up until the factory C6 firmware is restored.
- **One eFuse key block is written on first boot.** Settings (Wi-Fi
  password, SSH key) are encrypted with an HMAC key the deck generates and
  burns into one free eFuse key block the first time v0.7.1 or later boots.
  It does not lock the board, it does not restrict flashing, and nothing
  else is burned (no Secure Boot, no flash encryption). If the burn fails the
  deck falls back to plain settings. This is the only one-way change
  DECK//OS makes to a Tab5.
- **Updates are signed.** A deck running DECK//OS installs over-the-air
  updates only when they are signed with the project's key. USB and
  M5Burner can always write any image.
- **First boot takes a moment.** The boot POST runs, then the home screen.
  Join Wi-Fi in SETTINGS > NETWORK; the clock sets itself from NTP after
  that.

## Changelog for this version

v1.5.0 (2026-11-23)

- A cleaner home: a live panel (big clock, date, sections from your apps)
  and six fixed tiles with an icon, a name and their Alt+letter; three
  tile styles (GLOW, TWO-TONE, NEON).
- App toggles: SYSTEM is now SETTINGS, with an APPLICATIONS tab and a switch
  per optional app. A switched-off app is greyed in APPS, runs nothing in
  the background and ignores its shortcut.
- WIRELESS TOOLS: NET TOOLS (default) or SENTINEL, applied at the next
  restart.
- Warnings that land: alerts show as a red banner in any app, replace a
  plain warning's banner, stay twenty seconds and wake the screen; the
  status bar count turns red; the screensaver shows how many warnings wait.
- Shortcuts: Alt+1..6 for the home tiles, Alt+A for APPS, Alt+R NET TOOLS
  or Alt+X SENTINEL, Alt+H lists them all.

Earlier versions are in the GitHub releases and in `ROADMAP.md`.

## Links

- Releases: https://github.com/hey-its-brian/tab5-cyberdeck/releases
- Web flasher (same image, from a browser): https://hey-its-brian.github.io/tab5-cyberdeck/
- Source and README: https://github.com/hey-its-brian/tab5-cyberdeck
- Radio board firmware: https://github.com/hey-its-brian/rf-sentinel
