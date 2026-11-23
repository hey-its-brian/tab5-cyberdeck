/* The event log, capture files and a wardrive's files. See sentinel_store.h. */
#include "sentinel_store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "deck_hal.h"
#include "sentinel_log.h"
#include "sentinel_pcap.h"
#include "sentinel_wigle.h"

#define FLUSH_EVERY 16 /* packets between flushes while frames keep coming */
#define DIR_MAX     200 /* "/sdcard/sentinel" on the device; the simulator's root is a long host path */
#define PATH_MAX_   256

#define DRIVE_SYNC_S 5 /* a wardrive's rows reach the card at least this often */

static sentinel_store_stats_t s_stats;
static struct {
    FILE  *f[SENTINEL_DRIVE_FILES];
    bool   unsynced[SENTINEL_DRIVE_FILES];
    time_t synced;
} s_drive;
static void drive_sync(void);
#define CAP_RETRY_S 2 /* after a file refuses to open or take a frame, leave the card alone this long */
static struct {
    FILE                 *f;
    sentinel_pcap_clock_t clock;
    unsigned              unflushed;
    time_t                retry_at; /* no attempt on the card before this */
} s_cap[SENTINEL_LINK_N];

/* A capture file failed (to open, or to take a frame): drop frames without
 * touching the card for a while, and say so in the stats once. */
static void capture_failed(sentinel_link_t link)
{
    if (s_cap[link].f) fclose(s_cap[link].f);
    s_cap[link].f        = NULL;
    s_cap[link].retry_at = time(NULL) + CAP_RETRY_S;
    s_stats.cap_failing[link] = true;
    s_stats.packets_dropped++;
}

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
        if (fwrite(head, 1, sizeof(head), f) != sizeof(head) || fflush(f) != 0) {
            fclose(f);
            return NULL;
        }
    }
    /* Appending starts at the end now, not at the first write: a record that fails
     * is cut back to where it began, never to the start of an older capture. */
    fseek(f, 0, SEEK_END);
    /* The name is shown on screen, so it is only a name once the file is there. */
    snprintf(s_stats.cap_file[link], sizeof(s_stats.cap_file[link]), "%s", name);
    s_stats.cap_failing[link] = false;
    return f;
}

void sentinel_store_packet(const sentinel_pkt_t *p)
{
    if ((int)p->link < 0 || p->link >= SENTINEL_LINK_N || p->len == 0 || p->len > SENTINEL_PKT_MAX) {
        s_stats.packets_dropped++;
        return;
    }
    if (!s_cap[p->link].f) {
        time_t now = time(NULL); /* (a clock set backwards must not stretch the pause: only a pause still inside its length counts) */
        if (s_cap[p->link].retry_at && now < s_cap[p->link].retry_at && s_cap[p->link].retry_at - now <= CAP_RETRY_S) {
            s_stats.packets_dropped++;
            return;
        }
        s_cap[p->link].f         = capture_open(p->link);
        s_cap[p->link].clock.set = false;
        s_cap[p->link].unflushed = 0;
        if (!s_cap[p->link].f) {
            capture_failed(p->link);
            return;
        }
    }
    FILE *f = s_cap[p->link].f;
    uint32_t sec, usec;
    uint8_t  rec[SENTINEL_PCAP_RECORD], radio[SENTINEL_PCAP_BLE_PHDR];
    /* Bluetooth packets go behind a radio header that carries the signal
     * and admits the CRC was never seen; it counts as part of the packet. */
    size_t prefix = sentinel_pcap_prefix(p->link);
    if (prefix) sentinel_pcap_ble_phdr(radio, p->rssi);
    sentinel_pcap_time(&s_cap[p->link].clock, (uint32_t)time(NULL), p->us, &sec, &usec);
    sentinel_pcap_record(rec, sec, usec, (uint32_t)(prefix + p->len), (uint32_t)(prefix + p->orig_len));
    long at = ftell(f); /* where this record starts: a record half written is cut back to here */
    if (fwrite(rec, 1, sizeof(rec), f) != sizeof(rec) || (prefix && fwrite(radio, 1, prefix, f) != prefix) ||
        fwrite(p->data, 1, p->len, f) != p->len) {
        /* The card filled up or went away: no half record left behind, and the next packet tries again later. */
        if (at >= 0 && fflush(f) == 0) (void)ftruncate(fileno(f), at);
        capture_failed(p->link);
        return;
    }
    s_stats.packets++;
    if (++s_cap[p->link].unflushed >= FLUSH_EVERY) {
        /* A buffered write "succeeds" long after a card is gone: the flush is where its absence shows. */
        if (fflush(f) != 0) capture_failed(p->link);
        s_cap[p->link].unflushed = 0;
    }
}

