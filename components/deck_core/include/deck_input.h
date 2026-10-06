/*
 * Keyboard input: turns Tab5 Keyboard HID events into
 *   1. shell hotkeys (Alt+Esc home, Alt+1..9 launch),
 *   2. app shortcuts (an app's on_key sees every press first),
 *   3. LVGL keypad keys for the focused widget (typing, arrows, Enter).
 *
 * The keypad indev is bound to one shared group (deck_input_group()). Widgets
 * created while it is the default group are added automatically.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HID modifier bits; left and right variants folded together. */
#define DECK_MOD_CTRL   0x11
#define DECK_MOD_SHIFT  0x22
#define DECK_MOD_ALT    0x44

/* HID usage IDs used by the shell and apps. */
#define HID_A          0x04
#define HID_H          0x0B
#define HID_Z          0x1D
#define HID_1          0x1E
#define HID_9          0x26
#define HID_0          0x27
#define HID_ENTER      0x28
#define HID_ESC        0x29
#define HID_BACKSPACE  0x2A
#define HID_TAB        0x2B
#define HID_SPACE      0x2C
#define HID_DELETE     0x4C
#define HID_RIGHT      0x4F
#define HID_LEFT       0x50
#define HID_DOWN       0x51
#define HID_UP         0x52

/* A key press as apps see it. */
typedef struct {
    uint8_t  code;   /* HID usage ID */
    uint8_t  mods;   /* HID modifier byte */
    uint32_t key;    /* LV_KEY_* or a Unicode character; 0 if it has no
                        meaning to widgets (e.g. Ctrl+S) */
} deck_key_t;

void        deck_input_init(void);

/* The group keyboard focus currently moves in. Normally the shared app
 * group; a modal pushes its own so focus cannot leave the dialog. */
lv_group_t *deck_input_group(void);
void        deck_input_push_group(lv_group_t *g);
void        deck_input_pop_group(void);

/* HID usage + modifiers to an LVGL key or character (US layout). */
uint32_t deck_input_translate(uint8_t code, uint8_t mods);

/* Forget any half-delivered key press (call when the screen is rebuilt). */
void deck_input_reset(void);

#ifdef __cplusplus
}
#endif
