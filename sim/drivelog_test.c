/* A wardrive in progress (components/deck_sentinel/sentinel_drivelog.c): what reaches the files, and what the screen is told. */
#include <stdio.h>
#include <string.h>

#include "sentinel_drivelog.h"

static int s_fail;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);         \
            s_fail++;                                                     \
        }                                                                 \
    } while (0)

static sentinel_state_t s_s;
static int              s_rows[SENTINEL_DRIVE_FILES];
static char             s_last[SENTINEL_DRIVE_FILES][SENTINEL_SURVEY_LINE_MAX + 1];

static bool             s_full; /* the queue to the card has no room */

static bool out(sentinel_drive_file_t file, const char *row, int len)
{
    if (s_full) return false;
    s_rows[file]++;
    memcpy(s_last[file], row, (size_t)len);
    s_last[file][len] = 0;
    return true;
}

static uint32_t    s_clock = 1000;
static const char *s_gps   = "GPS,0,0,6,99.9,0.000000,0.000000,0.0,0.0,0,0,0";

static void say(const char *payload)
{
    static sentinel_lines_t l;
    char frame[SENTINEL_LINE_MAX + 8];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    CHECK(n > 0 && sentinel_proto_feed(&l, &s_s, frame, (size_t)n, s_clock, NULL) == 1);
}

/* Time moves to `now`, and the GPS says where it is again, as it does every second. */
static uint32_t at(uint32_t now)
{
    s_clock = now;
    say(s_gps);
    return now;
}

