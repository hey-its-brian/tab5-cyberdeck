/*
 * DECK//OS desktop simulator.
 *
 * Window mode:   deck_sim [--no-boot]
 *   mouse = touch, keyboard = Tab5 keyboard (Option/Alt = ALT).
 *
 * Headless mode renders scripted screenshots on a virtual clock:
 *   deck_sim --headless [--no-boot] [--wifi] [--accent N]
 *            [--key MS:SPEC]...   SPEC like esc, enter, left, a, 5, comma, alt+2, ctrl+s, shift+9
 *            [--tap MS:X,Y]...
 *            [--shot MS:FILE.ppm]...
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "deck_hal.h"
#include "deck_input.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "lvgl.h"
#include "sim.h"

#include "app_calc.h"
#include "app_notes.h"
#include "app_player.h"
#include "deck_audio.h"
#include "app_files.h"
#include "app_sys.h"
#include "app_term.h"
#include "app_weather.h"

/* ---- Scripted events (headless) ------------------------------------------ */

typedef enum { EV_KEY, EV_TAP, EV_SHOT } ev_type_t;

typedef struct {
    uint32_t at;
    ev_type_t type;
    uint8_t code, mods;
    int32_t x, y;
    char path[256];
} ev_t;

static ev_t s_ev[512];
static int s_ev_n;

static uint32_t s_now;
static bool s_realtime; /* --realtime: let real processes (the pty shell) keep up */
static uint32_t fake_tick(void) { return s_now; }

static bool s_touch_down;
static int32_t s_touch_x, s_touch_y;
static uint32_t s_touch_until;

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_touch_x;
    data->point.y = s_touch_y;
    data->state   = s_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ASCII to HID usage + shift (US layout), for --type. */
static bool ascii_to_hid(char c, uint8_t *code, uint8_t *mods)
{
    static const char plain[] = "1234567890\0\0\0\0 -=[]\\\0;'`,./";
    static const char shift[] = "!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";
    *mods = 0;
    if (c >= 'a' && c <= 'z') { *code = (uint8_t)(0x04 + c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *code = (uint8_t)(0x04 + c - 'A'); *mods = 0x02; return true; }
    if (c == '\n') { *code = 0x28; return true; }
    for (int i = 0; i < (int)sizeof(plain) - 1; i++) {
        if (plain[i] && plain[i] == c) { *code = (uint8_t)(0x1E + i); return true; }
        if (shift[i] && shift[i] == c) { *code = (uint8_t)(0x1E + i); *mods = 0x02; return true; }
    }
    return false;
}

static bool parse_key(const char *spec, uint8_t *code, uint8_t *mods)
{
    *mods = 0;
    for (;;) {
        if (strncmp(spec, "alt+", 4) == 0) {
            *mods |= 0x04;
            spec += 4;
        } else if (strncmp(spec, "ctrl+", 5) == 0) {
            *mods |= 0x01;
            spec += 5;
        } else if (strncmp(spec, "shift+", 6) == 0) {
            *mods |= 0x02;
            spec += 6;
        } else {
            break;
        }
    }
    static const struct {
        const char *name;
        uint8_t code;
    } names[] = {
        {"esc", 0x29},   {"enter", 0x28}, {"tab", 0x2B},  {"space", 0x2C}, {"bs", 0x2A},
        {"del", 0x4C},   {"right", 0x4F}, {"left", 0x50}, {"down", 0x51},  {"up", 0x52},
        {"minus", 0x2D}, {"equal", 0x2E}, {"lbracket", 0x2F}, {"rbracket", 0x30}, {"backslash", 0x31},
        {"semicolon", 0x33}, {"quote", 0x34}, {"grave", 0x35}, {"comma", 0x36}, {"dot", 0x37}, {"slash", 0x38},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(spec, names[i].name) == 0) {
            *code = names[i].code;
            return true;
        }
    }
    if (spec[0] >= 'a' && spec[0] <= 'z' && spec[1] == '\0') {
        *code = (uint8_t)(0x04 + spec[0] - 'a');
        return true;
    }
    if (spec[0] >= '1' && spec[0] <= '9' && spec[1] == '\0') {
        *code = (uint8_t)(0x1E + spec[0] - '1');
        return true;
    }
    if (spec[0] == '0' && spec[1] == '\0') {
        *code = 0x27;
        return true;
    }
    return false;
}

