/* The event log and capture files. See sentinel_store.h. */
#include "sentinel_store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "deck_hal.h"
#include "sentinel_log.h"
#include "sentinel_pcap.h"

#define FLUSH_EVERY 16 /* packets between flushes while frames keep coming */
#define DIR_MAX     200 /* "/sdcard/sentinel" on the device; the simulator's root is a long host path */
#define PATH_MAX_   256

static sentinel_store_stats_t s_stats;
static struct {
    FILE                 *f;
    sentinel_pcap_clock_t clock;
    unsigned              unflushed;
} s_cap[SENTINEL_LINK_N];

/* "<root>/sentinel", created on first use. False when there is no storage. */
static bool store_dir(char *out, size_t n)
{
    const char *root = hal_storage_root();
    if (!root) return false;
    int len = snprintf(out, n, "%s/sentinel", root);
    if (len < 0 || (size_t)len >= n) return false; /* a root too long to hold is no storage */
    return mkdir(out, 0755) == 0 || errno == EEXIST;
}

static void local_now(struct tm *t, time_t *raw)
{
    time_t now = time(NULL);
    localtime_r(&now, t);
    if (raw) *raw = now;
}

void sentinel_store_event(const sentinel_event_t *e, uint8_t fix, double lat, double lon)
{
    char      dir[DIR_MAX], name[32], path[PATH_MAX_], row[SENTINEL_CSV_LINE_MAX];
    struct tm t;
    local_now(&t, NULL);
    if (!store_dir(dir, sizeof(dir)) || !sentinel_log_events_name(name, sizeof(name), &t)) {
        s_stats.events_dropped++;
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    bool        fresh = stat(path, &st) != 0 || st.st_size == 0;
    int         len   = sentinel_log_csv(row, sizeof(row), &t, e, fix, lat, lon);
    FILE       *f     = len > 0 ? fopen(path, "a") : NULL;
    if (!f) {
        s_stats.events_dropped++;
        return;
    }
    bool ok = (!fresh || fputs(SENTINEL_CSV_HEADER, f) >= 0) && fwrite(row, 1, (size_t)len, f) == (size_t)len;
    ok      = fclose(f) == 0 && ok;
    if (ok) s_stats.events++;
    else s_stats.events_dropped++;
}

static FILE *capture_open(sentinel_link_t link)
{
    char      dir[DIR_MAX], name[48], path[PATH_MAX_];
    struct tm t;
    local_now(&t, NULL);
    if (!hal_storage_is_sd() || !store_dir(dir, sizeof(dir)) ||
        !sentinel_log_capture_name(name, sizeof(name), link, &t))
        return NULL;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    /* Append, never truncate: the name only resolves to a second, and a
     * reopen after a failed write (or a quick off and on) can land on a file
     * that already holds packets. Only an empty file gets the header. */
    FILE *f = fopen(path, "ab");
    if (!f) return NULL;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0) {
        uint8_t head[SENTINEL_PCAP_HEADER];
        sentinel_pcap_header(head, sentinel_pcap_linktype(link));
        if (fwrite(head, 1, sizeof(head), f) != sizeof(head)) {
            fclose(f);
            return NULL;
        }
    }
    return f;
}

void sentinel_store_packet(const sentinel_pkt_t *p)
{
    if ((int)p->link < 0 || p->link >= SENTINEL_LINK_N || p->len == 0 || p->len > SENTINEL_PKT_MAX) {
        s_stats.packets_dropped++;
        return;
    }
    if (!s_cap[p->link].f) {
        s_cap[p->link].f         = capture_open(p->link);
        s_cap[p->link].clock.set = false;
        s_cap[p->link].unflushed = 0;
    }
    FILE *f = s_cap[p->link].f;
    if (!f) {
        s_stats.packets_dropped++;
        return;
    }
    uint32_t sec, usec;
    uint8_t  rec[SENTINEL_PCAP_RECORD], radio[SENTINEL_PCAP_BLE_PHDR];
    /* Bluetooth packets go behind a radio header that carries the signal
     * and admits the CRC was never seen; it counts as part of the packet. */
    size_t prefix = sentinel_pcap_prefix(p->link);
    if (prefix) sentinel_pcap_ble_phdr(radio, p->rssi);
    sentinel_pcap_time(&s_cap[p->link].clock, (uint32_t)time(NULL), p->us, &sec, &usec);
    sentinel_pcap_record(rec, sec, usec, (uint32_t)(prefix + p->len), (uint32_t)(prefix + p->orig_len));
    if (fwrite(rec, 1, sizeof(rec), f) != sizeof(rec) || (prefix && fwrite(radio, 1, prefix, f) != prefix) ||
        fwrite(p->data, 1, p->len, f) != p->len) {
        /* The card filled up or went away: close, and let the next packet try a new file. */
        fclose(f);
        s_cap[p->link].f = NULL;
        s_stats.packets_dropped++;
        return;
    }
    s_stats.packets++;
    if (++s_cap[p->link].unflushed >= FLUSH_EVERY) {
        fflush(f);
        s_cap[p->link].unflushed = 0;
    }
}

void sentinel_store_flush(void)
{
    for (int i = 0; i < SENTINEL_LINK_N; i++) {
        if (s_cap[i].f && s_cap[i].unflushed) {
            fflush(s_cap[i].f);
            s_cap[i].unflushed = 0;
        }
    }
}

void sentinel_store_close(void)
{
    for (int i = 0; i < SENTINEL_LINK_N; i++) {
        if (s_cap[i].f) fclose(s_cap[i].f);
        s_cap[i].f = NULL;
    }
}

void sentinel_store_stats(sentinel_store_stats_t *out) { *out = s_stats; }

int sentinel_store_tail(char *out, size_t n)
{
    char      dir[DIR_MAX], name[32], path[PATH_MAX_];
    struct tm t;
    if (n == 0) return 0;
    out[0] = 0;
    local_now(&t, NULL);
    const char *root = hal_storage_root();
    if (!root || !sentinel_log_events_name(name, sizeof(name), &t)) return 0;
    snprintf(dir, sizeof(dir), "%s/sentinel", root);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    long start = size > (long)(n - 1) ? size - (long)(n - 1) : 0;
    fseek(f, start, SEEK_SET);
    size_t got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = 0;
    if (start > 0) {
        /* The window opened mid-row: drop up to and including the first newline. */
        char *nl = memchr(out, '\n', got);
        if (!nl) {
            out[0] = 0;
            return 0;
        }
        got -= (size_t)(nl + 1 - out);
        memmove(out, nl + 1, got + 1);
    }
    return (int)got;
}
