/*
 * SENTINEL > BLUETOOTH: the Bluetooth LE devices in earshot.
 *
 *   DEVICES   everything heard; a row opens that device
 *   TRACKERS  only trackers and surveillance devices
 *   CAPTURE   record advertising to a file (the screen is in sn_wifi.c)
 *
 * The board listens passively and gets the radio full time while one of
 * these is open. Devices that deserve a look come first; the rest follow by
 * signal, and the order is only worked out again every few seconds so rows
 * do not jump about under the eye or the thumb.
 */
#include "sn_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_icons.h"
#include "deck_theme.h"

#define TICK_MS    500
#define REORDER_MS 5000

static struct {
    lv_timer_t *timer;
    bool        trackers_only, detail;
    sn_list_t   list;
    lv_obj_t   *summary, *state, *v[6];
    uint8_t     order[SENTINEL_BLETAB_MAX][6]; /* the addresses in the order shown */
    int         order_n;
    uint32_t    ordered_at;
    int         shown[SENTINEL_BLETAB_MAX]; /* table index of each listed device */
    int         n;
    bool        have_sel;
    uint8_t     sel[6];
    sentinel_state_t  st;
    sentinel_bletab_t tab;
} s_ble DECK_PSRAM_BSS;

static void mine_toggle(void);
static void list_build(void);

static bool flagged(const sentinel_bdv_t *d) { return sentinel_ble_is_tracker(d->kind) || sentinel_ble_is_surveillance(d->kind); }

/* A surveillance device, or a tracker that has stayed with you, is a
 * warning. A tracker that is just nearby is worth showing first, no more. */
static sn_level_t dev_level(const sentinel_bdv_t *d)
{
    if (sentinel_ble_mine(d->addr)) return SN_NOTE;
    if (sentinel_ble_is_surveillance(d->kind)) return SN_WARNING;
    sentinel_track_t t;
    if (sentinel_ble_is_tracker(d->kind) && sentinel_ble_track(d->addr, &t) && t.alerted) return SN_WARNING;
    return SN_NOTE;
}

static int find(const uint8_t addr[6])
{
    for (int i = 0; i < s_ble.tab.n; i++)
        if (memcmp(s_ble.tab.dev[i].d.addr, addr, 6) == 0) return i;
    return -1;
}

/* The order shown: flagged devices first, then by signal (the table's own
 * order), settled every REORDER_MS. In between, a device that goes is
 * dropped and a new one joins at the end. */
static void order_update(void)
{
    uint32_t now = sentinel_now_ms();
    if (!s_ble.ordered_at || now - s_ble.ordered_at >= REORDER_MS) {
        s_ble.ordered_at = now ? now : 1;
        s_ble.order_n    = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < s_ble.tab.n; i++)
                if (flagged(&s_ble.tab.dev[i].d) == (pass == 0)) memcpy(s_ble.order[s_ble.order_n++], s_ble.tab.dev[i].d.addr, 6);
    } else {
        int keep = 0;
        for (int i = 0; i < s_ble.order_n; i++)
            if (find(s_ble.order[i]) >= 0) {
                if (keep != i) memcpy(s_ble.order[keep], s_ble.order[i], 6);
                keep++;
            }
        s_ble.order_n = keep;
        for (int i = 0; i < s_ble.tab.n && s_ble.order_n < SENTINEL_BLETAB_MAX; i++) {
            bool known = false;
            for (int k = 0; k < s_ble.order_n && !known; k++) known = memcmp(s_ble.order[k], s_ble.tab.dev[i].d.addr, 6) == 0;
            if (!known) memcpy(s_ble.order[s_ble.order_n++], s_ble.tab.dev[i].d.addr, 6);
        }
    }
    s_ble.n = 0;
    for (int i = 0; i < s_ble.order_n; i++) {
        int t = find(s_ble.order[i]);
        if (t < 0 || (s_ble.trackers_only && !flagged(&s_ble.tab.dev[t].d))) continue;
        s_ble.shown[s_ble.n++] = t;
    }
}

