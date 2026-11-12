/* The event log and capture files (components/deck_sentinel/sentinel_store.c), on a scratch directory. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sentinel_log.h"
#include "sentinel_store.h"
#include "sentinel_wigle.h"

static int s_fail;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);         \
            s_fail++;                                                     \
        }                                                                 \
    } while (0)

/* The hardware layer the store asks for its root. */
static const char *s_root;
static bool        s_sd = true;
const char *hal_storage_root(void) { return s_root; }
bool        hal_storage_is_sd(void) { return s_sd; }

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void today(char *out, size_t n, const char *root)
{
    time_t    now = time(NULL);
    struct tm t;
    char      name[32];
    localtime_r(&now, &t);
    sentinel_log_events_name(name, sizeof(name), &t);
    snprintf(out, n, "%s/sentinel/%s", root, name);
}

int main(void)
{
    char root[] = "/tmp/sentinel_store_XXXXXX";
    if (!mkdtemp(root)) return 2;
    sentinel_store_stats_t st;
    sentinel_event_t       e = {.ms = 1, .warn = true, .src = "sub", .text = "433.9 MHz active (-61 dBm)"};

    /* store_without_root: nothing to write to is a counted drop, not a crash */
    s_root = NULL;
    sentinel_store_event(&e, 0, 0, 0);
    sentinel_pkt_t p = {.link = SENTINEL_LINK_WIFI, .ch = 6, .rssi = -50, .us = 1000, .orig_len = 4, .len = 4, .data = {1, 2, 3, 4}};
    sentinel_store_packet(&p);
    sentinel_store_stats(&st);
    CHECK(st.events == 0 && st.events_dropped == 1 && st.packets == 0 && st.packets_dropped == 1);
    char tail[256];
    CHECK(sentinel_store_tail(tail, sizeof(tail)) == 0 && tail[0] == 0);

    /* store_writes_header_once_and_appends */
    s_root = root;
    char path[160];
    today(path, sizeof(path), root);
    sentinel_store_event(&e, 3, 40.741895, -73.989308);
    sentinel_event_t e2 = {.warn = false, .src = "gps", .text = "3D fix, 9 satellites"};
    sentinel_store_event(&e2, 0, 0, 0);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        char line[SENTINEL_CSV_LINE_MAX];
        CHECK(fgets(line, sizeof(line), f) && strcmp(line, SENTINEL_CSV_HEADER) == 0);
        CHECK(fgets(line, sizeof(line), f) && strstr(line, ",warn,sub,433.9 MHz active (-61 dBm),40.741895,-73.989308,3\n"));
        CHECK(fgets(line, sizeof(line), f) && strstr(line, ",info,gps,\"3D fix, 9 satellites\",,,0\n"));
        CHECK(!fgets(line, sizeof(line), f)); /* one header, two rows */
        fclose(f);
    }
    sentinel_store_stats(&st);
    CHECK(st.events == 2 && st.events_dropped == 1);

    /* store_tail_starts_on_a_line: the whole file when it fits, otherwise whole rows from the end */
    char big[1024];
    int  n = sentinel_store_tail(big, sizeof(big));
    CHECK(n == (int)file_size(path) && strncmp(big, "time,level", 10) == 0);
    n = sentinel_store_tail(tail, 60); /* room for the last row (58 bytes) but not the one before */
    CHECK(n > 0 && strstr(tail, "3D fix") && !strstr(tail, "433.9") && tail[n - 1] == '\n');
    CHECK(strncmp(tail, "20", 2) == 0); /* starts at a row's timestamp */
    CHECK(sentinel_store_tail(tail, 10) == 0); /* too small for any whole row */

    /* store_capture_is_valid_pcap: 24 byte header, then 16 + n per packet */
    sentinel_store_packet(&p);
    p.us = 2500000, p.len = 200, p.orig_len = 300;
    sentinel_store_packet(&p);
    sentinel_store_close();
    sentinel_store_stats(&st);
    CHECK(st.packets == 2 && st.packets_dropped == 1);
    char cmd[256], cap[200] = {0};
    snprintf(cmd, sizeof(cmd), "ls %s/sentinel/cap-wifi-*.pcap", root);
    FILE *ls = popen(cmd, "r");
    CHECK(ls && fgets(cap, sizeof(cap), ls));
    if (ls) pclose(ls);
    cap[strcspn(cap, "\n")] = 0;
    CHECK(file_size(cap) == 24 + (16 + 4) + (16 + 200));
    f = fopen(cap, "rb");
    if (f) {
        unsigned char h[24 + 16];
        CHECK(fread(h, 1, sizeof(h), f) == sizeof(h));
        CHECK(h[0] == 0xd4 && h[1] == 0xc3 && h[2] == 0xb2 && h[3] == 0xa1 && h[20] == 105);
        CHECK(h[24 + 8] == 4 && h[24 + 12] == 4); /* first record: 4 bytes kept of 4 */
        fclose(f);
    }

    /* A reopen never truncates: the same second gives the same name, and the
     * packets already in that file must survive (one header per file). */
    p.us = 3000000;
    sentinel_store_packet(&p);
    sentinel_store_close();
    snprintf(cmd, sizeof(cmd), "cat %s/sentinel/cap-wifi-*.pcap | wc -c", root);
    long total = 0, files = 0;
    FILE *wc = popen(cmd, "r");
    if (wc) {
        if (fscanf(wc, "%ld", &total) != 1) total = -1;
        pclose(wc);
    }
    snprintf(cmd, sizeof(cmd), "ls %s/sentinel/cap-wifi-*.pcap | wc -l", root);
    wc = popen(cmd, "r");
    if (wc) {
        if (fscanf(wc, "%ld", &files) != 1) files = -1;
        pclose(wc);
    }
    CHECK(files >= 1 && total == 24 * files + (16 + 4) + (16 + 200) + (16 + 200));

    /* A Bluetooth capture: link type 256, and each packet behind its 10-byte radio header. */
    sentinel_pkt_t b = {.link = SENTINEL_LINK_BLE, .ch = 37, .rssi = -61, .us = 5000, .orig_len = 18, .len = 18};
    memset(b.data, 0x5A, 18);
    sentinel_store_packet(&b);
    sentinel_store_close();
    snprintf(cmd, sizeof(cmd), "ls %s/sentinel/cap-ble-*.pcap", root);
    ls = popen(cmd, "r");
    cap[0] = 0;
    CHECK(ls && fgets(cap, sizeof(cap), ls));
    if (ls) pclose(ls);
    cap[strcspn(cap, "\n")] = 0;
    CHECK(file_size(cap) == 24 + 16 + 10 + 18);
    f = fopen(cap, "rb");
    if (f) {
        unsigned char h[24 + 16 + 10];
        CHECK(fread(h, 1, sizeof(h), f) == sizeof(h));
        CHECK(h[20] == 0x00 && h[21] == 0x01);        /* link type 256 */
        CHECK(h[24 + 8] == 28 && h[24 + 12] == 28);   /* lengths include the radio header */
        CHECK(h[40 + 1] == 0xC3 && h[40 + 8] == 0x13); /* -61 dBm; flags: CRC not checked */
        fclose(f);
    }

    /* captures are for the card only: on internal flash they are dropped, events still land */
    s_sd = false;
    sentinel_store_packet(&p);
    sentinel_store_event(&e, 0, 0, 0);
    sentinel_store_stats(&st);
    CHECK(st.packets == 4 && st.packets_dropped == 2 && st.events == 3);

    /* A wardrive: two files, each opened with its header, rows appended as
     * they come, and nothing written once the drive has ended. */
    s_sd = true;
    sentinel_wigle_id_t id = {.app = "1.5.0", .model = "Tab5", .release = "0.6.0", .board = "ESP32-P4", .brand = "M5Stack"};
    char head[400], wigle[400], survey[400], got[1200];
    int  head_n = sentinel_wigle_head(head, sizeof(head), &id);
    const char *row1 = "a4:c3:f0:11:22:33,Afterlife,[WPA2][ESS],2026-11-12 15:04:05,11,2462,-63,40.741895,-73.989308,12,6.0,,,WIFI\n";
    const char *row2 = "c1:22:33:44:55:66,Tile,[BLE],2026-11-12 15:04:07,0,7936,-71,40.741895,-73.989308,12,6.0,,,BLE\n";
    const char *srow = "2026-11-12T15:04:05Z,40.741895,-73.989308,12,6.0,-61,433.9,2,2462,-104,0\n";
    sentinel_store_drive_row(SENTINEL_DRIVE_WIGLE, row1, strlen(row1)); /* no drive yet: lost, and counted */
    sentinel_store_stats(&st);
    CHECK(!st.drive_open && st.drive_rows == 0 && st.drive_dropped == 1);
    CHECK(sentinel_store_drive_open(&id));
    sentinel_store_stats(&st);
    CHECK(st.drive_open && strncmp(st.drive_file, "wigle-", 6) == 0 && strstr(st.drive_file, ".csv"));
    snprintf(wigle, sizeof(wigle), "%s/sentinel/%s", root, st.drive_file);
    snprintf(survey, sizeof(survey), "%s/sentinel/survey-%s", root, st.drive_file + 6);
    sentinel_store_drive_row(SENTINEL_DRIVE_WIGLE, row1, strlen(row1));
    sentinel_store_drive_row(SENTINEL_DRIVE_SURVEY, srow, strlen(srow));
    sentinel_store_drive_row(SENTINEL_DRIVE_WIGLE, row2, strlen(row2));
    sentinel_store_flush(); /* on the card without closing: a deck that loses power keeps what it wrote */
    CHECK(file_size(wigle) == head_n + (long)strlen(row1) + (long)strlen(row2));
    CHECK(file_size(survey) == (long)strlen(SENTINEL_SURVEY_HEADER) + (long)strlen(srow));
    /* Opened again within the same second (stop and start): the same files, one header each. */
    sentinel_store_drive_close();
    CHECK(sentinel_store_drive_open(&id));
    sentinel_store_drive_row(SENTINEL_DRIVE_WIGLE, row2, strlen(row2));
    sentinel_store_drive_close();
    sentinel_store_stats(&st);
    CHECK(!st.drive_open && st.drive_rows == 4 && st.drive_dropped == 1);
    snprintf(cmd, sizeof(cmd), "cat %s/sentinel/wigle-*.csv", root);
    FILE *cat = popen(cmd, "r");
    size_t got_n = cat ? fread(got, 1, sizeof(got) - 1, cat) : 0;
    if (cat) pclose(cat);
    got[got_n] = 0;
    CHECK(strncmp(got, head, (size_t)head_n) == 0 && strstr(got + head_n, row1) && strstr(got, row2));
    int heads = 0;
    for (const char *q = got; (q = strstr(q, "WigleWifi-1.6")) != NULL; q++) heads++;
    snprintf(cmd, sizeof(cmd), "ls %s/sentinel/wigle-*.csv | wc -l", root);
    wc = popen(cmd, "r");
    if (wc) {
        if (fscanf(wc, "%ld", &files) != 1) files = -1;
        pclose(wc);
    }
    CHECK(heads == files && files >= 1); /* one header per file, however often it was opened */
    sentinel_store_drive_row(SENTINEL_DRIVE_WIGLE, row1, strlen(row1)); /* after the end: not written */
    sentinel_store_stats(&st);
    CHECK(st.drive_rows == 4 && st.drive_dropped == 2);
    /* Like captures, a drive is for the card: internal flash would fill within the hour. */
    s_sd = false;
    CHECK(!sentinel_store_drive_open(&id));
    sentinel_store_stats(&st);
    CHECK(!st.drive_open);

    /* A site survey: one file, written whole, wherever there is storage. */
    static sentinel_bandhost_t band;
    sentinel_bandhost_init(&band, 1000);
    for (int i = 0; i < SENTINEL_SUB_CH; i++) band.sub_stat[i] = (sentinel_substat_t){.n = 40, .busy = i == 9 ? 4 : 0, .floor = -106, .peak = (int8_t)(i == 9 ? -61 : -97), .sum = -4160.0f};
    for (int i = 0; i < SENTINEL_NRF_CH; i++) band.nrf_stat[i] = (sentinel_nrfstat_t){.n = 10, .any = i == 62 ? 9 : 0, .sum = i == 62 ? 72 : 0, .peak = (uint8_t)(i == 62 ? 8 : 0)};
    char site[40];
    sentinel_where_t where = {.lat = 40.741895, .lon = -73.989308};
    CHECK(sentinel_store_site(&band, "2026-11-12T15:04:05Z", &where, 61000, site, sizeof(site)));
    CHECK(strncmp(site, "site-", 5) == 0 && strstr(site, ".csv"));
    char site_path[400];
    snprintf(site_path, sizeof(site_path), "%s/sentinel/%s", root, site);
    FILE *sf = fopen(site_path, "r");
    CHECK(sf != NULL);
    if (sf) {
        char line[SENTINEL_SITE_LINE_MAX];
        int  lines = 0, sub_rows = 0, nrf_rows = 0;
        while (fgets(line, sizeof(line), sf)) {
            lines++;
            if (lines == 1) CHECK(strcmp(line, "time,lat,lon,seconds,sub_sweeps,nrf_reports\n") == 0);
            if (lines == 2) CHECK(strcmp(line, "2026-11-12T15:04:05Z,40.741895,-73.989308,60,40,10\n") == 0);
            if (lines == 3) CHECK(strcmp(line, "band,mhz,floor,mean,peak,busy_pct\n") == 0);
            if (!strncmp(line, "433,", 4)) sub_rows++;
            if (!strncmp(line, "2400,", 5)) nrf_rows++;
            if (!strncmp(line, "433,433.9,", 10)) CHECK(strcmp(line, "433,433.9,-106,-104.0,-61,10.0\n") == 0);
            if (!strncmp(line, "2400,2462,", 10)) CHECK(strcmp(line, "2400,2462,0,7.2,8,90.0\n") == 0);
        }
        fclose(sf);
        CHECK(lines == 3 + SENTINEL_SUB_CH + SENTINEL_NRF_CH && sub_rows == SENTINEL_SUB_CH && nrf_rows == SENTINEL_NRF_CH);
    }
    /* Nothing swept on either band (no link for the whole minute): there is no survey to write. */
    static sentinel_bandhost_t empty;
    sentinel_bandhost_init(&empty, 1000);
    CHECK(!sentinel_store_site(&empty, "2026-11-12T15:04:05Z", NULL, 61000, site, sizeof(site)));
    s_root = NULL; /* no storage at all: it says so */
    CHECK(!sentinel_store_site(&band, "2026-11-12T15:04:05Z", NULL, 61000, site, sizeof(site)));
    s_root = root;

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) printf("could not remove %s\n", root);
    printf("%s (%d failures)\n", s_fail ? "FAIL" : "PASS", s_fail);
    return s_fail ? 1 : 0;
}
