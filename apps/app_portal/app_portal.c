/*
 * PORTAL module: switch the LAN file portal on and off, and show how to
 * reach it (URL, PIN, QR code for phones) plus a live transfer log.
 * The portal keeps running after you leave this screen; the status bar
 * shows LINK while it is up, and it switches itself off when idle.
 *
 * Keys: Enter or Space toggles the portal.
 */
#include "app_portal.h"

#include <stdio.h>
#include <string.h>

#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_net.h"
#include "deck_portal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define LOG_LINES 14

static struct {
    lv_obj_t *state;
    lv_obj_t *toggle;
    lv_obj_t *on_box;
    lv_obj_t *off_box;
    lv_obj_t *off_msg;
    lv_obj_t *url_host;
    lv_obj_t *url_ip;
    lv_obj_t *pin;
    lv_obj_t *qr;
    lv_obj_t *idle;
    lv_obj_t *log;
    lv_timer_t *timer;
    uint32_t cursor;
    char ip_shown[16];
} s_ui;

/* Events survive leaving the screen, so the log shows recent history. */
static char s_log[LOG_LINES][80];
static int s_log_n;

static void log_push(const portal_event_t *e)
{
    if (s_log_n == LOG_LINES) {
        memmove(s_log[0], s_log[1], sizeof(s_log[0]) * (LOG_LINES - 1));
        s_log_n--;
    }
    snprintf(s_log[s_log_n++], sizeof(s_log[0]), "%s %s", e->error ? "!" : ">", e->text);
}

static void render_log(void)
{
    static char buf[LOG_LINES * 80];
    size_t k = 0;
    buf[0]   = '\0';
    for (int i = 0; i < s_log_n; i++) k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%s\n", s_log[i]);
    lv_label_set_text(s_ui.log, s_log_n ? buf : "> no activity yet");
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    static uint32_t s_global_cursor;
    portal_event_t ev[8];
    int n;
    bool changed = false;
    while ((n = portal_events(ev, 8, &s_global_cursor)) > 0) {
        for (int i = 0; i < n; i++) log_push(&ev[i]);
        changed = true;
    }
    if (changed) render_log();

    bool on = portal_running();
    lv_label_set_text(s_ui.state, on ? "LINK ACTIVE" : "LINK DOWN");
    lv_obj_set_style_text_color(s_ui.state, on ? g_pal.ok : g_pal.dim, 0);
    lv_label_set_text(lv_obj_get_child(s_ui.toggle, 0), on ? "DISENGAGE" : "ENGAGE");

    if (on) {
        lv_obj_remove_flag(s_ui.on_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.off_box, LV_OBJ_FLAG_HIDDEN);
        if (strcmp(s_ui.ip_shown, net_ip()) != 0) {
            snprintf(s_ui.ip_shown, sizeof(s_ui.ip_shown), "%s", net_ip());
            char url[48];
            snprintf(url, sizeof(url), "http://%s", net_ip());
            lv_label_set_text(s_ui.url_ip, url);
            lv_qrcode_update(s_ui.qr, url, (uint32_t)strlen(url));
        }
        lv_label_set_text_fmt(s_ui.url_host, "http://%s", portal_host());
        lv_label_set_text(s_ui.pin, portal_pin());
        uint32_t idle = portal_idle_s();
        if (portal_busy()) {
            lv_label_set_text(s_ui.idle, "TRANSFER IN PROGRESS");
        } else {
            uint32_t left = idle >= PORTAL_IDLE_OFF_S ? 0 : (PORTAL_IDLE_OFF_S - idle + 59) / 60;
            lv_label_set_text_fmt(s_ui.idle, "AUTO-OFF IN %lu MIN WITHOUT ACTIVITY", (unsigned long)left);
        }
    } else {
        lv_obj_add_flag(s_ui.on_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.off_box, LV_OBJ_FLAG_HIDDEN);
        s_ui.ip_shown[0] = '\0';
        bool online      = net_state() == NET_CONNECTED;
        lv_label_set_text(s_ui.off_msg, online ? "Share this deck's storage with any browser on your network.\n"
                                                 "Engage to get the address and a one-time PIN."
                                               : "No network. Join Wi-Fi in SYSTEM first.");
        lv_obj_set_style_text_color(s_ui.off_msg, online ? g_pal.text : g_pal.warn, 0);
    }
}

