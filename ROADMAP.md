# Roadmap

Each version ends with something flashable and usable.

## v0.1: Shell and cyberpunk UI (done)

- [x] Tab5 bring-up via M5Stack's BSP (all panel revisions), landscape through PPA
- [x] Tab5 Keyboard in HID mode with hot-plug, routed to hotkeys, modules and LVGL
- [x] Boot POST, HUD status bar, launcher, glitch transitions, scanlines
- [x] SYSTEM module: brightness, accents, toggles, RTC clock, hardware readout
- [x] Desktop simulator with headless screenshots
- [ ] Verify on hardware: boot time under 3 s, touch and keyboard on every screen

## v0.2: Markdown (read, write, preview)

- [x] OTA-ready partition table (two 6 MB app slots) so v0.6 can update without USB
- [x] md4c parser mapped to LVGL: headings, emphasis, lists, task lists, quotes, fenced code, tables, rules
- [x] File browser for `/sdcard/notes`: new, rename, delete (internal flash fallback without a card)
- [x] Monospace editor: autosave, `Ctrl+S`, save on exit, on-screen keyboard when no keyboard is attached
- [x] `Ctrl+P` toggles edit and preview; `Ctrl+T` side-by-side split with live preview
- [ ] Line numbers in the editor
- [ ] Checkpoint on hardware: write a note on the keyboard, preview it, power-cycle, it is still there

## v0.3: Calculator

- [x] Recursive-descent engine (doubles): `( ) ^ % !`, sin..round, pi/e/`ans`, DEG/RAD; 56 host unit tests
- [x] History tape with Up/Down recall; an operator on an empty line continues from `ans`
- [x] Programmer mode: hex/bin/oct literals and readout, `& | xor ~ << >>`, hex keys auto-prefix `0x`
- [x] Touch keypad (SCI/PROG pages) that hides while typing; Ctrl+K toggles
- [x] Checkpoint in the simulator: `(0xFF << 2) | 3` = 1023 and `2^10 / 3` = 341.333333333, from keyboard and touch
- [ ] Checkpoint on hardware

## v0.4: Weather and the network layer

- ESP32-C6 Wi-Fi over SDIO (esp_hosted), Wi-Fi setup in SYSTEM
- SNTP clock sync, timezone setting, HTTPS with the cert bundle
- Open-Meteo: conditions, 24 h trace, 7-day strip, offline cache
- Status bar NET indicator goes live
- Checkpoint: survives a Wi-Fi drop and reconnect; correct time after cold boot

## v0.5: SSH client

- wolfSSH + wolfSSL (libssh2 port as fallback), P4 hardware crypto
- VT100/xterm-256color emulator, dirty-rect grid renderer, PSRAM scrollback
- Full key passthrough incl. `Esc`; `Alt+Esc` returns to the deck
- Host profiles, known_hosts pinning, ed25519 keys on SD
- Checkpoint: `htop` and `vim` usable on a Linux box

## v0.6: OTA updates

- A/B app slots (layout already in place since v0.2, so no USB reflash needed)
- Check GitHub Releases for a newer firmware from SYSTEM, show notes, download, verify, swap slot
- Rollback: a new image must mark itself good after boot, otherwise the bootloader reverts
- Optional: push a build from the Mac over the local network during development
- Checkpoint: v0.6 updates itself to a test v0.6.1 over Wi-Fi, and a deliberately broken image rolls back

## v1.0: Polish

- Battery stats, idle sleep, wake on key
- More accents, cross-module links (open a note from SSH, send a result to Notes)
- 24 h soak test for leaks
