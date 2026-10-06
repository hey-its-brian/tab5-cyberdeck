#include "deck_modal.h"

#include <stdlib.h>
#include <string.h>

#include "deck_hal.h"
#include "deck_input.h"
#include "deck_theme.h"
#include "deck_widgets.h"

typedef enum { MODAL_NONE, MODAL_PROMPT, MODAL_CONFIRM, MODAL_LIST } modal_kind_t;

static struct {
    modal_kind_t kind;
    lv_obj_t *root;
    lv_obj_t *ta;
    lv_group_t *group;
    lv_group_t *prev_default;
    deck_prompt_cb_t prompt_cb;
    deck_confirm_cb_t confirm_cb;
    deck_list_cb_t list_cb;
    int list_choice;
    lv_obj_t *first_row;
    lv_obj_t *cancel_btn;
    void *user;
    bool danger;     /* CANCEL focused, Enter ignored for DANGER_ARM_MS */
    uint32_t opened; /* lv_tick at open */
} s_m;

#define DANGER_ARM_MS 1000

bool deck_modal_active(void) { return s_m.kind != MODAL_NONE; }

/* Tear the dialog down and hand back the callback to run. Deletion is
 * deferred because we are usually inside an event of one of its buttons. */
static void group_delete_async(void *g) { lv_group_delete((lv_group_t *)g); }

static void close_modal(void)
{
    if (s_m.root) lv_obj_delete_async(s_m.root);
    if (s_m.group) {
        deck_input_pop_group();
        lv_async_call(group_delete_async, s_m.group);
    }
    s_m.root  = NULL;
    s_m.ta    = NULL;
    s_m.group = NULL;
    s_m.kind  = MODAL_NONE;
}

static void finish(bool ok)
{
    modal_kind_t kind      = s_m.kind;
    deck_prompt_cb_t pcb   = s_m.prompt_cb;
    deck_confirm_cb_t ccb  = s_m.confirm_cb;
    deck_list_cb_t lcb     = s_m.list_cb;
    int choice             = ok ? s_m.list_choice : -1;
    void *user             = s_m.user;
    char *text             = NULL;
    if (kind == MODAL_PROMPT && ok && s_m.ta) text = strdup(lv_textarea_get_text(s_m.ta));
    close_modal();

    if (kind == MODAL_PROMPT && pcb) pcb(text, user);
    if (kind == MODAL_CONFIRM && ccb) ccb(ok, user);
    if (kind == MODAL_LIST && lcb) lcb(choice, user);
    free(text);
}

void deck_modal_enter(void)
{
    if (!deck_modal_active()) return;
    /* Type-ahead or a held Enter must not accept a dangerous dialog. */
    if (s_m.danger && lv_tick_elaps(s_m.opened) < DANGER_ARM_MS) return;
    lv_obj_t *f = lv_group_get_focused(s_m.group);
    if (f != NULL && f == s_m.cancel_btn) {
        finish(false);
    } else if (s_m.kind == MODAL_LIST) {
        intptr_t i = f ? (intptr_t)lv_obj_get_user_data(f) : 0;
        if (i > 0) {
            s_m.list_choice = (int)(i - 1);
            finish(true);
        }
    } else {
        finish(true);
    }
}

void deck_modal_cancel(void)
{
    if (deck_modal_active()) finish(false);
}

void deck_modal_discard(void)
{
    if (deck_modal_active()) close_modal();
}

static void ok_clicked(lv_event_t *e)
{
    (void)e;
    finish(true);
}

static void cancel_clicked(lv_event_t *e)
{
    (void)e;
    finish(false);
}

/* Dimmed backdrop plus a centered panel; returns the panel. Widgets made
 * until end_build() join the modal's own focus group. */
static lv_obj_t *begin_build(modal_kind_t kind, const char *title, void *user)
{
    deck_modal_discard();
    memset(&s_m, 0, sizeof(s_m));
    s_m.kind  = kind;
    s_m.user  = user;
    s_m.group = lv_group_create();
    lv_group_set_wrap(s_m.group, true);
    s_m.prev_default = lv_group_get_default();
    lv_group_set_default(s_m.group);

    s_m.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_m.root);
    lv_obj_set_size(s_m.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_m.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_m.root, LV_OPA_70, 0);
    lv_obj_add_flag(s_m.root, LV_OBJ_FLAG_CLICKABLE); /* block taps to what is behind */

    lv_obj_t *p = deck_panel(s_m.root, DECK_CUT_TL | DECK_CUT_BR, 22);
    deck_panel_set_tab(p, true);
    deck_panel_set_outline(p, g_pal.accent);
    lv_obj_set_size(p, 720, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(p, 30, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 20, 0);
    deck_section(p, title);
    return p;
}