int main(void)
{
    sentinel_drive_info_t info;
    sentinel_wbs_t ap  = {.bssid = {0xA4, 0xC3, 0xF0, 0x11, 0x22, 0x33}, .ch = 11, .rssi = -63, .sec = SENTINEL_SEC_WPA2, .ssid = "Afterlife"};
    sentinel_wbs_t hid = {.bssid = {0xA4, 0xC3, 0xF0, 0x11, 0x22, 0x34}, .ch = 36, .rssi = -70, .sec = SENTINEL_SEC_OPEN};
    sentinel_bdv_t phone = {.addr = {0x5A, 1, 2, 3, 4, 5}, .rssi = -50};
    sentinel_bdv_t tag   = {.addr = {0xF3, 1, 2, 3, 4, 5}, .rssi = -71, .kind = SENTINEL_BLE_TILE};

    sentinel_state_init(&s_s);
    say("STAT,60,1,1,0,1,200000");
    say(s_gps);

    /* Before a drive begins nothing is taken. */
    sentinel_drivelog_ap(&s_s, &ap, 100, out);
    sentinel_drivelog_tick(&s_s, 100, out);
    sentinel_drivelog_info(&info);
    CHECK(!sentinel_drivelog_on() && !info.on && s_rows[SENTINEL_DRIVE_WIGLE] == 0 && info.nofix == 0);

    /* Begun, but the GPS has no fix: counted, not written. */
    sentinel_drivelog_begin(1000);
    sentinel_drivelog_ap(&s_s, &ap, 1100, out);
    sentinel_drivelog_tick(&s_s, 1100, out);
    sentinel_drivelog_info(&info);
    CHECK(info.on && !info.placed && info.nofix == 1 && info.rows == 0 && info.started_ms == 1000);
    CHECK(s_rows[SENTINEL_DRIVE_WIGLE] == 0 && s_rows[SENTINEL_DRIVE_SURVEY] == 0);

    /* The fix arrives. The first survey row waits for a sweep of either band. */
    s_gps = "GPS,1,9,12,1.2,40.741895,-73.989308,12.4,38.0,45,150405.00,121126";
    sentinel_drivelog_tick(&s_s, at(2000), out);
    CHECK(s_rows[SENTINEL_DRIVE_SURVEY] == 0);
    s_s.sub_sweeps = 1;
    sentinel_drivelog_tick(&s_s, at(2100), out);
    sentinel_drivelog_info(&info);
    CHECK(info.placed && s_rows[SENTINEL_DRIVE_SURVEY] == 1 && info.survey_rows == 1);
    CHECK(strncmp(s_last[SENTINEL_DRIVE_SURVEY], "2026-11-12T15:04:05Z,40.741895,-73.989308,", 42) == 0);
    sentinel_drivelog_tick(&s_s, at(2100 + SENTINEL_DRIVELOG_SURVEY_MS - 100), out);
    CHECK(s_rows[SENTINEL_DRIVE_SURVEY] == 1); /* one every five seconds */
    sentinel_drivelog_tick(&s_s, at(2100 + SENTINEL_DRIVELOG_SURVEY_MS), out);
    CHECK(s_rows[SENTINEL_DRIVE_SURVEY] == 2);

    /* Sightings: a row the first time, nothing the second; a phone never. */
    sentinel_drivelog_ap(&s_s, &ap, at(7200), out);
    CHECK(s_rows[SENTINEL_DRIVE_WIGLE] == 1);
    CHECK(strcmp(s_last[SENTINEL_DRIVE_WIGLE],
                 "a4:c3:f0:11:22:33,Afterlife,[WPA2][ESS],2026-11-12 15:04:05,11,2462,-63,40.741895,-73.989308,12,6.0,,,WIFI\n") == 0);
    sentinel_drivelog_ap(&s_s, &ap, 7300, out);
    sentinel_drivelog_device(&s_s, &phone, 7300, out);
    CHECK(s_rows[SENTINEL_DRIVE_WIGLE] == 1);
    sentinel_drivelog_device(&s_s, &tag, 7400, out);
    CHECK(s_rows[SENTINEL_DRIVE_WIGLE] == 2 && strstr(s_last[SENTINEL_DRIVE_WIGLE], ",[BLE],") &&
          strncmp(s_last[SENTINEL_DRIVE_WIGLE], "f3:01:02:03:04:05,", 18) == 0);
    sentinel_drivelog_ap(&s_s, &hid, 7500, out);
    sentinel_drivelog_info(&info);
    CHECK(info.wifi == 2 && info.ble == 1 && info.rows == 3 && info.priv == 1 && info.nofix == 1 && info.optout == 0);
    /* What the screen lists: newest first, each with something to call it. */
    CHECK(info.recent_n == 3 && !info.recent[0].ble && strstr(info.recent[0].text, "<hidden>") && info.recent[0].rssi == -70);
    CHECK(info.recent[1].ble && strstr(info.recent[1].text, "Tile") && info.recent[1].rssi == -71);
    CHECK(!info.recent[2].ble && strstr(info.recent[2].text, "Afterlife") && strstr(info.recent[2].text, "WPA2"));

    /* The deck moves 60 m north: distance counted, and the network gets its second row. */
    s_gps = "GPS,1,9,12,1.2,40.742435,-73.989308,12.4,38.0,45,150410.00,121126";
    sentinel_drivelog_tick(&s_s, at(9000), out);
    sentinel_drivelog_ap(&s_s, &ap, 9100, out);
    sentinel_drivelog_info(&info);
    CHECK(info.distance_m > 55 && info.distance_m < 65 && info.rows == 4 && info.wifi == 2);
    CHECK(strstr(s_last[SENTINEL_DRIVE_WIGLE], ",2026-11-12 15:04:10,") && strstr(s_last[SENTINEL_DRIVE_WIGLE], ",40.742435,"));

    /* The queue to the card is full: the row is not on file, the list does
     * not show it, and the device is written the next time it is heard. */
    sentinel_wbs_t late = ap;
    late.bssid[5] = 0x55;
    snprintf(late.ssid, sizeof(late.ssid), "late");
    int before = s_rows[SENTINEL_DRIVE_WIGLE], survey_before = s_rows[SENTINEL_DRIVE_SURVEY];
    s_full = true;
    sentinel_drivelog_ap(&s_s, &late, 9150, out);
    sentinel_drivelog_device(&s_s, &phone, 9150, out);
    sentinel_drivelog_tick(&s_s, at(9000 + SENTINEL_DRIVELOG_SURVEY_MS), out);
    sentinel_drivelog_info(&info);
    CHECK(s_rows[SENTINEL_DRIVE_WIGLE] == before && info.rows == 4 && !strstr(info.recent[0].text, "late"));
    CHECK(s_rows[SENTINEL_DRIVE_SURVEY] == survey_before && info.survey_rows == (uint32_t)survey_before);
    s_full = false;
    sentinel_drivelog_ap(&s_s, &late, at(9000 + SENTINEL_DRIVELOG_SURVEY_MS + 100), out);
    sentinel_drivelog_tick(&s_s, 9000 + SENTINEL_DRIVELOG_SURVEY_MS + 100, out);
    sentinel_drivelog_info(&info);
    CHECK(s_rows[SENTINEL_DRIVE_WIGLE] == before + 1 && info.rows == 5 && strstr(info.recent[0].text, "late"));
    CHECK(s_rows[SENTINEL_DRIVE_SURVEY] == survey_before + 1);

    /* The link drops: no more GPS sentences, so the last fix goes stale.
     * Nothing is written with it, and the screen is told there is no place. */
    survey_before = s_rows[SENTINEL_DRIVE_SURVEY];
    sentinel_drivelog_tick(&s_s, 80000, out); /* a minute after the last GPS sentence */
    sentinel_wbs_t ghost = ap;
    ghost.bssid[5] = 0x56;
    sentinel_drivelog_ap(&s_s, &ghost, 80000, out);
    sentinel_drivelog_info(&info);
    CHECK(!info.placed && s_rows[SENTINEL_DRIVE_SURVEY] == survey_before && info.rows == 5 && info.nofix == 2);
    sentinel_drivelog_tick(&s_s, at(81000), out); /* the link is back */
    sentinel_drivelog_info(&info);
    CHECK(info.placed);

    /* More rows than the list holds: it keeps the newest. */
    for (int i = 0; i < SENTINEL_DRIVE_RECENT + 3; i++) {
        sentinel_wbs_t a = ap;
        a.bssid[5] = (uint8_t)(0x80 + i);
        snprintf(a.ssid, sizeof(a.ssid), "net-%02d", i);
        sentinel_drivelog_ap(&s_s, &a, 81000, out);
    }
    sentinel_drivelog_info(&info);
    char want[16];
    snprintf(want, sizeof(want), "net-%02d", SENTINEL_DRIVE_RECENT + 2);
    CHECK(info.recent_n == SENTINEL_DRIVE_RECENT && strstr(info.recent[0].text, want) && strstr(info.recent[SENTINEL_DRIVE_RECENT - 1].text, "net-03"));

    /* Ended: nothing more is taken, and the counts stay for the screen. */
    int rows = s_rows[SENTINEL_DRIVE_WIGLE], survey = s_rows[SENTINEL_DRIVE_SURVEY];
    sentinel_drivelog_end();
    hid.bssid[5] = 0x99;
    sentinel_drivelog_ap(&s_s, &hid, at(90000), out);
    sentinel_drivelog_device(&s_s, &tag, 90000, out);
    sentinel_drivelog_tick(&s_s, 90000, out);
    sentinel_drivelog_info(&info);
    CHECK(!info.on && !info.placed && s_rows[SENTINEL_DRIVE_WIGLE] == rows && s_rows[SENTINEL_DRIVE_SURVEY] == survey);
    CHECK(info.rows == (uint32_t)rows && info.wifi == 3 + SENTINEL_DRIVE_RECENT + 3);
    /* A new drive starts from nothing. */
    sentinel_drivelog_begin(at(95000));
    sentinel_drivelog_info(&info);
    CHECK(info.on && info.rows == 0 && info.wifi == 0 && info.recent_n == 0 && info.distance_m == 0);
    sentinel_drivelog_ap(&s_s, &ap, 95100, out);
    CHECK(s_rows[SENTINEL_DRIVE_WIGLE] == rows + 1); /* on file last time is not on file this time */

    printf("%s (%d failures)\n", s_fail ? "FAIL" : "PASS", s_fail);
    return s_fail ? 1 : 0;
}
