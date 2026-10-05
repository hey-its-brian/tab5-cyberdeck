#include "deck_input.h"

#include "deck_hal.h"
#include "deck_shell.h"

#define PUMP_MS 10
#define PENDING_MAX 16

/* Keys waiting to be handed to LVGL through the keypad read callback. */
typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} pending_t;

static lv_group_t *s_group;
static lv_indev_t *s_indev;
static pending_t s_pending[PENDING_MAX];
static int s_head, s_tail;
static uint32_t s_down; /* LVGL key currently held, 0 if none */

static void push(uint32_t key, lv_indev_state_t state)
{
    int next = (s_tail + 1) % PENDING_MAX;
    if (next == s_head) return; /* full: drop rather than block the UI */
    s_pending[s_tail] = (pending_t){key, state};
    s_tail            = next;
}

/* Unshifted / shifted characters for HID usages 0x1E..0x38 (US layout). */
static const char s_plain[] = "1234567890\0\0\0\0 -=[]\\\0;'`,./";
static const char s_shift[] = "!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";

uint32_t deck_input_translate(uint8_t code, uint8_t mods)
{
    bool shift = (mods & DECK_MOD_SHIFT) != 0;
    bool chord = (mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) != 0;

    switch (code) {
        case HID_ENTER: return LV_KEY_ENTER;
        case HID_ESC: return LV_KEY_ESC;
        case HID_BACKSPACE: return LV_KEY_BACKSPACE;
        case HID_TAB: return shift ? LV_KEY_PREV : LV_KEY_NEXT;
        case HID_DELETE: return LV_KEY_DEL;
        case HID_RIGHT: return LV_KEY_RIGHT;
        case HID_LEFT: return LV_KEY_LEFT;
        case HID_DOWN: return LV_KEY_DOWN;
        case HID_UP: return LV_KEY_UP;
        case 0x4A: return LV_KEY_HOME;
        case 0x4D: return LV_KEY_END;
        default: break;
    }
    if (chord) return 0; /* Ctrl/Alt + character is a shortcut, not text */

    if (code >= HID_A && code <= HID_Z) {
        return (uint32_t)((shift ? 'A' : 'a') + (code - HID_A));
    }
    if (code >= HID_1 && code <= 0x38) {
        char c = (shift ? s_shift : s_plain)[code - HID_1];
        return (uint32_t)(unsigned char)c;
    }
    return 0;
}

/* LVGL's keypad sends arrows to the focused widget. For buttons and switches
 * that does nothing useful, so turn arrows into focus movement unless the
 * focused widget actually uses that axis. */
static uint32_t route_arrow(uint32_t key)
{
    if (key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_LEFT && key != LV_KEY_RIGHT) {
        return key;
    }
    lv_obj_t *f = lv_group_get_focused(s_group);
    bool horizontal = key == LV_KEY_LEFT || key == LV_KEY_RIGHT;
    if (f != NULL) {
        if (lv_obj_check_type(f, &lv_textarea_class)) return key;
        if (lv_obj_check_type(f, &lv_slider_class) && horizontal) return key;
        if (lv_obj_check_type(f, &lv_roller_class) && !horizontal) return key;
    }
    return (key == LV_KEY_UP || key == LV_KEY_LEFT) ? LV_KEY_PREV : LV_KEY_NEXT;
}

/* Drain hardware key events on the LVGL task, where it is safe to change
 * the UI (the indev read callback is not). */
static void pump(lv_timer_t *t)
{
    (void)t;
    hal_key_t k;
    while (hal_key_poll(&k)) {
        if (!k.pressed) {
            /* The keyboard reports releases as code 0, so release whatever
             * LVGL key we last pressed. */
            if (s_down) {
                push(s_down, LV_INDEV_STATE_RELEASED);
                s_down = 0;
            }
            continue;
        }

        deck_key_t dk = {k.code, k.mods, deck_input_translate(k.code, k.mods)};
        if (deck_shell_key(&dk) || dk.key == 0) {
            continue;
        }
        uint32_t key = route_arrow(dk.key);
        if (s_down) push(s_down, LV_INDEV_STATE_RELEASED); /* rollover typing */
        push(key, LV_INDEV_STATE_PRESSED);
        s_down = key;
    }
    if (s_head != s_tail) {
        lv_indev_read(s_indev); /* deliver now instead of on the next poll */
    }
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint32_t last_key;
    if (s_head == s_tail) {
        /* Nothing new: keep reporting the held key so LVGL can auto-repeat. */
        data->key   = s_down ? s_down : last_key;
        data->state = s_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
    pending_t p            = s_pending[s_head];
    s_head                 = (s_head + 1) % PENDING_MAX;
    data->key              = p.key;
    data->state            = p.state;
    last_key               = p.key;
    data->continue_reading = s_head != s_tail;
}

void deck_input_reset(void)
{
    s_head = s_tail = 0;
    s_down = 0;
}

lv_group_t *deck_input_group(void) { return s_group; }

void deck_input_init(void)
{
    s_group = lv_group_create();
    lv_group_set_default(s_group);
    lv_group_set_wrap(s_group, true);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_indev, keypad_read);
    lv_indev_set_group(s_indev, s_group);
    lv_indev_set_long_press_time(s_indev, 450);
    lv_indev_set_long_press_repeat_time(s_indev, 45);

    lv_timer_create(pump, PUMP_MS, NULL);
}
