/*
 * TERMINAL module: saved SSH hosts and an xterm-compatible terminal.
 *
 * Host list: Enter / tap connects, Ctrl+N adds, Del removes.
 * Terminal: every key goes to the remote (Esc included). Alt+Esc returns to
 * the deck and closes the session. Shift+Up/Down scrolls back; dragging on
 * the screen does too. The touch bar supplies Esc, Tab, sticky Ctrl,
 * arrows, PgUp/PgDn, Home/End and F1..F12.
 */
#include "app_term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_saver.h"
#include "deck_shell.h"
#include "deck_term.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "term_link.h"
#include "vterm_keycodes.h"

#define MAX_HOSTS 24
#define PUMP_MS 15
#define HID_N 0x11

typedef struct {
    char name[48];
    char user[32];
    char host[64];
    uint16_t port;
    bool local; /* simulator-only local shell */
} host_t;

static host_t s_hosts[MAX_HOSTS];
static int s_nhosts;

static struct {
    lv_obj_t *parent;
    lv_obj_t *screen;
    bool in_term;
    deck_term_t *term;
    lv_obj_t *info;
    lv_obj_t *bar;
    bool fn_page;
    lv_timer_t *pump;
    int sel;
    link_state_t shown_state;
    char target[112];
    char cols_rows[16];
    host_t pending;
} s_t;

static void show_hosts(void);

/* ---- Host store: <storage>/ssh/hosts.txt, one "name\tuser\thost\tport" per line */

static void hosts_path(char *out, size_t n)
{
    const char *root = hal_storage_root();
    snprintf(out, n, "%s/ssh/hosts.txt", root ? root : "");
}

static void hosts_load(void)
{
    s_nhosts = 0;
    if (link_has_local()) {
        host_t *h = &s_hosts[s_nhosts++];
        memset(h, 0, sizeof(*h));
        snprintf(h->name, sizeof(h->name), "LOCAL SHELL");
        snprintf(h->host, sizeof(h->host), "this machine");
        h->local = true;
    }
    char path[96];
    hosts_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL) return;
    char line[200];
    while (fgets(line, sizeof(line), f) && s_nhosts < MAX_HOSTS) {
        host_t h = {0};
        unsigned port = 22;
        if (sscanf(line, "%47[^\t]\t%31[^\t]\t%63[^\t]\t%u", h.name, h.user, h.host, &port) >= 3) {
            h.port               = (uint16_t)port;
            s_hosts[s_nhosts++] = h;
        }
    }
    fclose(f);
}

static void hosts_save(void)
{
    const char *root = hal_storage_root();
    if (root == NULL) return;
    char dir[96], path[96];
    snprintf(dir, sizeof(dir), "%s/ssh", root);
    mkdir(dir, 0755);
    hosts_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f == NULL) return;
    for (int i = 0; i < s_nhosts; i++) {
        if (s_hosts[i].local) continue;
        fprintf(f, "%s\t%s\t%s\t%u\n", s_hosts[i].name, s_hosts[i].user, s_hosts[i].host, s_hosts[i].port);
    }
    fclose(f);
}

/* "user@host[:port]" */
static bool parse_target(const char *s, host_t *h)
{
    memset(h, 0, sizeof(*h));
    const char *at = strchr(s, '@');
    if (at == NULL || at == s || !at[1]) return false;
    snprintf(h->user, sizeof(h->user), "%.*s", (int)(at - s), s);
    const char *colon = strrchr(at + 1, ':');
    size_t hl         = colon ? (size_t)(colon - at - 1) : strlen(at + 1);
    snprintf(h->host, sizeof(h->host), "%.*s", (int)hl, at + 1);
    h->port = colon ? (uint16_t)atoi(colon + 1) : 22;
    if (h->port == 0 || h->host[0] == '\0') return false;
    snprintf(h->name, sizeof(h->name), "%.47s", h->host);
    return true;
}

/* ---- Page helper --------------------------------------------------------- */

static lv_obj_t *page(void)
{
    if (s_t.screen) {
        lv_obj_add_flag(s_t.screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_t.screen);
    }
    lv_group_remove_all_objs(deck_input_group());
    s_t.screen = deck_box(s_t.parent);
    lv_obj_set_size(s_t.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_t.screen, LV_FLEX_FLOW_COLUMN);
    return s_t.screen;
}

/* ---- Terminal view ------------------------------------------------------- */

static void term_output(const char *d, size_t n, void *u)
{
    (void)u;
    link_write(d, n);
}

static void term_resized(int cols, int rows, void *u)
{
    (void)u;
    snprintf(s_t.cols_rows, sizeof(s_t.cols_rows), "%dx%d", cols, rows);
    link_resize(cols, rows);
}

