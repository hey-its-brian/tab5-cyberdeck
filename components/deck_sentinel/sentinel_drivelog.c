/* A wardrive in progress. See sentinel_drivelog.h. */
#include "sentinel_drivelog.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"

static sentinel_drive_slot_t s_slots[SENTINEL_DRIVELOG_SLOTS] DECK_PSRAM_BSS;
static struct {
    bool             on, placed;
    sentinel_drive_t d;
    uint32_t         started_ms, survey_ms, survey_rows;
    bool             survey_due_now; /* the first survey row goes out with the first fix */
    struct {
        bool   ble;
        int8_t rssi;
        char   text[SENTINEL_DRIVE_TEXT];
    } recent[SENTINEL_DRIVE_RECENT]; /* a ring, newest at (head - 1) */
    int head, n;
} s_log;

void sentinel_drivelog_begin(uint32_t now_ms)
{
    memset(&s_log, 0, sizeof(s_log));
    sentinel_drive_init(&s_log.d, s_slots, SENTINEL_DRIVELOG_SLOTS);
    s_log.on             = true;
    s_log.started_ms     = now_ms;
    s_log.survey_due_now = true;
}

/* The counts stay for the screen to show what the drive came to. */
void sentinel_drivelog_end(void) { s_log.on = false; }

bool sentinel_drivelog_on(void) { return s_log.on; }

static void remember(bool ble, int8_t rssi, const char *text)
{
    s_log.recent[s_log.head].ble  = ble;
    s_log.recent[s_log.head].rssi = rssi;
    snprintf(s_log.recent[s_log.head].text, sizeof(s_log.recent[0].text), "%s", text);
    s_log.head = (s_log.head + 1) % SENTINEL_DRIVE_RECENT;
    if (s_log.n < SENTINEL_DRIVE_RECENT) s_log.n++;
}

void sentinel_drivelog_ap(const sentinel_state_t *s, const sentinel_wbs_t *a, uint32_t now_ms, sentinel_drivelog_row_t out)
{
    if (!s_log.on) return;
    sentinel_where_t w;
    bool             placed = sentinel_where(&w, s, now_ms);
    if (sentinel_drive_wifi(&s_log.d, a, placed ? &w : NULL, now_ms) != SENTINEL_DRIVE_ROW) return;
    static char row[SENTINEL_WIGLE_LINE_MAX]; /* static: these run on the reader's stack, under the caller's lock */
    int         len = sentinel_wigle_wifi(row, sizeof(row), a, &w);
    if (len <= 0 || !out(SENTINEL_DRIVE_WIGLE, row, len)) {
        sentinel_drive_lost(&s_log.d, a->bssid, false); /* not on file after all */
        return;
    }
    char text[SENTINEL_DRIVE_TEXT];
    snprintf(text, sizeof(text), "CH %-3u %-4s %.24s", (unsigned)a->ch, sentinel_sec_name(a->sec), a->ssid[0] ? a->ssid : "<hidden>");
    remember(false, a->rssi, text);
}

void sentinel_drivelog_device(const sentinel_state_t *s, const sentinel_bdv_t *d, uint32_t now_ms,
                              sentinel_drivelog_row_t out)
{
    if (!s_log.on) return;
    sentinel_where_t w;
    bool             placed = sentinel_where(&w, s, now_ms);
    if (sentinel_drive_ble(&s_log.d, d, placed ? &w : NULL, now_ms) != SENTINEL_DRIVE_ROW) return;
    static char row[SENTINEL_WIGLE_LINE_MAX];
    int         len = sentinel_wigle_ble(row, sizeof(row), d, &w);
    if (len <= 0 || !out(SENTINEL_DRIVE_WIGLE, row, len)) {
        sentinel_drive_lost(&s_log.d, d->addr, true);
        return;
    }
    char        text[SENTINEL_DRIVE_TEXT], addr[18];
    const char *label = sentinel_ble_kind_label(d->kind), *vendor = sentinel_ble_company(d->company);
    sentinel_ble_addr_text(addr, d->addr);
    snprintf(text, sizeof(text), "BLE    %.31s", d->name[0] ? d->name : label[0] ? label : vendor ? vendor : addr);
    remember(true, d->rssi, text);
}

void sentinel_drivelog_tick(const sentinel_state_t *s, uint32_t now_ms, sentinel_drivelog_row_t out)
{
    if (!s_log.on) return;
    sentinel_where_t w;
    s_log.placed = sentinel_where(&w, s, now_ms); /* false once the link drops: the last fix goes stale */
    if (!s_log.placed) return;
    sentinel_drive_moved(&s_log.d, &w);
    if (!s_log.survey_due_now && (uint32_t)(now_ms - s_log.survey_ms) < SENTINEL_DRIVELOG_SURVEY_MS) return;
    if (!s->sub_sweeps && !s->nrf_sweeps) return; /* neither band has been swept yet: nothing to say */
    static char row[SENTINEL_SURVEY_LINE_MAX];
    int         len = sentinel_survey_csv(row, sizeof(row), s, &w);
    if (len <= 0 || !out(SENTINEL_DRIVE_SURVEY, row, len)) return; /* tried again at the next tick */
    s_log.survey_ms      = now_ms;
    s_log.survey_due_now = false;
    s_log.survey_rows++;
}

void sentinel_drivelog_info(sentinel_drive_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->on          = s_log.on;
    out->placed      = s_log.on && s_log.placed;
    out->started_ms  = s_log.started_ms;
    out->wifi        = s_log.d.wifi;
    out->ble         = s_log.d.ble;
    out->rows        = s_log.d.rows;
    out->survey_rows = s_log.survey_rows;
    out->nofix       = s_log.d.nofix;
    out->optout      = s_log.d.optout;
    out->priv        = s_log.d.priv;
    out->distance_m  = s_log.d.distance_m;
    out->recent_n    = s_log.n;
    for (int i = 0; i < s_log.n; i++) { /* newest first */
        int k = (s_log.head - 1 - i + 2 * SENTINEL_DRIVE_RECENT) % SENTINEL_DRIVE_RECENT;
        out->recent[i].ble  = s_log.recent[k].ble;
        out->recent[i].rssi = s_log.recent[k].rssi;
        memcpy(out->recent[i].text, s_log.recent[k].text, sizeof(out->recent[i].text));
    }
}
