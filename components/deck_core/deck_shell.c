#include "deck_shell.h"

#include "deck_boot.h"
#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_launcher.h"
#include "deck_modal.h"
#include "deck_statusbar.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TRANSITION_MS 260

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static deck_app_t *s_current;
static deck_app_t *s_next;   /* app to start at the midpoint of a transition */
static bool s_busy;          /* a transition is running */

/* ---- Building blocks ----------------------------------------------------- */

static void build_root(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, g_pal.bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_root = deck_box(scr);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);

    deck_statusbar_create(s_root);

    s_content = deck_box(s_root);
    lv_obj_set_width(s_content, LV_PCT(100));
    lv_obj_set_flex_grow(s_content, 1);
}

/* Delete whatever is on screen, then stop the app. The order matters:
 * removing focused widgets fires DEFOCUSED events into the app's handlers,
 * which must still find the app's state intact. */
static void teardown(void)
{
    deck_app_t *app = s_current;
    s_current       = NULL;
    if (app && app->on_exit) app->on_exit(app);
    deck_modal_discard();
    lv_group_remove_all_objs(deck_input_group());
    lv_obj_clean(s_content);
    deck_input_reset();
    if (app && app->on_stop) app->on_stop(app);
}

static void show_home_now(void)
{
    teardown();
    deck_launcher_create(s_content);
    deck_statusbar_set_app(NULL);
}

static void start_app_now(deck_app_t *app)
{
    teardown();
    if (!app->on_start(app, s_content)) {
        LV_LOG_ERROR("module %s failed to start", app->name);
        lv_group_remove_all_objs(deck_input_group());
        lv_obj_clean(s_content);
        deck_launcher_create(s_content);
        deck_statusbar_set_app(NULL);
        return;
    }
    s_current = app;
    deck_statusbar_set_app(app);
}

/* ---- Transitions --------------------------------------------------------- */

static void transition_mid(void *u)
{
    (void)u;
    if (s_next) {
        start_app_now(s_next);
    } else {
        show_home_now();
    }
}

static void transition_done(void *u)
{
    (void)u;
    s_busy = false;
}

void deck_shell_launch(deck_app_t *app)
{
    if (app == NULL || s_busy || app == s_current) return;
    s_busy = true;
    s_next = app;
    deck_fx_glitch(TRANSITION_MS, transition_mid, transition_done, NULL);
}

void deck_shell_home(void)
{
    if (s_busy || s_current == NULL) return;
    s_busy = true;
    s_next = NULL;
    deck_fx_glitch(TRANSITION_MS, transition_mid, transition_done, NULL);
}

deck_app_t *deck_shell_current(void) { return s_current; }

/* Deferred: rebuild is usually requested from an event handler of a widget
 * that the rebuild deletes. */
static void rebuild_async(void *u)
{
    (void)u;
    deck_app_t *app = s_current;
    teardown();
    deck_statusbar_destroy();
    lv_obj_delete(s_root);
    deck_input_reset();
    build_root();
    if (app) {
        start_app_now(app);
    } else {
        show_home_now();
    }
}

void deck_shell_rebuild(void) { lv_async_call(rebuild_async, NULL); }

/* ---- Keys ---------------------------------------------------------------- */

bool deck_shell_key(const deck_key_t *k)
{
    if (deck_boot_active()) {
        deck_boot_skip();
        return true;
    }
    if (s_busy) return true; /* swallow keys mid-transition */

    if (deck_modal_active()) {
        if (k->code == HID_ESC) {
            deck_modal_cancel();
            return true;
        }
        return (k->mods & DECK_MOD_ALT) != 0; /* no module switching under a dialog */
    }

    if (k->mods & DECK_MOD_ALT) {
        if (k->code == HID_ESC) {
            deck_shell_home();
            return true;
        }
        if (k->code >= HID_1 && k->code <= HID_9) {
            deck_app_t *app = deck_app_get((size_t)(k->code - HID_1));
            if (app) deck_shell_launch(app);
            return true;
        }
    }

    if (s_current == NULL) {
        return deck_launcher_key(k);
    }
    if (s_current->on_key && s_current->on_key(s_current, k)) {
        return true;
    }
    if (k->code == HID_ESC) {
        deck_shell_home();
        return true;
    }
    return false;
}

/* ---- Start --------------------------------------------------------------- */

void deck_shell_start(bool boot_anim)
{
    deck_grid_bg(lv_screen_active());
    build_root();
    show_home_now();
    deck_fx_scanlines(hal_cfg_get_i32("scan", 1) != 0);
    if (boot_anim) deck_boot_run();
}
