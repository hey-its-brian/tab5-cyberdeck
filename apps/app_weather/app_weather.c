/*
 * WEATHER module: Open-Meteo (no API key) for a location picked by city
 * name. Current conditions, a 24 hour temperature / rain trace and a 7 day
 * strip. The last good response is cached on storage so the screen still
 * fills in offline, marked with its age.
 *
 * Keys: R refresh, L location, U switch units.
 */
#include "app_weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "weather_data.h"

#define REFRESH_S (15 * 60)
#define HID_L 0x0F
#define HID_R 0x15
#define HID_U 0x18

/* Session state: survives leaving and re-entering the module. */
static struct {
    wx_t wx;
    bool have;
    bool from_cache;
    time_t fetched;      /* wall clock of the data (cache file time if cached) */
    bool loading;
    bool failed;
} s_w;

/* UI handles, valid while the module is open. */
static struct {
    volatile bool alive; /* cleared on stop so late fetches are dropped */
    lv_obj_t *place;
    lv_obj_t *icon;
    lv_obj_t *temp;
    lv_obj_t *cond;
    lv_obj_t *range;
    lv_obj_t *kv[5];
    lv_obj_t *status;
    lv_obj_t *units_btn;
    lv_obj_t *chart;
    lv_chart_series_t *s_temp;
    lv_chart_series_t *s_pop;
    lv_obj_t *hour_lbl[8];
    lv_obj_t *day_name[WX_DAYS], *day_icon[WX_DAYS], *day_hi[WX_DAYS], *day_lo[WX_DAYS], *day_pop[WX_DAYS];
    lv_timer_t *timer;
    wx_place_t places[WX_PLACES];
    int nplaces;
} s_ui;

enum { KV_HUMID, KV_WIND, KV_PRESS, KV_SUNRISE, KV_SUNSET };

static void render(void);
static void refresh_now(void);

/* ---- Settings and cache -------------------------------------------------- */

static bool place(char *name, size_t n, double *lat, double *lon)
{
    char a[24], b[24];
    if (!hal_cfg_get_str("wx_name", name, n) || !hal_cfg_get_str("wx_lat", a, sizeof(a)) ||
        !hal_cfg_get_str("wx_lon", b, sizeof(b))) {
        return false;
    }
    *lat = atof(a);
    *lon = atof(b);
    return true;
}

static bool metric(void) { return hal_cfg_get_i32("wx_metric", 0) != 0; }

static void cache_path(char *out, size_t n)
{
    const char *root = hal_storage_root();
    snprintf(out, n, "%s/weather.json", root ? root : "");
}

static void cache_save(const char *json, size_t len)
{
    char path[96];
    cache_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f == NULL) return;
    fwrite(json, 1, len, f);
    fclose(f);
}

static void cache_load(void)
{
    char path[96];
    cache_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f == NULL) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n > 0 && n < 64 * 1024) ? malloc((size_t)n + 1) : NULL;
    if (buf) {
        buf[fread(buf, 1, (size_t)n, f)] = '\0';
        if (wx_parse(buf, &s_w.wx)) {
            s_w.have       = true;
            s_w.from_cache = true;
            struct stat st;
            s_w.fetched = (stat(path, &st) == 0) ? st.st_mtime : 0;
        }
        free(buf);
    }
    fclose(f);
}

/* ---- Fetching ------------------------------------------------------------ */

static void on_forecast(char *body, size_t len, void *user)
{
    (void)user;
    s_w.loading = false;
    static wx_t fresh;
    if (body && wx_parse(body, &fresh)) {
        s_w.wx         = fresh;
        s_w.have       = true;
        s_w.from_cache = false;
        s_w.failed     = false;
        s_w.fetched    = time(NULL);
        cache_save(body, len);
    } else {
        s_w.failed = true;
    }
    render();
}

static void refresh_now(void)
{
    char name[48];
    double lat, lon;
    if (s_w.loading || !place(name, sizeof(name), &lat, &lon)) return;
    if (net_state() != NET_CONNECTED) {
        render();
        return;
    }
    char url[512];
    wx_forecast_url(url, sizeof(url), lat, lon, metric());
    s_w.loading = true;
    render();
    net_fetch(url, on_forecast, NULL, &s_ui.alive);
}