static void update_info(void)
{
    link_state_t st = link_state();
    static const char *names[] = {"IDLE", "CONNECTING", "VERIFY HOST", "AUTH", "ONLINE", "CLOSED"};
    lv_label_set_text_fmt(s_t.info, "%s  //  %s  //  %s   %s", s_t.target, names[st], s_t.cols_rows,
                          st == LINK_OPEN ? "" : link_status());
    lv_obj_set_style_text_color(s_t.info, st == LINK_OPEN ? g_pal.accent : st == LINK_CLOSED ? g_pal.danger : g_pal.warn,
                                0);
}

static void hostkey_decided(bool yes, void *u)
{
    (void)u;
    link_hostkey_decide(yes);
}

static void pump(lv_timer_t *tm)
{
    (void)tm;
    static char buf[4096];
    size_t total = 0, n;
    while (total < 64 * 1024 && (n = link_read(buf, sizeof(buf))) > 0) {
        deck_term_feed(s_t.term, buf, n);
        total += n;
    }
    if (total) deck_saver_poke(); /* a session with output keeps the screen on */
    link_state_t st = link_state();
    if (st != s_t.shown_state) {
        s_t.shown_state = st;
        update_info();
        if (st == LINK_VERIFY_HOST) {
            char msg[200];
            snprintf(msg, sizeof(msg), "%s\n\n%s\n\nTrust this host?",
                     link_hostkey_changed() ? "WARNING: the host key CHANGED since last time. This can mean "
                                              "someone is intercepting the connection."
                                            : "First connection to this host. Check its fingerprint:",
                     link_fingerprint());
            if (link_hostkey_changed()) {
                deck_modal_confirm_danger("HOST KEY CHANGED", msg, "TRUST NEW KEY", hostkey_decided, NULL);
            } else {
                deck_modal_confirm("NEW HOST", msg, "TRUST", hostkey_decided, NULL);
            }
        } else if (st == LINK_CLOSED) {
            const char *m = "\r\n\x1b[1;31m[link closed]\x1b[0m press Enter to return\r\n";
            deck_term_feed(s_t.term, m, strlen(m));
        }
    }
}

static const char *s_bar_main[] = {"ESC", "TAB", "CTRL", "<", "UP", "DN", ">", "PGUP", "PGDN", "HOME", "END", "Fn",
                                   "QUIT", ""};
static const char *s_bar_fn[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Fn",
                                 ""};

static void set_bar_page(bool fn)
{
    s_t.fn_page = fn;
    lv_buttonmatrix_set_map(s_t.bar, fn ? s_bar_fn : s_bar_main);
    lv_buttonmatrix_clear_button_ctrl_all(s_t.bar, LV_BUTTONMATRIX_CTRL_CHECKED);
    if (!fn) lv_buttonmatrix_set_button_ctrl(s_t.bar, 2, LV_BUTTONMATRIX_CTRL_CHECKABLE);
}

static void close_term(void)
{
    link_close();
    if (s_t.pump) lv_timer_delete(s_t.pump);
    s_t.pump = NULL;
    deck_term_destroy(s_t.term);
    s_t.term    = NULL;
    s_t.in_term = false;
    show_hosts();
}

static void bar_clicked(lv_event_t *e)
{
    lv_obj_t *m  = lv_event_get_target_obj(e);
    uint32_t id  = lv_buttonmatrix_get_selected_button(m);
    const char *l = id == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(m, id);
    if (l == NULL) return;
    if (!strcmp(l, "Fn")) {
        set_bar_page(!s_t.fn_page);
    } else if (!strcmp(l, "QUIT")) {
        close_term();
    } else if (!strcmp(l, "CTRL")) {
        deck_term_set_sticky_ctrl(s_t.term, lv_buttonmatrix_has_button_ctrl(m, id, LV_BUTTONMATRIX_CTRL_CHECKED));
    } else if (l[0] == 'F' && l[1] >= '1' && l[1] <= '9') {
        deck_term_send_vkey(s_t.term, VTERM_KEY_FUNCTION(atoi(l + 1)), 0);
    } else {
        static const struct {
            const char *l;
            int k;
        } map[] = {{"ESC", VTERM_KEY_ESCAPE}, {"TAB", VTERM_KEY_TAB},     {"<", VTERM_KEY_LEFT},
                   {"UP", VTERM_KEY_UP},      {"DN", VTERM_KEY_DOWN},     {">", VTERM_KEY_RIGHT},
                   {"PGUP", VTERM_KEY_PAGEUP}, {"PGDN", VTERM_KEY_PAGEDOWN}, {"HOME", VTERM_KEY_HOME},
                   {"END", VTERM_KEY_END}};
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (!strcmp(l, map[i].l)) deck_term_send_vkey(s_t.term, map[i].k, 0);
        }
    }
}