static void kind_text(char *out, size_t n, const sentinel_bdv_t *d, bool longer)
{
    if (d->kind == SENTINEL_BLE_UNKNOWN) snprintf(out, n, "%s", longer ? "NOT RECOGNISED" : "-");
    else snprintf(out, n, "%s", longer ? sentinel_ble_kind_label(d->kind) : sentinel_ble_kind_word(d->kind));
    sn_upper(out);
}

static void detail_render(void)
{
    int t = s_ble.have_sel ? find(s_ble.sel) : -1;
    if (t < 0) {
        sn_set(s_ble.state, g_pal.label, "NO LONGER HEARD");
        for (int i = 0; i < 5; i++) sn_set(s_ble.v[i], g_pal.label, " ");
        sn_action_enable(mine_toggle, false);
        return;
    }
    const sentinel_bletab_dev_t *e = &s_ble.tab.dev[t];
    const sentinel_bdv_t        *d = &e->d;
    bool        mine = sentinel_ble_mine(d->addr);
    char        addr[18], kind[32], span[24];
    sentinel_ble_addr_text(addr, d->addr);
    kind_text(kind, sizeof(kind), d, true);
    sn_set(s_ble.state, lv_color_white(), "%s%s", kind, mine ? " (MINE)" : "");

    sentinel_track_t tr;
    if (mine) {
        sn_set(s_ble.v[0], g_pal.ok, "Marked as yours: it never raises a warning.");
    } else if (sentinel_ble_track(d->addr, &tr)) {
        sn_fmt_span(span, sizeof(span), sentinel_now_ms() - tr.first_ms);
        if (sentinel_ble_is_surveillance(d->kind)) sn_set(s_ble.v[0], g_pal.warn, "! A surveillance device, in earshot for %s.", span);
        else if (tr.has_pos) sn_set(s_ble.v[0], tr.alerted ? g_pal.warn : g_pal.text, "%sWith you for %s while you moved %u m.", tr.alerted ? "! " : "", span, (unsigned)tr.moved_m);
        else sn_set(s_ble.v[0], tr.alerted ? g_pal.warn : g_pal.text, "%sWith you for %s. No GPS fix, so distance is unknown.", tr.alerted ? "! " : "", span);
    } else if (d->kind == SENTINEL_BLE_FINDMY) {
        sn_set(s_ble.v[0], g_pal.ok, "Its owner is near it, so it is not tracking you.");
    } else {
        sn_set(s_ble.v[0], g_pal.label, "Nothing unusual about it.");
    }
    const char *vendor = sentinel_ble_company(d->company);
    sn_set(s_ble.v[1], g_pal.text, "SIGNAL   %d dBm", d->rssi);
    if (vendor) sn_set(s_ble.v[2], g_pal.text, "MAKER    %s", vendor);
    else if (d->company) sn_set(s_ble.v[2], g_pal.text, "MAKER    NOT KNOWN (0x%04X)", d->company);
    else sn_set(s_ble.v[2], g_pal.text, "MAKER    NOT GIVEN");
    sn_set(s_ble.v[3], g_pal.text, "NAME     %s", d->name[0] ? d->name : "NOT GIVEN");
    sn_set(s_ble.v[4], g_pal.text, "ADDRESS  %s (%s)", addr, d->pub ? "FIXED" : "CHANGES OVER TIME");
    sn_fmt_span(span, sizeof(span), sentinel_now_ms() - e->first_ms);
    sn_set(s_ble.v[5], g_pal.label, "Heard for %s, %u advertisements.", span, d->count);
    sn_action_enable(mine_toggle, true);
    sn_action_label(mine_toggle, mine ? "NOT MINE AFTER ALL" : "THIS ONE IS MINE");
}

