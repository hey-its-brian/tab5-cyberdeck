# Third-party components

| Component | Source | License | Notes |
|---|---|---|---|
| `components/m5_tab5_component` | [M5Stack M5Tab5-Keyboard-UserDemo](https://github.com/m5stack/M5Tab5-Keyboard-UserDemo) | MIT | Tab5 BSP and LVGL port. Changes: `src/tools/m5tab5_tools_lvgl.cpp` removed from the build (unused, fails `-Werror=format` on IDF 5.4); multi-touch emulation in `lvgl_port_touch.cpp` disabled behind `M5TAB5_TOUCH_MULTI_EMULATION` (it double-toggled switches and auto-repeated clicks on long holds). |
| `components/m5_tab5_keyboard_component` | same | MIT | Tab5 Keyboard I2C driver, unmodified. |
| `components/md4c` | [mity/md4c](https://github.com/mity/md4c) 0.5.2 | MIT | Markdown parser, unmodified. |
| LVGL 9.5 | [lvgl/lvgl](https://github.com/lvgl/lvgl) via the component registry | MIT | |
| Orbitron | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/orbitron) | SIL OFL 1.1 | `assets/fonts/OFL-Orbitron.txt` |
| Share Tech Mono | [Google Fonts](https://github.com/google/fonts/tree/main/ofl/sharetechmono) | SIL OFL 1.1 | `assets/fonts/OFL-ShareTechMono.txt` |
| Material Design Icons (subset) | [Templarian/MaterialDesign](https://github.com/Templarian/MaterialDesign) | Pictogrammers Free License (icons Apache 2.0) | `assets/fonts/MDI_LICENSE`; subset built by `tools/subset_icons.sh` |

Keyboard protocol reference: [Tab5 Keyboard docs](https://docs.m5stack.com/en/tab5/Tab5_Keyboard)
and [M5Tab5-Keyboard-Internal-FW](https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW).