static void open_term(const host_t *h, const char *password)
{
    s_t.in_term = true;
    lv_obj_t *root = page();
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, 0, 0);

    s_t.info = deck_label(root, g_font.mono_s, g_pal.warn, "");
    lv_obj_set_width(s_t.info, LV_PCT(100));
    lv_obj_set_style_pad_hor(s_t.info, 12, 0);
    lv_obj_set_style_pad_ver(s_t.info, 4, 0);
    lv_obj_set_style_bg_color(s_t.info, g_pal.panel, 0);
    lv_obj_set_style_bg_opa(s_t.info, LV_OPA_COVER, 0);

    lv_obj_t *area = deck_box(root);
    lv_obj_set_width(area, LV_PCT(100));
    lv_obj_set_flex_grow(area, 1);

    if (h->local) {
        snprintf(s_t.target, sizeof(s_t.target), "LOCAL SHELL");
    } else {
        snprintf(s_t.target, sizeof(s_t.target), "%s@%s:%u", h->user, h->host, h->port);
    }
    deck_term_cfg_t cfg = {.output = term_output, .resized = term_resized};
    s_t.term            = deck_term_create(area, &cfg);

    s_t.bar = lv_buttonmatrix_create(root);
    lv_obj_set_size(s_t.bar, LV_PCT(100), 54);
    lv_obj_set_style_bg_color(s_t.bar, g_pal.panel, 0);
    lv_obj_set_style_bg_opa(s_t.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_t.bar, 0, 0);
    lv_obj_set_style_radius(s_t.bar, 0, 0);
    lv_obj_set_style_pad_all(s_t.bar, 6, 0);
    lv_obj_set_style_pad_gap(s_t.bar, 6, 0);
    lv_obj_set_style_bg_color(s_t.bar, g_pal.panel_hi, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_t.bar, g_pal.line, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_t.bar, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_t.bar, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_t.bar, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_t.bar, g_font.mono_m, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_t.bar, g_pal.text, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_t.bar, g_pal.accent, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_t.bar, g_pal.bg, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_t.bar, g_pal.accent2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_t.bar, g_pal.bg, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_t.bar, bar_clicked, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_remove_obj(s_t.bar);
    set_bar_page(false);

    int cols, rows;
    deck_term_size(s_t.term, &cols, &rows);
    snprintf(s_t.cols_rows, sizeof(s_t.cols_rows), "%dx%d", cols, rows);

    link_params_t p = {0};
    snprintf(p.host, sizeof(p.host), "%s", h->host);
    snprintf(p.user, sizeof(p.user), "%s", h->user);
    snprintf(p.password, sizeof(p.password), "%s", password ? password : "");
    p.port          = h->port;
    s_t.shown_state = (link_state_t)-1;
    link_open(&p, cols, rows);
    /* link_open keeps its own copy; don't leave the password on the stack.
     * Called through a volatile pointer so the dead store is not dropped. */
    static void *(*const volatile wipe)(void *, int, size_t) = memset;
    wipe(&p, 0, sizeof(p));
    s_t.pump = lv_timer_create(pump, PUMP_MS, NULL);
    pump(s_t.pump);
}

/* ---- Host list ----------------------------------------------------------- */

static void password_entered(const char *text, void *u)
{
    (void)u;
    if (text == NULL) return; /* cancelled */
    open_term(&s_t.pending, text);
}

static void connect_host(int i)
{
    if (i < 0 || i >= s_nhosts) return;
    s_t.pending = s_hosts[i];
    if (s_hosts[i].local) {
        open_term(&s_t.pending, NULL);
        return;
    }
    char title[96];
    snprintf(title, sizeof(title), "%s@%s (EMPTY = DEVICE KEY)", s_hosts[i].user, s_hosts[i].host);
    deck_modal_password(title, password_entered, NULL);
}

static void target_entered(const char *text, void *u)
{
    (void)u;
    host_t h;
    if (text == NULL || !parse_target(text, &h) || s_nhosts >= MAX_HOSTS) return;
    s_hosts[s_nhosts++] = h;
    hosts_save();
    show_hosts();
}

static void delete_confirmed(bool yes, void *u)
{
    int i = (int)(intptr_t)u;
    if (yes && i >= 0 && i < s_nhosts && !s_hosts[i].local) {
        memmove(&s_hosts[i], &s_hosts[i + 1], sizeof(host_t) * (size_t)(s_nhosts - i - 1));
        s_nhosts--;
        hosts_save();
    }
    show_hosts();
}