void sentinel_store_flush(void)
{
    for (int i = 0; i < SENTINEL_LINK_N; i++) {
        if (s_cap[i].f && s_cap[i].unflushed) {
            if (fflush(s_cap[i].f) != 0) capture_failed((sentinel_link_t)i);
            s_cap[i].unflushed = 0;
        }
    }
    drive_sync();
}

void sentinel_store_close(void)
{
    for (int i = 0; i < SENTINEL_LINK_N; i++) {
        if (s_cap[i].f) fclose(s_cap[i].f);
        s_cap[i].f        = NULL;
        s_cap[i].retry_at = 0; /* a new session tries the card afresh */
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

/* ---- A wardrive's files --------------------------------------------------- */

static void drive_sync(void)
{
    bool failed = false;
    for (int i = 0; i < SENTINEL_DRIVE_FILES; i++) {
        if (!s_drive.f[i] || !s_drive.unsynced[i]) continue;
        /* fflush hands the bytes to the file system; only fsync makes FAT
         * write the file's new length, which is what a power cut would lose.
         * A buffered write "succeeds" long after a card is gone: this is
         * where its absence shows. */
        if (fflush(s_drive.f[i]) != 0 || fsync(fileno(s_drive.f[i])) != 0) failed = true;
        s_drive.unsynced[i] = false;
    }
    s_drive.synced = time(NULL);
    if (failed) {
        s_stats.drive_dropped++; /* at least what was waiting in the buffer */
        sentinel_store_drive_close();
    }
}

/* Append, never truncate (see capture_open); only an empty file gets `head`. */
static FILE *drive_file(const char *dir, const char *name, const char *head, size_t head_n)
{
    char path[PATH_MAX_];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "a");
    if (!f) return NULL;
    struct stat st;
    if ((stat(path, &st) != 0 || st.st_size == 0) && fwrite(head, 1, head_n, f) != head_n) {
        fclose(f);
        return NULL;
    }
    return f;
}

bool sentinel_store_drive_open(const sentinel_wigle_id_t *id)
{
    char      dir[DIR_MAX], wigle[32], survey[32], head[400];
    struct tm t;
    sentinel_store_drive_close();
    local_now(&t, NULL);
    int head_n = sentinel_wigle_head(head, sizeof(head), id);
    if (!hal_storage_is_sd() || !store_dir(dir, sizeof(dir)) || head_n <= 0 ||
        !sentinel_log_drive_name(wigle, sizeof(wigle), "wigle", &t) ||
        !sentinel_log_drive_name(survey, sizeof(survey), "survey", &t))
        return false;
    s_drive.f[SENTINEL_DRIVE_WIGLE]  = drive_file(dir, wigle, head, (size_t)head_n);
    s_drive.f[SENTINEL_DRIVE_SURVEY] = drive_file(dir, survey, SENTINEL_SURVEY_HEADER, strlen(SENTINEL_SURVEY_HEADER));
    if (!s_drive.f[SENTINEL_DRIVE_WIGLE] || !s_drive.f[SENTINEL_DRIVE_SURVEY]) {
        sentinel_store_drive_close();
        return false;
    }
    s_drive.unsynced[SENTINEL_DRIVE_WIGLE] = s_drive.unsynced[SENTINEL_DRIVE_SURVEY] = true; /* the headers */
    drive_sync();
    if (!s_drive.f[SENTINEL_DRIVE_WIGLE] || !s_drive.f[SENTINEL_DRIVE_SURVEY]) return false; /* the headers never reached the card: no drive */
    snprintf(s_stats.drive_file, sizeof(s_stats.drive_file), "%s", wigle);
    s_stats.drive_open = true;
    return true;
}

void sentinel_store_drive_row(sentinel_drive_file_t which, const char *row, size_t len)
{
    if ((int)which < 0 || which >= SENTINEL_DRIVE_FILES || !s_drive.f[which]) {
        s_stats.drive_dropped++;
        return;
    }
    if (fwrite(row, 1, len, s_drive.f[which]) != len) {
        /* The card filled up or went away: the drive's files are done. */
        sentinel_store_drive_close();
        s_stats.drive_dropped++;
        return;
    }
    s_stats.drive_rows++;
    s_drive.unsynced[which] = true;
    /* (A clock set backwards, by NTP say, must not put the next sync hours away.) */
    time_t now = time(NULL);
    if (now - s_drive.synced >= DRIVE_SYNC_S || now < s_drive.synced) drive_sync();
}

void sentinel_store_drive_close(void)
{
    for (int i = 0; i < SENTINEL_DRIVE_FILES; i++) {
        if (s_drive.f[i]) fclose(s_drive.f[i]);
        s_drive.f[i]        = NULL;
        s_drive.unsynced[i] = false;
    }
    s_stats.drive_open = false;
}

bool sentinel_store_site(const sentinel_bandhost_t *h, const char *time_text, const sentinel_where_t *w, const char *tag,
                         uint32_t now_ms, char *name, size_t name_n)
{
    char      dir[DIR_MAX], file[SENTINEL_SITE_NAME_MAX], path[PATH_MAX_], line[SENTINEL_SITE_LINE_MAX];
    struct tm t;
    time_t    now = time(NULL);
    localtime_r(&now, &t);
    if (h->sub_stat[0].n == 0 && h->nrf_stat[0].n == 0) return false; /* nothing was swept: there is no survey */
    if (!store_dir(dir, sizeof(dir)) || !sentinel_site_name(file, sizeof(file), &t, tag)) return false;
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    bool ok  = fputs("time,lat,lon,seconds,sub_sweeps,nrf_reports,site\n", f) >= 0;
    int  len = sentinel_site_head(line, sizeof(line), h, time_text, w, tag, now_ms);
    ok       = ok && len > 0 && fwrite(line, 1, (size_t)len, f) == (size_t)len;
    ok       = ok && fputs("band,mhz,floor,mean,peak,busy_pct\n", f) >= 0;
    for (int i = 0; ok && i < SENTINEL_SUB_CH; i++) {
        len = sentinel_site_sub_row(line, sizeof(line), h, i);
        ok  = len > 0 && fwrite(line, 1, (size_t)len, f) == (size_t)len;
    }
    for (int i = 0; ok && i < SENTINEL_NRF_CH; i++) {
        len = sentinel_site_nrf_row(line, sizeof(line), h, i);
        ok  = len > 0 && fwrite(line, 1, (size_t)len, f) == (size_t)len;
    }
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(path); /* half a survey is worse than none */
        return false;
    }
    snprintf(name, name_n, "%s", file);
    return true;
}

bool sentinel_store_site_from(const sentinel_bandhost_t *h, const sentinel_state_t *s, const char *tag, uint32_t now_ms, char *name,
                              size_t name_n)
{
    sentinel_where_t w;
    char             text[32];
    bool             placed = sentinel_where(&w, s, now_ms);
    if (placed) {
        snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)w.year, (unsigned)w.mon, (unsigned)w.day,
                 (unsigned)w.hour, (unsigned)w.min, (unsigned)w.sec);
    } else {
        time_t    now = time(NULL);
        struct tm t;
        gmtime_r(&now, &t);
        strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &t);
    }
    return sentinel_store_site(h, text, placed ? &w : NULL, tag, now_ms, name, name_n);
}