/* ---- Location ------------------------------------------------------------ */

static void place_picked(int index, void *user)
{
    (void)user;
    if (index < 0 || index >= s_ui.nplaces) return;
    const wx_place_t *p = &s_ui.places[index];
    char a[24], b[24];
    snprintf(a, sizeof(a), "%.4f", p->lat);
    snprintf(b, sizeof(b), "%.4f", p->lon);
    hal_cfg_set_str("wx_name", p->name);
    hal_cfg_set_str("wx_lat", a);
    hal_cfg_set_str("wx_lon", b);
    s_w.have = false; /* old city's data no longer applies */
    refresh_now();
}

static void on_places(char *body, size_t len, void *user)
{
    (void)len;
    (void)user;
    s_ui.nplaces = body ? wx_parse_places(body, s_ui.places, WX_PLACES) : 0;
    if (s_ui.nplaces == 0) {
        lv_label_set_text(s_ui.status, body ? "NO MATCHING PLACE" : "LOOKUP FAILED");
        lv_obj_set_style_text_color(s_ui.status, g_pal.danger, 0);
        return;
    }
    const char *items[WX_PLACES];
    for (int i = 0; i < s_ui.nplaces; i++) items[i] = s_ui.places[i].label;
    deck_modal_list("LOCATION", items, s_ui.nplaces, place_picked, NULL);
}

static void city_entered(const char *text, void *user)
{
    (void)user;
    if (text == NULL || text[0] == '\0') return;
    if (net_state() != NET_CONNECTED) {
        lv_label_set_text(s_ui.status, "CONNECT WI-FI IN SYSTEM FIRST");
        lv_obj_set_style_text_color(s_ui.status, g_pal.warn, 0);
        return;
    }
    char url[512];
    wx_geocode_url(url, sizeof(url), text);
    lv_label_set_text(s_ui.status, "SEARCHING...");
    net_fetch(url, on_places, NULL, &s_ui.alive);
}

static void ask_location(void) { deck_modal_prompt("CITY", "", city_entered, NULL); }

/* ---- Rendering ----------------------------------------------------------- */

static void fmt_temp(char *buf, size_t n, float t) { snprintf(buf, n, "%.0f\xC2\xB0", (double)t); }

static void render_status(void)
{
    char buf[96];
    lv_color_t c = g_pal.dim;
    char name[48];
    double lat, lon;
    if (!place(name, sizeof(name), &lat, &lon)) {
        snprintf(buf, sizeof(buf), "NO LOCATION // PRESS L OR TAP LOCATION");
        c = g_pal.warn;
    } else if (s_w.loading) {
        snprintf(buf, sizeof(buf), "UPLINK // OPEN-METEO...");
        c = g_pal.accent;
    } else if (s_w.have) {
        long age = s_w.fetched ? (long)(time(NULL) - s_w.fetched) : -1;
        bool stale = s_w.from_cache || s_w.failed || net_state() != NET_CONNECTED;
        if (age >= 0 && age < 90) {
            snprintf(buf, sizeof(buf), "%s // UPDATED JUST NOW", stale ? "CACHED" : "LIVE");
        } else if (age >= 0 && age < 7200) {
            snprintf(buf, sizeof(buf), "%s // UPDATED %ld MIN AGO", stale ? "CACHED" : "LIVE", age / 60);
        } else {
            snprintf(buf, sizeof(buf), "%s // UPDATED %ld H AGO", stale ? "CACHED" : "LIVE", age / 3600);
        }
        c = stale ? g_pal.warn : g_pal.dim;
    } else if (net_state() != NET_CONNECTED) {
        snprintf(buf, sizeof(buf), "OFFLINE // CONNECT WI-FI IN SYSTEM");
        c = g_pal.warn;
    } else {
        snprintf(buf, sizeof(buf), s_w.failed ? "FETCH FAILED // PRESS R" : "NO DATA YET");
        c = s_w.failed ? g_pal.danger : g_pal.dim;
    }
    lv_label_set_text(s_ui.status, buf);
    lv_obj_set_style_text_color(s_ui.status, c, 0);
}