static void list_render(void)
{
    int sel = s_ble.list.sel;
    for (int i = 0; s_ble.have_sel && i < s_ble.n; i++)
        if (memcmp(s_ble.tab.dev[s_ble.shown[i]].d.addr, s_ble.sel, 6) == 0) sel = i;
    sn_list_set(&s_ble.list, s_ble.n, sel,
                sn_board_problem(&s_ble.st) ? "WAITING FOR THE RADIO BOARD" : s_ble.trackers_only ? "NO TRACKERS OR SURVEILLANCE DEVICES HEARD" : "LISTENING...");
    if (s_ble.n) {
        memcpy(s_ble.sel, s_ble.tab.dev[s_ble.shown[s_ble.list.sel]].d.addr, 6);
        s_ble.have_sel = true;
    }
    int trackers = 0, cameras = 0;
    for (int i = 0; i < s_ble.tab.n; i++) {
        if (sentinel_ble_is_tracker(s_ble.tab.dev[i].d.kind)) trackers++;
        if (sentinel_ble_is_surveillance(s_ble.tab.dev[i].d.kind)) cameras++;
    }
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        int k = sn_list_item(&s_ble.list, r);
        if (k < 0) continue;
        const sentinel_bdv_t *d    = &s_ble.tab.dev[s_ble.shown[k]].d;
        sn_level_t            lvl  = dev_level(d);
        bool                  mine = sentinel_ble_mine(d->addr);
        char                  kind[14];
        kind_text(kind, sizeof(kind), d, false);
        const char *vendor = sentinel_ble_company(d->company);
        sn_set(sn_list_text(&s_ble.list, r), lvl == SN_NOTE ? g_pal.text : sn_level_color(lvl), "%-2s %4d dBm   %-10s %-10s %s%s", sn_level_mark(lvl), d->rssi, kind,
               vendor ? vendor : "", d->name, mine ? " (MINE)" : "");
    }
    if (!s_ble.st.ble_ok && !sn_board_problem(&s_ble.st)) sn_set(s_ble.summary, g_pal.warn, "THIS BOARD'S FIRMWARE HAS NO BLUETOOTH (NEEDS 0.4.0).");
    else if (s_ble.trackers_only) sn_set(s_ble.summary, g_pal.label, "%d TRACKER%s AND %d SURVEILLANCE DEVICE%s IN EARSHOT.", trackers, trackers == 1 ? "" : "S", cameras, cameras == 1 ? "" : "S");
    else sn_set(s_ble.summary, g_pal.label, "%d DEVICES IN EARSHOT.   %d TRACKER%s, %d SURVEILLANCE.   ONES TO LOOK AT COME FIRST.", s_ble.tab.n, trackers, trackers == 1 ? "" : "S", cameras);
}

static void render(void)
{
    order_update();
    if (s_ble.detail) detail_render();
    else list_render();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_ble.st);
    sentinel_ble_table(&s_ble.tab);
    render();
}

static void mine_toggle(void)
{
    if (s_ble.have_sel) sentinel_ble_set_mine(s_ble.sel, !sentinel_ble_mine(s_ble.sel));
    render();
}

static void open_row(int index)
{
    if (index < 0 || index >= s_ble.n) return;
    memcpy(s_ble.sel, s_ble.tab.dev[s_ble.shown[index]].d.addr, 6);
    s_ble.have_sel = true;
    s_ble.detail   = true;
    lv_obj_t *body = sn_body_reset();
    memset(&s_ble.list, 0, sizeof(s_ble.list));
    s_ble.state = sn_state(body);
    s_ble.v[0]  = sn_value(body);
    sn_heading(body, "DETAILS");
    for (int i = 1; i < 5; i++) s_ble.v[i] = sn_value(body);
    s_ble.v[5] = sn_note(body);
    sn_action(HID_LETTER('M'), "M", "THIS ONE IS MINE", mine_toggle);
    sn_help("What kind of device this is comes from matching what it broadcasts against known patterns. A label is a "
            "match, not proof.\n\n"
            "THIS ONE IS MINE stops warnings about a tracker or tag you own. It can be undone here.\n\n"
            "An address that changes over time belongs to a phone, watch or similar: the same device will show up again "
            "later as a new row.");
    detail_render();
}

