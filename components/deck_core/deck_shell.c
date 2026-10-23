#include "deck_shell.h"

#include "deck_boot.h"
#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_launcher.h"
#include "deck_audio.h"
#include "deck_modal.h"
#include "deck_saver.h"
#include "deck_statusbar.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TRANSITION_MS 260

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static deck_app_t *s_current;    /* running app, NULL on the home or a group screen */
static deck_group_t *s_group;    /* group screen shown, or the group the running app sits in */
static deck_app_t *s_next;       /* what to show at the midpoint of a transition */
static deck_group_t *s_next_group;
static bool s_busy;              /* a transition is running */

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

/* The home screen (group NULL) or a group screen. */
static void show_launcher_now(deck_group_t *group)
{
    teardown();
    s_group = group;
    deck_launcher_create(s_content, group);
    deck_statusbar_set_path(group, NULL);
}

static void start_app_now(deck_app_t *app)
{
    teardown();
    s_group = deck_app_group(app);
    if (!app->on_start(app, s_content)) {
        LV_LOG_ERROR("module %s failed to start", app->name);
        lv_group_remove_all_objs(deck_input_group());
        lv_obj_clean(s_content);
        deck_launcher_create(s_content, s_group);
        deck_statusbar_set_path(s_group, NULL);
        return;
    }
    s_current = app;
    deck_statusbar_set_path(s_group, app);
}

/* ---- Transitions --------------------------------------------------------- */

static void transition_mid(void *u)
{
    (void)u;
    if (s_next) {
        start_app_now(s_next);
    } else {
        show_launcher_now(s_next_group);
    }
}

static void transition_done(void *u)
{
    (void)u;
    s_busy = false;
}

static void go(deck_app_t *app, deck_group_t *group)
{
    s_busy       = true;
    s_next       = app;
    s_next_group = group;
    deck_fx_glitch(TRANSITION_MS, transition_mid, transition_done, NULL);
}

void deck_shell_launch(deck_app_t *app)
{
    if (app == NULL || s_busy || app == s_current) return;
    go(app, NULL);
}

void deck_shell_open_group(deck_group_t *group)
{
    if (group == NULL || s_busy || (s_current == NULL && s_group == group)) return;
    go(NULL, group);
}

void deck_shell_home(void)
{
    if (s_busy || (s_current == NULL && s_group == NULL)) return;
    go(NULL, NULL);
}

void deck_shell_back(void)
{
    if (s_busy) return;
    if (s_current) {
        go(NULL, s_group); /* the app's group, or home */
    } else if (s_group) {
        go(NULL, NULL);
    }
}

deck_app_t *deck_shell_current(void) { return s_current; }
deck_group_t *deck_shell_group(void) { return s_group; }

/* ---- Sleep and keyboard light -------------------------------------------- */

static lv_obj_t *s_sleep;

static void wake_async(void *u)
{
    (void)u;
    if (s_sleep) {
        lv_obj_delete(s_sleep);
        s_sleep = NULL;
    }
    hal_display_power(true);
}

static void sleep_pressed(lv_event_t *e)
{
    (void)e;
    lv_async_call(wake_async, NULL); /* not from inside the overlay's own event */
}

void deck_shell_sleep(void)
{
    if (s_sleep) return;
    /* A black, clickable layer above everything (modals included) so the
     * waking touch lands here instead of on a control. */
    s_sleep = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sleep);
    lv_obj_set_size(s_sleep, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sleep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sleep, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sleep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sleep, sleep_pressed, LV_EVENT_PRESSED, NULL);
    hal_display_power(false);
}

bool deck_shell_sleeping(void) { return s_sleep != NULL; }

static const uint8_t s_kbd_levels[3] = {0, 3, 20}; /* OFF, LOW, HIGH (20 = keyboard default) */

void deck_shell_apply_kbd_light(void)
{
    int level = (int)hal_cfg_get_i32("kbd_led", 1);
    if (level < 0 || level > 2) level = 1;
    bool theme   = hal_cfg_get_i32("kbd_theme", 0) != 0;
    uint32_t rgb = lv_color_to_u32(g_pal.accent) & 0xFFFFFF;
    hal_kbd_light(s_kbd_levels[level], theme, rgb);
}

/* ---- Shortcut list (Alt+H) ---------------------------------------------- */

