/*
 * Inside SENTINEL: the frame every screen shares, and what a screen is.
 *
 * A screen is either a menu (a grid of tiles, one per child) or a leaf that
 * does one job. The frame (sn_frame.c) draws the same four things round
 * every one of them, top to bottom: where you are and why, the alert line,
 * the body, and the action bar. A leaf fills the body and names its actions;
 * the frame draws each action as a button, and the button's key and a tap on
 * it are the same thing. Nothing else on a screen looks like a button.
 *
 * See docs/superpowers/specs/2026-11-13-sentinel-restructure-design.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "deck_app.h"
#include "deck_input.h"
#include "deck_sentinel.h"
#include "lvgl.h"

/* ---- Modes and levels ----------------------------------------------------- */

/* DEFENSIVE listens. OFFENSIVE also holds the node's arm lease, which is
 * what anything that transmits checks. The mode is OFFENSIVE exactly while
 * a screen under the OFFENSIVE menu is open. */
typedef enum { SN_DEFENSIVE, SN_OFFENSIVE } sn_mode_t;
sn_mode_t sn_mode(void);

/* What an event deserves. NOTE is written down and no more; WARNING is
 * something to look at; ALERT is happening now. */
typedef enum { SN_NOTE, SN_WARNING, SN_ALERT } sn_level_t;
sn_level_t  sn_level_of(bool warn, const char *text);
lv_color_t  sn_level_color(sn_level_t level);
const char *sn_level_mark(sn_level_t level); /* "", "!", "!!": a level is never colour alone */

/* ---- Screens -------------------------------------------------------------- */

#define SN_BOARD     0x01 /* needs the radio board linked: the frame says so instead of building it */
#define SN_OFFENSE   0x02 /* the OFFENSIVE menu: entering asks first, being under it is the mode */
#define SN_KEEP_TOOL 0x04 /* the board going away does not replace it with the blocked page: the leaf shows the problem itself, and is built without the board too */

typedef void (*sn_fn_t)(void); /* an action, a continuation */
typedef struct sn_screen sn_screen_t;
struct sn_screen {
    const char *name;    /* "NETWORKS" */
    const char *purpose; /* one sentence: why anyone is on this screen */
    const char *quote;   /* what its tile says under the name: a line from a film, for fun (no quotation marks: a mark can wrap onto a line of its own) */
    const char *icon;    /* for its tile */
    uint8_t     flags;
    /* A menu has children and nothing else. */
    const sn_screen_t *const *kids;
    int                       nkids;
    /* A leaf. `body` is empty, a column, as wide as the screen. */
    void (*build)(lv_obj_t *body);
    void (*destroy)(void);                /* timers, and whatever the screen asked of the node */
    bool (*key)(const deck_key_t *k);     /* keys no action took */
    bool (*back)(void);                   /* true: the screen used BACK itself (a detail page closing) */
    /* The frame is about to leave this leaf by a route that is not its own
     * BACK (the alert line opening LOG). Return true to keep the screen up;
     * call go() once the user has agreed, or never. Optional. */
    bool (*hold)(sn_fn_t go);
    /* The live line on this screen's tile, and its level. Optional. */
    void (*status)(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level);
};

void sn_open(const sn_screen_t *screen); /* push */
void sn_back(void);                      /* pop; from the home page, leave SENTINEL */
void sn_refresh(void);                   /* build the screen on top again (after a detail page, a mode change) */

/* ---- What a leaf uses ------------------------------------------------------ */

/* An action: a button in the bar. `hid` is its key (0 for touch only),
 * `key` what the cap on the button says ("S", "ENTER"). Call these from
 * build(); BACK is always there already. */
void sn_action(uint8_t hid, const char *key, const char *label, sn_fn_t fn);
void sn_action_label(sn_fn_t fn, const char *label); /* the same action says something else now */
void sn_action_enable(sn_fn_t fn, bool on);
void sn_action_danger(sn_fn_t fn, bool on);          /* drawn in the danger colour: it transmits, or it ends something */
/* A HELP button that shows `text` (plain sentences; "\n\n" between paragraphs). */
void sn_help(const char *text);

/* The body again, empty, with the actions gone: for a leaf that swaps
 * between a list and one row's detail. */
lv_obj_t *sn_body_reset(void);