static void list_build(void)
{
    lv_obj_t *body = sn_body_reset();
    s_ble.detail   = false;
    s_ble.state    = NULL;
    s_ble.summary  = sn_note(body);
    lv_label_set_long_mode(s_ble.summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_ble.summary, lv_font_get_line_height(g_font.mono_m));
    sn_list_build(&s_ble.list, body, open_row);
    sn_help(s_ble.trackers_only
                ? "Trackers (AirTag, Tile, SmartTag and the like) and surveillance devices (licence plate cameras, body "
                  "cameras) that the board can hear now.\n\n"
                  "A tracker near you is normal in a city: other people carry them. ! marks one that has stayed with you "
                  "while you moved, which is what being tracked looks like, and any surveillance device.\n\n"
                  "Press Enter or tap a row for the device."
                : "Every Bluetooth LE device the board can hear. Devices worth a look come first, then the rest by signal.\n\n"
                  "! marks a surveillance device, or a tracker that has stayed with you while you moved.\n\n"
                  "Press Enter or tap a row for the device. The board only listens.");
    list_render();
}

static void build(bool trackers_only)
{
    memset(&s_ble, 0, sizeof(s_ble));
    s_ble.trackers_only = trackers_only;
    sentinel_tool("ble"); /* Bluetooth gets the radio full time while this screen is up */
    sentinel_snapshot(&s_ble.st);
    sentinel_ble_table(&s_ble.tab);
    order_update();
    list_build();
    s_ble.timer = lv_timer_create(tick, TICK_MS, NULL);
}

static void devices_build(lv_obj_t *body)
{
    (void)body;
    build(false);
}

static void trackers_build(lv_obj_t *body)
{
    (void)body;
    build(true);
}

static void destroy(void)
{
    if (s_ble.timer) lv_timer_delete(s_ble.timer);
    s_ble.timer = NULL;
    memset(&s_ble.list, 0, sizeof(s_ble.list));
    sentinel_tool("watch"); /* back to sharing the radio */
}

static bool back(void)
{
    if (!s_ble.detail) return false;
    list_build();
    return true;
}

static bool key(const deck_key_t *k)
{
    if (s_ble.detail || !sn_list_key(&s_ble.list, k)) return false;
    if (!s_ble.detail && s_ble.n) {
        memcpy(s_ble.sel, s_ble.tab.dev[s_ble.shown[s_ble.list.sel]].d.addr, 6);
        s_ble.have_sel = true;
        list_render();
    }
    return true;
}

static void menu_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)level;
    if (!st->ble_ok) snprintf(line, n, "NOT ON THIS BOARD'S FIRMWARE");
    else snprintf(line, n, "%u DEVICES IN EARSHOT\n%u TRACKER%s", st->ble_devices, st->ble_trackers, st->ble_trackers == 1 ? "" : "S");
}

static void trackers_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)level;
    snprintf(line, n, "%u IN EARSHOT NOW", st->ble_trackers);
}

static const sn_screen_t s_devices  = {.name = "DEVICES", .purpose = "Every Bluetooth device in earshot. Open one to see what it is.", .icon = ICON_WIRELESS, .flags = SN_BOARD, .build = devices_build, .destroy = destroy, .key = key, .back = back};
static const sn_screen_t s_trackers = {.name = "TRACKERS", .purpose = "Trackers and surveillance devices near you.", .icon = ICON_PIN, .flags = SN_BOARD, .build = trackers_build, .destroy = destroy, .key = key, .back = back, .status = trackers_status};

extern const sn_screen_t sn_ble_capture_screen;
static const sn_screen_t *const s_kids[] = {&s_devices, &s_trackers, &sn_ble_capture_screen};

const sn_screen_t sn_ble_menu = {
    .name    = "BLUETOOTH",
    .purpose = "The Bluetooth devices around you, and whether one is following you.",
    .icon    = ICON_WIRELESS,
    .kids    = s_kids,
    .nkids   = 3,
    .status  = menu_status,
};