void deck_shell_shortcuts(void)
{
    static const char *const cols[] = {
        "ANYWHERE\n"
        "  ALT+1..5    open a home tile\n"
        "  ALT+T N C W M F S   jump to a module\n"
        "  ALT+ESC     back to the deck\n"
        "  ALT+0       sleep the screen (or tap the clock)\n"
        "  ALT+H       this list\n"
        "  ALT+P       pause / resume music\n"
        "  ESC or DEL  back / close (DEL deletes inside text)\n"
        "\n"
        "HOME AND GROUPS\n"
        "  LEFT RIGHT ENTER, or 1..5   pick a tile\n"
        "  in a group: 1..n opens, ESC goes back\n"
        "\n"
        "TERMINAL\n"
        "  CTRL+N new SSH host  CTRL+O new Ollama server\n"
        "  SSH: every key goes to the server, ESC too\n"
        "  OLLAMA: CTRL+C stops a reply, /help commands\n"
        "  ALT+ESC leaves  SHIFT+UP/DOWN scrolls back",

        "NOTES\n"
        "  ENTER open  BACKSPACE up a folder  S sync (panel)\n"
        "  CTRL+S save  CTRL+P preview  CTRL+T split\n"
        "  ESC save and close\n"
        "\n"
        "CALC\n"
        "  ENTER evaluate  UP/DOWN recall\n"
        "  CTRL+D deg/rad  CTRL+K keypad  CTRL+L clear tape\n"
        "\n"
        "WEATHER\n"
        "  R refresh  L location  U units\n"
        "\n"
        "PLAYER\n"
        "  ENTER play  SPACE pause  LEFT/RIGHT seek\n"
        "  [ ] prev/next  - = volume  S shuffle  R repeat\n"
        "\n"
        "FILES\n"
        "  ENTER open  BACKSPACE up  DEL or D delete\n"
        "  P preview / portal  SPACE engage / disengage",
    };
    deck_modal_info("SHORTCUTS", cols, 2);
}

/* Deferred: rebuild is usually requested from an event handler of a widget
 * that the rebuild deletes. */
static void rebuild_async(void *u)
{
    (void)u;
    deck_app_t *app     = s_current;
    deck_group_t *group = s_group;
    teardown();
    deck_statusbar_destroy();
    lv_obj_delete(s_root);
    deck_input_reset();
    deck_shell_apply_kbd_light(); /* accent may have changed */
    build_root();
    if (app) {
        start_app_now(app);
    } else {
        show_launcher_now(group);
    }
}

void deck_shell_rebuild(void) { lv_async_call(rebuild_async, NULL); }

/* ---- Keys ---------------------------------------------------------------- */

/* Del doubles as a back key (it sits in the keyboard's top right corner,
 * easier to reach than Esc), except while a text field has focus, where it
 * keeps deleting. Modules that use Del themselves consume it first. */
static bool back_key(const deck_key_t *k)
{
    if (k->code == HID_ESC) return true;
    if (k->code != HID_DELETE || (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) return false;
    lv_obj_t *f = lv_group_get_focused(deck_input_group());
    return f == NULL || !lv_obj_check_type(f, &lv_textarea_class);
}

bool deck_shell_key(const deck_key_t *k)
{
    deck_saver_poke(); /* every key is activity, even ones the shell consumes */
    if (deck_saver_active()) {
        deck_saver_wake(); /* the waking key does nothing else */
        return true;
    }
    if (s_sleep) {
        wake_async(NULL); /* the waking key does nothing else */
        return true;
    }
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
        if (k->code == HID_ENTER) {
            deck_modal_enter();
            return true;
        }
        return (k->mods & DECK_MOD_ALT) != 0; /* no module switching under a dialog */
    }

    if (k->mods & DECK_MOD_ALT) {
        if (k->code == HID_ESC) {
            deck_shell_home();
            return true;
        }
        if (k->code == HID_0) {
            deck_shell_sleep();
            return true;
        }
        if (k->code == HID_H) {
            deck_shell_shortcuts();
            return true;
        }
        if (k->code == HID_P) {
            player_toggle(); /* music pause / resume from anywhere */
            return true;
        }
        if (k->code >= HID_1 && k->code <= HID_9) {
            const deck_tile_t *t = deck_home_tile((size_t)(k->code - HID_1));
            if (t && t->group) {
                deck_shell_open_group(t->group);
            } else if (t) {
                deck_shell_launch(t->app);
            }
            return true;
        }
        deck_app_t *app = deck_app_by_hotkey(k->code);
        if (app) {
            deck_shell_launch(app);
            return true;
        }
    }

    if (s_current == NULL) {
        if (deck_launcher_key(k)) return true;
        if (back_key(k) && s_group) {
            deck_shell_back();
            return true;
        }
        return false;
    }
    if (s_current->on_key && s_current->on_key(s_current, k)) {
        return true;
    }
    if (back_key(k)) {
        deck_shell_back();
        return true;
    }
    return false;
}

/* ---- Start --------------------------------------------------------------- */

void deck_shell_start(bool boot_anim)
{
    deck_grid_bg(lv_screen_active());
    build_root();
    show_launcher_now(NULL);
    deck_fx_scanlines(hal_cfg_get_i32("scan", 1) != 0);
    deck_shell_apply_kbd_light();
    if (boot_anim) deck_boot_run();
}
