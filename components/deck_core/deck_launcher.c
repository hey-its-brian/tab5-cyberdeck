/*
 * Home screen: one row of tall module tiles (big touch targets), a header,
 * and a scrolling system ticker along the bottom.
 */
#include "deck_launcher.h"

#include <stdio.h>

#include "deck_hal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TILE_W 226
#define TILE_H 470
#define TILE_GAP 22
#define MAX_TILES 9

static lv_obj_t *s_tiles[MAX_TILES];
static int s_tile_count;
static int s_sel;          /* keyboard selection, survives trips into apps */
static bool s_kbd_nav;     /* only show the selection once the keyboard is used */
static lv_obj_t *s_uptime;
static lv_timer_t *s_timer;

static void tile_clicked(lv_event_t *e)
{
    deck_app_t *app = (deck_app_t *)lv_event_get_user_data(e);
    s_sel           = deck_app_index(app);
    deck_shell_launch(app);
}

static void show_selection(void)
{
    for (int i = 0; i < s_tile_count; i++) {
        if (s_kbd_nav && i == s_sel) {
            lv_obj_add_state(s_tiles[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_tiles[i], LV_STATE_CHECKED);
        }
    }
}

static lv_obj_t *build_tile(lv_obj_t *parent, deck_app_t *app, int index)
{
    lv_obj_t *t = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(t, TILE_W, TILE_H);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    deck_panel_set_tab(t, true);
    deck_panel_set_hazard(t, app->eta != NULL);
    lv_obj_set_style_pad_all(t, 18, 0);
    lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, app);

    char num[4];
    snprintf(num, sizeof(num), "%02d", index + 1);
    lv_obj_t *n = deck_label(t, g_font.disp_l, g_pal.accent2, num);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 4, 10);

    lv_obj_t *chip = app->eta ? deck_chip(t, app->eta, g_pal.warn) : deck_chip(t, "ONLINE", g_pal.ok);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, 0, 14);

    lv_obj_t *icon = deck_label(t, g_font.icon_l, app->eta ? g_pal.dim : g_pal.accent, app->icon);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *name = deck_label(t, g_font.disp_m, g_pal.text, app->name);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 52);

    lv_obj_t *tag = deck_label(t, g_font.mono_s, g_pal.dim, app->tagline);
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, 84);

    char hot[12];
    snprintf(hot, sizeof(hot), "ALT+%d", index + 1);
    lv_obj_t *h = deck_label(t, g_font.mono_s, g_pal.dim, hot);
    lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 4, -18);

    lv_obj_t *state = deck_label(t, g_font.mono_s, app->eta ? g_pal.warn : g_pal.ok,
                                 app->eta ? "OFFLINE" : "READY");
    lv_obj_align(state, LV_ALIGN_BOTTOM_RIGHT, -8, -18);
    return t;
}

static void uptime_tick(lv_timer_t *t)
{
    (void)t;
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    uint32_t s = si.uptime_s;
    lv_label_set_text_fmt(s_uptime, "NODE %s  ::  UPTIME %02lu:%02lu:%02lu", si.board, (unsigned long)(s / 3600),
                          (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
}

static void launcher_deleted(lv_event_t *e)
{
    (void)e;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_tile_count = 0;
    s_uptime     = NULL;
}

static const char *ticker_text(void)
{
    static char buf[320];
    char sd[40];
    if (hal_sd_mounted()) {
        snprintf(sd, sizeof(sd), "SD %.1f GB FREE", (double)hal_sd_free_bytes() / 1e9);
    } else {
        snprintf(sd, sizeof(sd), "SD NO MEDIA");
    }
    snprintf(buf, sizeof(buf),
             "KBD LINK %s   //   %s   //   NET OFFLINE (SCHEDULED v0.4)   //   "
             "ALT+1..%d QUICK LAUNCH   //   ESC RETURNS TO DECK   //   THE NET IS VAST, STAY FROSTY   //   ",
             hal_kbd_present() ? "ONLINE" : "DOWN", sd, (int)deck_app_count());
    return buf;
}

void deck_launcher_create(lv_obj_t *parent)
{
    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(root, 24, 0);
    lv_obj_set_style_pad_row(root, 20, 0);
    lv_obj_add_event_cb(root, launcher_deleted, LV_EVENT_DELETE, NULL);

    /* Header row */
    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    deck_label(head, g_font.disp_s, g_pal.accent, "// SELECT MODULE");
    s_uptime = deck_label(head, g_font.mono_s, g_pal.dim, "");
    lv_obj_align(s_uptime, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Tiles */
    lv_obj_t *row = deck_box(root);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, TILE_GAP, 0);
    s_tile_count = 0;
    for (size_t i = 0; i < deck_app_count() && i < MAX_TILES; i++) {
        s_tiles[s_tile_count++] = build_tile(row, deck_app_get(i), (int)i);
    }
    if (s_sel >= s_tile_count) s_sel = 0;
    show_selection();

    /* Ticker */
    lv_obj_t *bar = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 12);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_style_pad_hor(bar, 14, 0);

    lv_obj_t *tag = deck_panel(bar, DECK_CUT_BR, 8);
    deck_panel_set_fill(tag, g_pal.accent);
    deck_panel_set_outline(tag, g_pal.accent);
    lv_obj_set_size(tag, 76, 30);
    lv_obj_align(tag, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *tl = deck_label(tag, g_font.disp_s, g_pal.bg, "SYS>");
    lv_obj_center(tl);

    lv_obj_t *tick = deck_label(bar, g_font.mono_m, g_pal.text, ticker_text());
    lv_label_set_long_mode(tick, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(tick, 1228 - 28 - 76 - 20);
    lv_obj_set_style_anim_duration(tick, 70, 0); /* for scrolling labels this is px/s */
    lv_obj_align(tick, LV_ALIGN_LEFT_MID, 92, 0);

    uptime_tick(NULL);
    s_timer = lv_timer_create(uptime_tick, 1000, NULL);
}

bool deck_launcher_key(const deck_key_t *k)
{
    if (s_tile_count == 0) return false;

    int target = -1;
    switch (k->code) {
        case HID_LEFT:
            s_sel = s_kbd_nav ? (s_sel + s_tile_count - 1) % s_tile_count : s_sel;
            break;
        case HID_RIGHT:
        case HID_TAB:
            s_sel = s_kbd_nav ? (s_sel + 1) % s_tile_count : s_sel;
            break;
        case HID_ENTER:
        case HID_SPACE:
            if (s_kbd_nav) target = s_sel;
            break;
        default:
            if (k->code >= HID_1 && k->code < HID_1 + s_tile_count) {
                target = k->code - HID_1;
                s_sel  = target;
            } else {
                return false;
            }
            break;
    }
    s_kbd_nav = true;
    show_selection();
    if (target >= 0) {
        deck_shell_launch(deck_app_get((size_t)target));
    }
    return true;
}