static void row_clicked(lv_event_t *e) { connect_host((int)(intptr_t)lv_event_get_user_data(e)); }
static void new_clicked(lv_event_t *e)
{
    (void)e;
    deck_modal_prompt("NEW HOST  user@host[:port]", "", target_entered, NULL);
}
static void key_clicked(lv_event_t *e)
{
    (void)e;
    deck_modal_confirm("DEVICE KEY", link_pubkey(), "OK", NULL, NULL);
}

static lv_obj_t *tool_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = deck_button(parent, text, g_font.disp_s);
    lv_obj_set_style_min_height(b, 46, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_group_remove_obj(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void show_hosts(void)
{
    hosts_load();
    lv_obj_t *root = page();
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_row(root, 14, 0);

    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, "// HOSTS");
    lv_obj_set_flex_grow(t, 1);
    tool_button(head, "DEVICE KEY", key_clicked);
    tool_button(head, "+ NEW", new_clicked);

    lv_obj_t *list = deck_box(root);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (s_nhosts == 0) deck_label(list, g_font.mono_m, g_pal.dim, "No hosts yet. Ctrl+N or + NEW to add user@host.");
    lv_obj_t *focus = NULL;
    for (int i = 0; i < s_nhosts; i++) {
        lv_obj_t *row = deck_panel(list, DECK_CUT_BR, 12);
        lv_obj_set_size(row, LV_PCT(100), 68);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_pad_hor(row, 18, 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(deck_input_group(), row);
        lv_obj_t *n = deck_icon_text(row, ICON_CONSOLE, s_hosts[i].name, g_font.mono_l, g_pal.text);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        char where[112];
        if (s_hosts[i].local) {
            snprintf(where, sizeof(where), "simulator: runs your shell");
        } else {
            snprintf(where, sizeof(where), "%s@%s:%u", s_hosts[i].user, s_hosts[i].host, s_hosts[i].port);
        }
        lv_obj_t *w = deck_label(row, g_font.mono_s, g_pal.dim, where);
        lv_obj_align(w, LV_ALIGN_RIGHT_MID, 0, 0);
        if (i == s_t.sel || focus == NULL) focus = row;
    }
    deck_label(root, g_font.mono_s, g_pal.dim, "[ENTER] CONNECT    [CTRL+N] NEW    [DEL] REMOVE    [ESC] DECK");
    if (focus) {
        lv_group_focus_obj(focus);
        lv_obj_add_state(focus, LV_STATE_FOCUS_KEY);
    }
}

/* ---- Module -------------------------------------------------------------- */

static int focused_host(void)
{
    lv_obj_t *f = lv_group_get_focused(deck_input_group());
    intptr_t i  = f ? (intptr_t)lv_obj_get_user_data(f) : 0;
    return (int)i - 1;
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (s_t.in_term) {
        if (link_state() == LINK_CLOSED && k->code == HID_ENTER) {
            close_term();
            return true;
        }
        deck_term_key(s_t.term, k);
        return true; /* everything belongs to the remote; Alt+Esc is the way out */
    }
    if ((k->mods & DECK_MOD_CTRL) && k->code == HID_N) {
        new_clicked(NULL);
        return true;
    }
    if (k->code == HID_DELETE) {
        int i = focused_host();
        if (i >= 0 && !s_hosts[i].local) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Remove %s from the list?", s_hosts[i].name);
            deck_modal_confirm("REMOVE HOST", msg, "REMOVE", delete_confirmed, (void *)(intptr_t)i);
        }
        return true;
    }
    if (k->code == HID_ENTER) {
        int i = focused_host();
        if (i >= 0) {
            s_t.sel = i;
            connect_host(i);
            return true;
        }
    }
    return false;
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_t, 0, sizeof(s_t));
    s_t.parent = parent;
    show_hosts();
    return true;
}

static void term_exit(deck_app_t *self)
{
    (void)self;
    if (s_t.in_term) {
        link_close();
        if (s_t.pump) lv_timer_delete(s_t.pump);
        s_t.pump = NULL;
    }
}

static void stop(deck_app_t *self)
{
    (void)self;
    deck_term_destroy(s_t.term); /* widgets are gone; free emulator state */
    memset(&s_t, 0, sizeof(s_t));
}

static deck_app_t s_app = {
    .name     = "TERMINAL",
    .tagline  = "SSH UPLINK",
    .icon     = ICON_CONSOLE,
    .eta      = NULL,
    .on_start = start,
    .on_exit  = term_exit,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_term(void) { return &s_app; }
