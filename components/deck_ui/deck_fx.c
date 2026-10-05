#include "deck_fx.h"

#include <stdlib.h>
#include <string.h>

#include "deck_theme.h"

/* ---- Glitch transition --------------------------------------------------- */

#define GLITCH_BARS 9
#define GLITCH_TICK_MS 33

typedef struct {
    lv_obj_t *layer;
    lv_obj_t *bars[GLITCH_BARS];
    lv_timer_t *timer;
    uint32_t start;
    uint32_t ms;
    bool mid_fired;
    deck_fx_done_cb_t mid;
    deck_fx_done_cb_t done;
    void *user;
} glitch_t;

static glitch_t *s_glitch;

static void glitch_shuffle(glitch_t *g)
{
    static const uint32_t colors[] = {0x00F0FF, 0xFF2A6D, 0xF3E600, 0xFFFFFF};
    for (int i = 0; i < GLITCH_BARS; i++) {
        lv_obj_t *b = g->bars[i];
        int32_t h   = 4 + (int32_t)lv_rand(0, 38);
        int32_t w   = 200 + (int32_t)lv_rand(0, 1080);
        lv_obj_set_size(b, w, h);
        lv_obj_set_pos(b, (int32_t)lv_rand(0, 1280) - w / 3, (int32_t)lv_rand(0, 720));
        uint32_t c = (i % 3 == 0) ? lv_color_to_u32(g_pal.accent) & 0xFFFFFF : colors[lv_rand(0, 3)];
        lv_obj_set_style_bg_color(b, lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(b, (lv_opa_t)lv_rand(90, 230), 0);
    }
}

static void glitch_tick(lv_timer_t *t)
{
    glitch_t *g      = (glitch_t *)lv_timer_get_user_data(t);
    uint32_t elapsed = lv_tick_elaps(g->start);

    if (!g->mid_fired && elapsed >= g->ms / 2) {
        g->mid_fired = true;
        if (g->mid) g->mid(g->user);
    }
    if (elapsed >= g->ms) {
        lv_timer_delete(g->timer);
        lv_obj_delete(g->layer);
        deck_fx_done_cb_t done = g->done;
        void *user             = g->user;
        free(g);
        s_glitch = NULL;
        if (done) done(user);
        return;
    }
    glitch_shuffle(g);
}

void deck_fx_glitch(uint32_t ms, deck_fx_done_cb_t mid, deck_fx_done_cb_t done, void *user)
{
    if (s_glitch != NULL) {
        /* Already running (double tap): just run the callbacks now. */
        if (mid) mid(user);
        if (done) done(user);
        return;
    }
    glitch_t *g = (glitch_t *)calloc(1, sizeof(glitch_t));
    g->ms       = ms;
    g->mid      = mid;
    g->done     = done;
    g->user     = user;
    g->start    = lv_tick_get();

    g->layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g->layer);
    lv_obj_set_size(g->layer, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(g->layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g->layer, LV_OBJ_FLAG_CLICKABLE); /* swallow taps mid-transition */
    for (int i = 0; i < GLITCH_BARS; i++) {
        g->bars[i] = lv_obj_create(g->layer);
        lv_obj_remove_style_all(g->bars[i]);
    }
    glitch_shuffle(g);
    s_glitch = g;
    g->timer = lv_timer_create(glitch_tick, GLITCH_TICK_MS, g);
}

/* ---- Scanlines ----------------------------------------------------------- */

#define SCAN_STEP 3

static lv_obj_t *s_scan;

static void scan_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t *clip = &layer->_clip_area;

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_black();
    d.bg_opa   = LV_OPA_20;

    int32_t y = clip->y1 - (clip->y1 % SCAN_STEP);
    for (; y <= clip->y2; y += SCAN_STEP) {
        lv_area_t a = {clip->x1, y, clip->x2, y};
        lv_draw_rect(layer, &d, &a);
    }
}

void deck_fx_scanlines(bool on)
{
    if (on && s_scan == NULL) {
        s_scan = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_scan);
        lv_obj_set_size(s_scan, LV_PCT(100), LV_PCT(100));
        lv_obj_remove_flag(s_scan, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_scan, scan_draw, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_move_background(s_scan); /* keep glitch bars above it */
    } else if (!on && s_scan != NULL) {
        lv_obj_delete(s_scan);
        s_scan = NULL;
    }
}

/* ---- Typewriter ---------------------------------------------------------- */

typedef struct {
    lv_obj_t *label;
    lv_timer_t *timer;
    char *text;
    size_t len;
    size_t shown;
    bool idle;   /* fully typed; timer only blinks the cursor */
} typer_t;

#define TYPE_TICK_MS 20

static void typer_tick(lv_timer_t *t)
{
    typer_t *ty = (typer_t *)lv_timer_get_user_data(t);
    if (ty->shown < ty->len) {
        /* Advance whole UTF-8 sequences so multi-byte glyphs never split. */
        size_t n = ty->shown;
        do {
            n++;
        } while (n < ty->len && ((unsigned char)ty->text[n] & 0xC0) == 0x80);
        ty->shown = n;
    }

    bool cursor_on = (lv_tick_get() / 450) % 2 == 0 || ty->shown < ty->len;
    char saved     = ty->text[ty->shown];
    ty->text[ty->shown] = '\0';
    lv_label_set_text_fmt(ty->label, "%s%s", ty->text, cursor_on ? "_" : " ");
    ty->text[ty->shown] = saved;

    /* Once fully typed, slow down to just blink the cursor. */
    if (ty->shown >= ty->len && !ty->idle) {
        ty->idle = true;
        lv_timer_set_period(t, 450);
    }
}

static void typer_free(lv_event_t *e)
{
    typer_t *ty = (typer_t *)lv_event_get_user_data(e);
    lv_timer_delete(ty->timer);
    free(ty->text);
    free(ty);
}

void deck_fx_typewriter(lv_obj_t *label, const char *text, uint32_t cps)
{
    typer_t *ty = (typer_t *)calloc(1, sizeof(typer_t));
    ty->label   = label;
    ty->text    = strdup(text);
    ty->len     = strlen(text);
    uint32_t period = cps ? 1000 / cps : TYPE_TICK_MS;
    if (period < 5) period = 5;
    ty->timer = lv_timer_create(typer_tick, period, ty);
    lv_obj_add_event_cb(label, typer_free, LV_EVENT_DELETE, ty);
    lv_label_set_text(label, "");
}