static void render(void)
{
    if (!s_ui.alive) return;
    char name[48] = "";
    double lat, lon;
    place(name, sizeof(name), &lat, &lon);
    deck_icon_text_set(s_ui.place, ICON_PIN, name[0] ? name : "NO LOCATION", name[0] ? g_pal.text : g_pal.warn);
    lv_label_set_text(lv_obj_get_child(s_ui.units_btn, 0), metric() ? "\xC2\xB0" "C" : "\xC2\xB0" "F");
    render_status();

    const wx_t *w = &s_w.wx;
    char buf[64];
    if (!s_w.have) {
        lv_label_set_text(s_ui.temp, "--");
        lv_label_set_text(s_ui.icon, WX_CLOUDY);
        lv_label_set_text(s_ui.cond, "");
        lv_label_set_text(s_ui.range, "");
        for (int i = 0; i < 5; i++) lv_label_set_text(s_ui.kv[i], "--");
        lv_chart_set_all_values(s_ui.chart, s_ui.s_temp, LV_CHART_POINT_NONE);
        lv_chart_set_all_values(s_ui.chart, s_ui.s_pop, LV_CHART_POINT_NONE);
        for (int i = 0; i < WX_DAYS; i++) {
            lv_label_set_text(s_ui.day_name[i], "---");
            lv_label_set_text(s_ui.day_icon[i], "");
            lv_label_set_text(s_ui.day_hi[i], "");
            lv_label_set_text(s_ui.day_lo[i], "");
            lv_label_set_text(lv_obj_get_child(s_ui.day_pop[i], 1), "");
        }
        return;
    }

    fmt_temp(buf, sizeof(buf), w->temp);
    lv_label_set_text(s_ui.temp, buf);
    lv_label_set_text(s_ui.icon, wx_code_icon(w->code, w->is_day));
    lv_label_set_text(s_ui.cond, wx_code_text(w->code));
    char hi[12], lo[12], fl[12];
    fmt_temp(fl, sizeof(fl), w->feels);
    fmt_temp(hi, sizeof(hi), w->day[0].hi);
    fmt_temp(lo, sizeof(lo), w->day[0].lo);
    lv_label_set_text_fmt(s_ui.range, "FEELS %s   H %s  L %s", fl, hi, lo);
    lv_label_set_text_fmt(s_ui.kv[KV_HUMID], "%d%%", w->humidity);
    lv_label_set_text_fmt(s_ui.kv[KV_WIND], "%.0f %s %s", (double)w->wind, w->wind_unit, wx_compass(w->wind_dir));
    lv_label_set_text_fmt(s_ui.kv[KV_PRESS], "%.0f hPa", (double)w->pressure);
    lv_label_set_text(s_ui.kv[KV_SUNRISE], w->sunrise[0] ? w->sunrise : "--");
    lv_label_set_text(s_ui.kv[KV_SUNSET], w->sunset[0] ? w->sunset : "--");

    /* 24 h trace: temperature on the left axis, rain chance 0..100 on the right. */
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < w->hours; i++) {
        tmin = fminf(tmin, w->hour_temp[i]);
        tmax = fmaxf(tmax, w->hour_temp[i]);
    }
    lv_chart_set_axis_range(s_ui.chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floorf(tmin) - 2, (int32_t)ceilf(tmax) + 2);
    for (int i = 0; i < WX_HOURS; i++) {
        bool ok = i < w->hours;
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.s_temp, (uint32_t)i,
                                        ok ? (int32_t)lroundf(w->hour_temp[i]) : LV_CHART_POINT_NONE);
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.s_pop, (uint32_t)i, ok ? w->hour_pop[i] : LV_CHART_POINT_NONE);
    }
    lv_chart_refresh(s_ui.chart);
    for (int i = 0; i < 8; i++) {
        int idx = i * 3;
        if (idx < w->hours) {
            lv_label_set_text_fmt(s_ui.hour_lbl[i], "%02d", w->hour_of_day[idx]);
        } else {
            lv_label_set_text(s_ui.hour_lbl[i], "");
        }
    }

    for (int i = 0; i < WX_DAYS; i++) {
        if (i >= w->days) continue;
        const wx_day_t *d = &w->day[i];
        lv_label_set_text(s_ui.day_name[i], i == 0 ? "TODAY" : d->weekday);
        lv_label_set_text(s_ui.day_icon[i], wx_code_icon(d->code, true));
        fmt_temp(buf, sizeof(buf), d->hi);
        lv_label_set_text(s_ui.day_hi[i], buf);
        fmt_temp(buf, sizeof(buf), d->lo);
        lv_label_set_text(s_ui.day_lo[i], buf);
        deck_icon_text_set(s_ui.day_pop[i], ICON_UMBRELLA, "", d->pop >= 50 ? g_pal.accent2 : g_pal.dim);
        lv_label_set_text_fmt(lv_obj_get_child(s_ui.day_pop[i], 1), "%d%%", d->pop);
    }
}