static void end_build(lv_obj_t *panel, const char *ok_label)
{
    lv_obj_t *btns = deck_box(panel);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 16, 0);
    lv_obj_t *cancel = deck_button(btns, "CANCEL", NULL);
    lv_obj_add_event_cb(cancel, cancel_clicked, LV_EVENT_CLICKED, NULL);
    s_m.cancel_btn = cancel;
    lv_obj_t *ok = cancel;
    if (ok_label) {
        ok = deck_button(btns, ok_label, NULL);
        lv_obj_add_event_cb(ok, ok_clicked, LV_EVENT_CLICKED, NULL);
    }

    /* No physical keyboard: give the prompt an on-screen one. */
    if (s_m.ta && !hal_kbd_present()) {
        lv_obj_t *kb = lv_keyboard_create(s_m.root);
        lv_obj_set_size(kb, LV_PCT(100), LV_PCT(42));
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(kb, s_m.ta);
        lv_obj_add_event_cb(kb, ok_clicked, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(kb, cancel_clicked, LV_EVENT_CANCEL, NULL);
        lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 40);
    } else {
        lv_obj_center(panel);
    }

    lv_group_set_default(s_m.prev_default);
    deck_input_push_group(s_m.group);
    lv_obj_t *first = s_m.ta ? s_m.ta : s_m.first_row ? s_m.first_row : s_m.danger ? cancel : ok;
    s_m.opened      = lv_tick_get();
    lv_group_focus_obj(first);
    lv_obj_add_state(first, LV_STATE_FOCUS_KEY); /* make Enter's target visible */
}

static void ta_ready(lv_event_t *e)
{
    (void)e;
    finish(true);
}

static void prompt_build(const char *title, const char *initial, bool password, deck_prompt_cb_t cb, void *user)
{
    lv_obj_t *p   = begin_build(MODAL_PROMPT, title, user);
    s_m.prompt_cb = cb;

    s_m.ta = lv_textarea_create(p);
    lv_textarea_set_one_line(s_m.ta, true);
    lv_textarea_set_text(s_m.ta, initial ? initial : "");
    lv_obj_set_width(s_m.ta, LV_PCT(100));
    lv_obj_set_style_text_font(s_m.ta, g_font.mono_l, 0);
    lv_obj_set_style_bg_color(s_m.ta, lv_color_hex(0x04050A), 0);
    lv_obj_set_style_border_color(s_m.ta, g_pal.accent, 0);
    lv_obj_set_style_text_color(s_m.ta, g_pal.text, 0);
    lv_obj_add_event_cb(s_m.ta, ta_ready, LV_EVENT_READY, NULL);
    if (password) lv_textarea_set_password_mode(s_m.ta, true);

    end_build(p, "OK");
}

void deck_modal_prompt(const char *title, const char *initial, deck_prompt_cb_t cb, void *user)
{
    prompt_build(title, initial, false, cb, user);
}

void deck_modal_password(const char *title, deck_prompt_cb_t cb, void *user)
{
    prompt_build(title, "", true, cb, user);
}

static void row_clicked(lv_event_t *e)
{
    s_m.list_choice = (int)(intptr_t)lv_event_get_user_data(e);
    finish(true);
}

void deck_modal_list(const char *title, const char *const *items, int count, deck_list_cb_t cb, void *user)
{
    lv_obj_t *p = begin_build(MODAL_LIST, title, user);
    s_m.list_cb = cb;

    lv_obj_t *list = deck_box(p);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_style_max_height(list, 380, 0);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_pad_all(list, 6, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int i = 0; i < count; i++) {
        lv_obj_t *row = deck_panel(list, DECK_CUT_BR, 10);
        lv_obj_set_size(row, LV_PCT(100), 52);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_user_data(row, (void *)(intptr_t)(i + 1));
        lv_group_add_obj(s_m.group, row);
        lv_obj_t *l = deck_label(row, g_font.mono_m, g_pal.text, items[i]);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        if (i == 0) s_m.first_row = row;
    }
    if (count == 0) deck_label(list, g_font.mono_m, g_pal.dim, "Nothing found.");

    end_build(p, NULL);
}

void deck_modal_confirm(const char *title, const char *message, const char *yes_label, deck_confirm_cb_t cb,
                        void *user)
{
    lv_obj_t *p    = begin_build(MODAL_CONFIRM, title, user);
    s_m.confirm_cb = cb;
    lv_obj_t *msg  = deck_label(p, g_font.mono_m, g_pal.text, message);
    lv_obj_set_width(msg, LV_PCT(100));
    end_build(p, yes_label ? yes_label : "OK");
}

void deck_modal_info(const char *title, const char *const *columns, int count)
{
    lv_obj_t *p    = begin_build(MODAL_CONFIRM, title, NULL);
    lv_obj_set_width(p, 1040);
    lv_obj_t *cols = deck_box(p);
    lv_obj_set_size(cols, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, 40, 0);
    for (int i = 0; i < count; i++) {
        lv_obj_t *l = deck_label(cols, g_font.mono_s, g_pal.text, columns[i]);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_style_text_line_space(l, 5, 0);
    }
    end_build(p, NULL); /* a single button: rename it, Enter and Esc both close */
    lv_label_set_text(lv_obj_get_child_by_type(s_m.cancel_btn, 0, &lv_label_class), "CLOSE");
}

void deck_modal_confirm_danger(const char *title, const char *message, const char *yes_label, deck_confirm_cb_t cb,
                               void *user)
{
    lv_obj_t *p    = begin_build(MODAL_CONFIRM, title, user);
    s_m.confirm_cb = cb;
    s_m.danger     = true;
    deck_panel_set_outline(p, g_pal.accent2);
    lv_obj_t *msg = deck_label(p, g_font.mono_m, g_pal.text, message);
    lv_obj_set_width(msg, LV_PCT(100));
    end_build(p, yes_label ? yes_label : "OK");
}