static void toggle(void)
{
    if (portal_running()) {
        portal_stop();
    } else if (!portal_start()) {
        portal_event_t e = {.error = true};
        snprintf(e.text, sizeof(e.text), "%s", net_state() == NET_CONNECTED ? "could not start (no storage?)"
                                                                            : "no network: join Wi-Fi in SYSTEM");
        log_push(&e);
        render_log();
    }
    refresh(NULL);
}

static void toggle_clicked(lv_event_t *e)
{
    (void)e;
    toggle();
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_column(root, 20, 0);

    /* Left: access */
    lv_obj_t *left = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(left, 700, LV_PCT(100));
    deck_panel_set_tab(left, true);
    lv_obj_set_style_pad_all(left, 26, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 14, 0);

    lv_obj_t *head = deck_box(left);
    lv_obj_set_width(head, LV_PCT(100));
    s_ui.state = deck_label(head, g_font.disp_l, g_pal.dim, "");
    lv_obj_align(s_ui.state, LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.toggle = deck_button(head, "ENGAGE", g_font.disp_m);
    lv_obj_align(s_ui.toggle, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_ui.toggle, toggle_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_remove_obj(s_ui.toggle); /* Enter/Space are handled in on_key */
    deck_section(left, "ACCESS");

    /* On: URL + PIN + QR */
    s_ui.on_box = deck_box(left);
    lv_obj_set_size(s_ui.on_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui.on_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_ui.on_box, 26, 0);

    lv_obj_t *txt = deck_box(s_ui.on_box);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 6, 0);
    deck_label(txt, g_font.mono_s, g_pal.dim, "OPEN IN A BROWSER ON THE SAME NETWORK");
    s_ui.url_host = deck_label(txt, g_font.disp_m, g_pal.accent, "");
    s_ui.url_ip   = deck_label(txt, g_font.mono_l, g_pal.text, "");
    lv_obj_t *pl  = deck_label(txt, g_font.mono_s, g_pal.dim, "PIN");
    lv_obj_set_style_margin_top(pl, 14, 0);
    s_ui.pin = deck_label(txt, g_font.disp_hero, g_pal.accent2, "");
    lv_obj_set_style_text_letter_space(s_ui.pin, 6, 0);

    s_ui.qr = lv_qrcode_create(s_ui.on_box);
    lv_qrcode_set_size(s_ui.qr, 200);
    lv_qrcode_set_dark_color(s_ui.qr, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_ui.qr, true);

    /* Off: explanation */
    s_ui.off_box = deck_box(left);
    lv_obj_set_width(s_ui.off_box, LV_PCT(100));
    s_ui.off_msg = deck_label(s_ui.off_box, g_font.mono_m, g_pal.text, "");
    lv_obj_set_width(s_ui.off_msg, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_ui.off_msg, 6, 0);

    lv_obj_t *spacer = deck_box(left);
    lv_obj_set_flex_grow(spacer, 1);
    s_ui.idle = deck_label(left, g_font.mono_s, g_pal.dim, "");
    deck_label(left, g_font.mono_s, g_pal.dim,
               "Folders: /notes (NOTES module), /music (player, v0.6). [ENTER] toggles the link.");

    /* Right: log */
    lv_obj_t *right = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_all(right, 26, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 14, 0);
    deck_section(right, "LINK LOG");
    s_ui.log = deck_label(right, g_font.mono_s, g_pal.text, "");
    lv_obj_set_width(s_ui.log, LV_PCT(100));
    lv_label_set_long_mode(s_ui.log, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_line_space(s_ui.log, 6, 0);

    render_log();
    refresh(NULL);
    s_ui.timer = lv_timer_create(refresh, 500, NULL);
    return true;
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    memset(&s_ui, 0, sizeof(s_ui));
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if ((k->code == HID_ENTER || k->code == HID_SPACE) && !(k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) {
        toggle();
        return true;
    }
    return false;
}

static deck_app_t s_app = {
    .name     = "PORTAL",
    .tagline  = "FILE UPLINK",
    .icon     = ICON_SERVER,
    .eta      = NULL,
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_portal(void) { return &s_app; }