/* ---- Building ------------------------------------------------------------ */

static lv_obj_t *kv_row(lv_obj_t *parent, const char *icon, const char *key)
{
    lv_obj_t *r = deck_box(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_t *k = deck_icon_text(r, icon, key, g_font.mono_m, g_pal.dim);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = deck_label(r, g_font.mono_m, g_pal.text, "--");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

static lv_obj_t *tool_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = deck_button(parent, text, g_font.disp_s);
    lv_obj_set_style_min_height(b, 44, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_min_width(b, 0, 0);
    lv_group_remove_obj(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void loc_clicked(lv_event_t *e)
{
    (void)e;
    ask_location();
}

static void units_clicked(lv_event_t *e)
{
    (void)e;
    hal_cfg_set_i32("wx_metric", !metric());
    refresh_now();
}

static void refresh_clicked(lv_event_t *e)
{
    (void)e;
    refresh_now();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (net_state() == NET_CONNECTED && !s_w.loading &&
        (!s_w.have || s_w.from_cache || time(NULL) - s_w.fetched > REFRESH_S)) {
        refresh_now();
    } else {
        render_status();
    }
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.alive = true;
    if (!s_w.have) cache_load();

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_column(root, 20, 0);

    /* Left: now */
    lv_obj_t *hero = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(hero, 430, LV_PCT(100));
    deck_panel_set_tab(hero, true);
    lv_obj_set_style_pad_all(hero, 24, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hero, 10, 0);

    s_ui.place = deck_icon_text(hero, ICON_PIN, "", g_font.mono_m, g_pal.text);
    lv_obj_t *now = deck_box(hero);
    lv_obj_set_width(now, LV_PCT(100));
    lv_obj_set_flex_flow(now, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(now, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(now, 18, 0);
    s_ui.icon = deck_label(now, g_font.icon_l, g_pal.accent, "");
    s_ui.temp = deck_label(now, g_font.disp_hero, g_pal.text, "--");
    s_ui.cond  = deck_label(hero, g_font.disp_m, g_pal.accent2, "");
    s_ui.range = deck_label(hero, g_font.mono_m, g_pal.dim, "");
    lv_obj_t *sec = deck_section(hero, "NOW");
    lv_obj_set_style_margin_top(sec, 6, 0);
    s_ui.kv[KV_HUMID]   = kv_row(hero, ICON_HUMIDITY, "HUMIDITY");
    s_ui.kv[KV_WIND]    = kv_row(hero, WX_WIND, "WIND");
    s_ui.kv[KV_PRESS]   = kv_row(hero, ICON_GAUGE, "PRESSURE");
    s_ui.kv[KV_SUNRISE] = kv_row(hero, WX_SUNRISE, "SUNRISE");
    s_ui.kv[KV_SUNSET]  = kv_row(hero, WX_SUNSET, "SUNSET");
    lv_obj_t *spacer = deck_box(hero);
    lv_obj_set_flex_grow(spacer, 1);
    s_ui.status = deck_label(hero, g_font.mono_s, g_pal.dim, "");

    /* Right: trace + week */
    lv_obj_t *right = deck_box(root);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 14, 0);

    lv_obj_t *head = deck_box(right);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 12, 0);
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, "// 24H TRACE");
    lv_obj_set_flex_grow(t, 1);
    deck_label(head, g_font.mono_s, g_pal.accent, "TEMP");
    deck_label(head, g_font.mono_s, g_pal.accent2, "RAIN %");
    tool_button(head, "LOCATION", loc_clicked);
    s_ui.units_btn = tool_button(head, "\xC2\xB0" "F", units_clicked);
    tool_button(head, "REFRESH", refresh_clicked);

    lv_obj_t *trace = deck_panel(right, DECK_CUT_TL, 16);
    lv_obj_set_width(trace, LV_PCT(100));
    lv_obj_set_flex_grow(trace, 1);
    lv_obj_set_style_pad_all(trace, 16, 0);
    lv_obj_set_flex_flow(trace, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(trace, 6, 0);

    s_ui.chart = lv_chart_create(trace);
    lv_obj_set_width(s_ui.chart, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.chart, 1);
    lv_chart_set_type(s_ui.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_ui.chart, WX_HOURS);
    lv_chart_set_div_line_count(s_ui.chart, 4, 7);
    lv_chart_set_axis_range(s_ui.chart, LV_CHART_AXIS_SECONDARY_Y, 0, 100);
    lv_obj_set_style_bg_opa(s_ui.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.chart, 0, 0);
    lv_obj_set_style_pad_all(s_ui.chart, 4, 0);
    lv_obj_set_style_line_color(s_ui.chart, g_pal.grid, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_ui.chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(s_ui.chart, 0, 0, LV_PART_INDICATOR);
    s_ui.s_pop  = lv_chart_add_series(s_ui.chart, g_pal.accent2, LV_CHART_AXIS_SECONDARY_Y);
    s_ui.s_temp = lv_chart_add_series(s_ui.chart, g_pal.accent, LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *hours = deck_box(trace);
    lv_obj_set_width(hours, LV_PCT(100));
    lv_obj_set_flex_flow(hours, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hours, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 8; i++) s_ui.hour_lbl[i] = deck_label(hours, g_font.mono_s, g_pal.dim, "");

    lv_obj_t *week = deck_box(right);
    lv_obj_set_width(week, LV_PCT(100));
    lv_obj_set_flex_flow(week, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(week, 10, 0);
    for (int i = 0; i < WX_DAYS; i++) {
        lv_obj_t *d = deck_panel(week, DECK_CUT_BR, 12);
        lv_obj_set_height(d, 196);
        lv_obj_set_flex_grow(d, 1);
        lv_obj_set_style_pad_ver(d, 12, 0);
        lv_obj_set_flex_flow(d, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(d, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(d, 4, 0);
        if (i == 0) deck_panel_set_tab(d, true);
        s_ui.day_name[i] = deck_label(d, g_font.disp_s, i == 0 ? g_pal.accent : g_pal.text, "");
        s_ui.day_icon[i] = deck_label(d, g_font.icon_m, g_pal.accent, "");
        s_ui.day_hi[i]   = deck_label(d, g_font.mono_l, g_pal.text, "");
        s_ui.day_lo[i]   = deck_label(d, g_font.mono_m, g_pal.dim, "");
        s_ui.day_pop[i]  = deck_icon_text(d, ICON_UMBRELLA, "", g_font.mono_s, g_pal.dim);
        lv_obj_set_style_pad_column(s_ui.day_pop[i], 2, 0);
    }

    render();
    s_ui.timer = lv_timer_create(tick, 1000, NULL);
    tick(s_ui.timer);

    char name[48];
    double lat, lon;
    if (!place(name, sizeof(name), &lat, &lon)) ask_location();
    return true;
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) return false;
    switch (k->code) {
        case HID_R: refresh_now(); return true;
        case HID_L: ask_location(); return true;
        case HID_U: units_clicked(NULL); return true;
        default: return false;
    }
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    memset(&s_ui, 0, sizeof(s_ui)); /* alive = false: in-flight fetches are dropped */
    s_w.loading = false;
}

static deck_app_t s_app = {
    .name     = "WEATHER",
    .tagline  = "ATMOS SCAN",
    .icon     = ICON_WEATHER,
    .eta      = NULL,
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_weather(void) { return &s_app; }
