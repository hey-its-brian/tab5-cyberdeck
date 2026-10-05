# DECK//OS

Cyberdeck firmware for the **M5Stack Tab5** (ESP32-P4, 5" 1280x720) with the
official **Tab5 Keyboard**. A touch-first, keyboard-friendly launcher with a
cyberpunk HUD, hosting modules for SSH, markdown notes, a calculator and
weather.

![Home](docs/screenshots/home.png)

| Boot POST | System module | Scheduled module |
|---|---|---|
| ![Boot](docs/screenshots/boot.png) | ![System](docs/screenshots/system.png) | ![Offline module](docs/screenshots/module_offline.png) |

## Status: v0.3 in progress (Calculator)

![Calc](docs/screenshots/calc.png)

**v0.3 so far:** CALC module with live results as you type, a tape of past
results, hex/oct/bin readout for integers, DEG/RAD, and a touch keypad with
SCI and PROG pages that hides once you type. `Enter` evaluates, `Up/Down`
recall, `Ctrl+D` deg/rad, `Ctrl+K` keypad, `Ctrl+L` clear tape. Engine tests:
`cmake --build sim/build --target calc_test && sim/build/calc_test`.

### v0.2 (merged)

| Notes: split edit + live preview | Notes: preview |
|---|---|
| ![Split](docs/screenshots/notes_split.png) | ![Preview](docs/screenshots/notes_preview.png) |

**v0.2 so far:** NOTES module with a file browser (new, rename, delete), a
monospace editor, a rendered preview (headings, emphasis, lists, task lists,
quotes, code, tables, rules) and a live split view. Files are plain `.md` in
`/sdcard/notes`, or in internal flash when no card is inserted. `Ctrl+S` save,
`Ctrl+P` edit/preview, `Ctrl+T` split, `Esc` save and close, autosave after
20 s. Partition table is now OTA-ready (see ROADMAP v0.8), which means one USB
flash when moving from v0.1.

### v0.1.0 (released)

- Boot POST that reports real hardware state (keyboard, SD, RTC, battery); tap or any key skips it
- HUD status bar: clock, keyboard link, SD, network, battery
- Launcher with five large touch tiles, keyboard navigation and a live system ticker
- Glitch transitions, optional CRT scanlines, four accent themes (NETRUNNER, ARASAKA, NOMAD, MILITECH)
- **SYSTEM** module: brightness, scanlines, boot sequence, accent, RTC clock setting, live hardware readout
- TERMINAL, CALC and WEATHER are placeholders that show their planned features until their version lands

See [ROADMAP.md](ROADMAP.md) for v0.2 onward.

## Controls

| Keys | Action |
|---|---|
| `Alt+1..5` | Launch module 01..05 |
| `Alt+Esc` | Return to the deck from anywhere (reserved for when SSH owns `Esc`) |
| `Esc` | Back / close, or home if the module does not use it |
| `Left` `Right` `Enter`, or `1..5` | Pick a tile on the home screen |
| `Tab`, arrows | Move focus inside a module |

Everything is also reachable by touch: tiles, the `< DECK` button and on-screen controls.

## Build and flash

Needs ESP-IDF 5.4 or newer.

```bash
. ~/esp/esp-idf/export.sh
```

```bash
idf.py set-target esp32p4
```

```bash
idf.py build
```

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

The panel revision (ILI9881C, ST7123 or ST7121) is detected at runtime, so
one image works on every Tab5.

## Desktop simulator

`sim/` runs the same UI code (everything above `deck_hal`) on SDL2 with fake
hardware, so screens can be developed and checked without the board. It
reuses the LVGL tree that `idf.py build` downloads (or fetches LVGL 9.5.0 if
that is missing). Needs `brew install sdl2 cmake`.

```bash
cmake -S sim -B sim/build && cmake --build sim/build -j
```

```bash
./sim/build/deck_sim
```

Mouse is touch; the Mac keyboard acts as the Tab5 Keyboard (Option is Alt).
Headless mode renders scripted screenshots on a virtual clock:

```bash
./sim/build/deck_sim --headless --no-boot --key 500:alt+5 --shot 1500:system.ppm
```

`--key MS:SPEC` (e.g. `esc`, `enter`, `left`, `a`, `alt+2`, `ctrl+s`), `--tap MS:X,Y` and
`--shot MS:FILE.ppm` can repeat. Convert with `sips -s format png in.ppm --out out.png`.

## Layout

```
main/                 app_main: hal_init, theme, input, register modules, start shell
components/
  deck_hal/           the only hardware-aware layer (C API); hal_tab5.cpp for the board
  deck_ui/            theme tokens + fonts, chamfered panel widgets, effects
  deck_core/          app registry, key routing, shell, status bar, launcher, boot POST, modals
  deck_md/            markdown to LVGL renderer (md4c)
  md4c/               md4c markdown parser (vendored, MIT)
  m5_tab5_component/  M5Stack Tab5 BSP (vendored, MIT)
  m5_tab5_keyboard_component/  M5Stack Tab5 Keyboard driver (vendored, MIT)
apps/                 one component per module (app_sys is the template for real ones)
assets/fonts/         Orbitron, Share Tech Mono, MDI icon subset (tools/subset_icons.sh)
sim/                  SDL2 desktop simulator and fake HAL
```

### Writing a module

A module is a `deck_app_t` with `on_start(parent)` and optional `on_key`,
`on_exit` (save work; widgets still alive) and `on_stop` callbacks (see `components/deck_core/include/deck_app.h`). Build the
UI under `parent` with the `deck_ui` kit, register it in `main/main.c` and in
`sim/main.c`, and add its folder to `main/CMakeLists.txt`.

### Design notes

- **Keyboard path.** The Tab5 Keyboard runs in HID mode over I2C (0x6D on G0/G1)
  and reports releases as code 0. `deck_input` turns events into shell hotkeys,
  then the module's `on_key`, then LVGL keypad keys. It is polled every 10 ms
  rather than using the INT pin, which is edge triggered and can strand an event.
  The keyboard is hot-pluggable.
- **Display.** Direct mode (only dirty areas redraw), frame buffer in PSRAM,
  landscape via PPA rotation. Effects are sized for that: the grid and
  scanlines draw only inside the area being refreshed.
- **Fonts** are TTFs rendered at runtime by LVGL's tiny_ttf. Icons sit in their
  own labels (`deck_icon_text`) instead of being a font fallback, because each
  fallback lookup logs an error.

## Credits

See [THIRD_PARTY.md](THIRD_PARTY.md).
