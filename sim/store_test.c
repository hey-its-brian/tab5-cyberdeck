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

    /* captures are for the card only: on internal flash they are dropped, events still land */
    s_sd = false;
    sentinel_store_packet(&p);
    sentinel_store_event(&e, 0, 0, 0);
    sentinel_store_stats(&st);
    CHECK(st.packets == 3 && st.packets_dropped == 2 && st.events == 3);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) printf("could not remove %s\n", root);
    printf("%s (%d failures)\n", s_fail ? "FAIL" : "PASS", s_fail);
    return s_fail ? 1 : 0;
}