static void write_ppm(lv_display_t *disp, const char *path)
{
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    FILE *f            = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", HAL_SCREEN_W, HAL_SCREEN_H);
    for (int y = 0; y < HAL_SCREEN_H; y++) {
        const uint16_t *row = (const uint16_t *)(buf->data + (size_t)y * buf->header.stride);
        for (int x = 0; x < HAL_SCREEN_W; x++) {
            uint16_t c     = row[x];
            uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 0x1F) * 255 / 31), (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
                              (uint8_t)((c & 0x1F) * 255 / 31)};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("[sim] %5u ms  wrote %s\n", s_now, path);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static int run_headless(void)
{
    lv_tick_set_cb(fake_tick);
    lv_display_t *disp = lv_display_create(HAL_SCREEN_W, HAL_SCREEN_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    size_t size = (size_t)HAL_SCREEN_W * HAL_SCREEN_H * 2;
    void *fb    = malloc(size);
    lv_display_set_buffers(disp, fb, NULL, size, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    return 0;
}

static void headless_loop(lv_display_t *disp)
{
    uint32_t end = 0;
    for (int i = 0; i < s_ev_n; i++) {
        if (s_ev[i].at > end) end = s_ev[i].at;
    }
    for (s_now = 0; s_now <= end; s_now += 5) {
        for (int i = 0; i < s_ev_n; i++) {
            ev_t *e = &s_ev[i];
            if (e->type == EV_KEY) {
                if (e->at == s_now) sim_push_key(e->code, e->mods, true);
                if (e->at + 60 == s_now) sim_push_key(0, e->mods, false);
            } else if (e->type == EV_TAP && e->at == s_now) {
                s_touch_x     = e->x;
                s_touch_y     = e->y;
                s_touch_down  = true;
                s_touch_until = s_now + 80;
            }
        }
        if (s_touch_down && s_now >= s_touch_until) s_touch_down = false;
        lv_timer_handler();
        if (s_realtime) usleep(5000);
        for (int i = 0; i < s_ev_n; i++) {
            if (s_ev[i].type == EV_SHOT && s_ev[i].at == s_now) {
                lv_refr_now(disp);
                write_ppm(disp, s_ev[i].path);
            }
        }
    }
}

/* ---- Window mode --------------------------------------------------------- */

static int key_watch(void *ud, SDL_Event *ev)
{
    (void)ud;
    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP) return 0;
    SDL_Scancode sc = ev->key.keysym.scancode;
    if (sc >= SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_CAPSLOCK) return 0; /* modifiers are not keys */
    if (ev->type == SDL_KEYDOWN && ev->key.repeat) return 0;              /* the real keyboard never repeats */
    uint16_t m   = ev->key.keysym.mod;
    uint8_t mods = (uint8_t)(((m & KMOD_CTRL) ? 0x01 : 0) | ((m & KMOD_SHIFT) ? 0x02 : 0) |
                             ((m & (KMOD_ALT | KMOD_GUI)) ? 0x04 : 0));
    /* SDL scancodes are USB HID usage IDs, same as the Tab5 Keyboard reports. */
    sim_push_key((uint8_t)sc, mods, ev->type == SDL_KEYDOWN);
    return 0;
}

/* ---- Main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    bool headless = false, boot = true;
    int accent    = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : "";
        if (strcmp(a, "--headless") == 0) {
            headless = true;
        } else if (strcmp(a, "--realtime") == 0) {
            s_realtime = true;
        } else if (strcmp(a, "--wifi") == 0) {
            hal_cfg_set_str("wifi_ssid", "NIGHTCITY-5G"); /* start "online" */
        } else if (strcmp(a, "--no-boot") == 0) {
            boot = false;
        } else if (strcmp(a, "--accent") == 0) {
            accent = atoi(v);
            i++;
        } else if (strcmp(a, "--type") == 0) {
            /* --type MS:TEXT  ("\n" in TEXT presses Enter); one key every 40 ms */
            uint32_t at      = (uint32_t)strtoul(v, NULL, 10) / 5 * 5;
            const char *text = strchr(v, ':');
            for (const char *c = text ? text + 1 : ""; *c && s_ev_n < 512; c++, at += 40) {
                char ch = *c;
                if (c[0] == '\\' && c[1] == 'n') { ch = '\n'; c++; }
                ev_t *e = &s_ev[s_ev_n];
                if (ascii_to_hid(ch, &e->code, &e->mods)) { e->at = at; e->type = EV_KEY; s_ev_n++; }
            }
            i++;
        } else if ((strcmp(a, "--key") == 0 || strcmp(a, "--tap") == 0 || strcmp(a, "--shot") == 0) &&
                   s_ev_n < 512) {
            ev_t *e      = &s_ev[s_ev_n];
            e->at        = (uint32_t)strtoul(v, NULL, 10) / 5 * 5;
            const char *rest = strchr(v, ':');
            if (rest == NULL) continue;
            rest++;
            if (a[2] == 'k') {
                e->type = EV_KEY;
                if (!parse_key(rest, &e->code, &e->mods)) {
                    fprintf(stderr, "bad key %s\n", rest);
                    return 1;
                }
            } else if (a[2] == 't') {
                e->type = EV_TAP;
                sscanf(rest, "%d,%d", &e->x, &e->y);
            } else {
                e->type = EV_SHOT;
                snprintf(e->path, sizeof(e->path), "%s", rest);
            }
            s_ev_n++;
            i++;
        }
    }

    lv_init();
    lv_display_t *disp;
    if (headless) {
        run_headless();
        disp = lv_display_get_default();
    } else {
        disp = lv_sdl_window_create(HAL_SCREEN_W, HAL_SCREEN_H);
        lv_sdl_window_set_title(disp, "DECK//OS sim");
        lv_sdl_mouse_create();
        SDL_AddEventWatch(key_watch, NULL);
    }

    if (accent >= 0) hal_cfg_set_i32("accent", accent);
    deck_theme_init();
    deck_input_init();
    player_init();
    deck_app_register(app_term());
    deck_app_register(app_notes());
    deck_app_register(app_calc());
    deck_app_register(app_weather());
    deck_app_register(app_player());
    deck_app_register(app_files());
    deck_app_register(app_sys());
    deck_shell_start(boot);

    if (headless) {
        headless_loop(disp);
        return 0;
    }
    for (;;) {
        uint32_t wait = lv_timer_handler();
        SDL_Delay(wait < 5 ? wait : 5);
    }
}