/* Text. Nothing on a SENTINEL screen is smaller than 20 px. */
lv_obj_t *sn_heading(lv_obj_t *parent, const char *text); /* a caption with a rule: plain, not a panel */
lv_obj_t *sn_state(lv_obj_t *parent);                     /* the screen's one state word, 36 px */
lv_obj_t *sn_value(lv_obj_t *parent);                     /* 24 px, primary: what the user reads to decide */
lv_obj_t *sn_note(lv_obj_t *parent);                      /* 20 px, secondary, wraps */
lv_obj_t *sn_col(lv_obj_t *parent, int32_t width);        /* a plain column; width 0 grows */
lv_obj_t *sn_row_box(lv_obj_t *parent);                   /* a plain row, full width */
void      sn_set(lv_obj_t *label, lv_color_t color, const char *fmt, ...) LV_FORMAT_ATTRIBUTE(3, 4);

/* A big button inside a body (WARDRIVE's START, a toggle). Same look as the bar's. */
lv_obj_t *sn_button(lv_obj_t *parent, const char *key, const char *label, sn_fn_t fn);
void      sn_button_label(lv_obj_t *b, const char *label);
void      sn_button_danger(lv_obj_t *b, bool on); /* the danger colour: it ends something */

/* A list of rows that can each be opened: Up and Down move, Enter or a tap
 * opens. The caller fills the labels on every render. */
#define SN_LIST_ROWS 7
typedef struct {
    lv_obj_t *box, *row[SN_LIST_ROWS], *text[SN_LIST_ROWS], *more;
    int       n, sel, top;
    void (*open)(int index);
} sn_list_t;
void sn_list_build(sn_list_t *l, lv_obj_t *parent, void (*open)(int index));
/* Tell it how many items there are and which is chosen; then for each row
 * 0..SN_LIST_ROWS-1, sn_list_item() gives the item it shows (-1 for none)
 * and sn_list_text() the label to fill. `empty` is what an empty list says. */
void      sn_list_set(sn_list_t *l, int n, int sel, const char *empty);
int       sn_list_item(const sn_list_t *l, int row);
lv_obj_t *sn_list_text(const sn_list_t *l, int row);
bool      sn_list_key(sn_list_t *l, const deck_key_t *k); /* true: the selection moved or a row opened */

/* Why the radio board cannot be used, or NULL when it is linked. */
const char *sn_board_problem(const sentinel_state_t *st);

/* Small things several screens want. */
void sn_fmt_span(char *out, size_t n, uint32_t ms); /* "40 S", "12 MIN", "2 H 5 MIN" */
void sn_upper(char *s);
/* Ask before doing something that loses or ends something. CANCEL has the focus. */
void sn_confirm(const char *title, const char *message, const char *yes_label, sn_fn_t yes);
/* The same, with something to do on CANCEL too (going back out of a screen that was opened by mistake). */
void sn_confirm_or(const char *title, const char *message, const char *yes_label, sn_fn_t yes, sn_fn_t no);

/* ---- The module's side (app_sentinel.c) --------------------------------------- */

/* `log` NULL: no board behind the frame (NET TOOLS), so no mode chip and no alert line. */
void sn_frame_start(lv_obj_t *parent, const sn_screen_t *home, const sn_screen_t *log);
void sn_frame_exit(const char *why); /* leaving SENTINEL by any route: back to DEFENSIVE */
void sn_frame_stop(void);
bool sn_frame_key(const deck_key_t *k);
bool sn_frame_back(void);            /* false at the home page */
void sn_frame_offense_slept(void);
/* One line on the alert line for a while, from the deck itself. */
void sn_say(sn_level_t level, const char *text, uint32_t ms);

/* ---- The screens (one file per area) --------------------------------------- */

extern const sn_screen_t sn_wifi_menu, sn_ble_menu, sn_spectrum_menu, sn_net_menu, sn_board_menu, sn_offense_menu;
extern const sn_screen_t sn_drive_screen, sn_log_screen;
/* NETWORKS (it works without the board), and the NET TOOLS app's home page. */
extern const sn_screen_t sn_networks_screen, sn_net_home;

/* The radio board's power: a setting (BOARD > RADIO BOARD), not the app's. */
bool sn_board_enabled(void);
void sn_board_set_enabled(bool on);
