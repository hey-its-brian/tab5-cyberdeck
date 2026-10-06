/*
 * Music player core. A dedicated task (thread in the simulator) owns the
 * open file, the minimp3 decoder and the audio output; the UI talks to it
 * through a small command queue and reads its state.
 *
 * Duration and seeking: from the Xing/Info header when the file has one
 * (VBR), otherwise from the first frame's bitrate (CBR).
 */
#include "deck_audio.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aout.h"
#include "deck_hal.h"
#include "id3.h"
#include "minimp3.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static SemaphoreHandle_t s_lock;
static void lock_init(void) { s_lock = xSemaphoreCreateMutex(); }
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void sleep_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t rnd(void) { return esp_random(); }
#define LOG(...) ESP_LOGI("player", __VA_ARGS__)
#else
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void) {}
static void lock(void) { pthread_mutex_lock(&s_mtx); }
static void unlock(void) { pthread_mutex_unlock(&s_mtx); }
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}
static uint32_t rnd(void) { return (uint32_t)random(); }
#define LOG(...) (printf("[player] " __VA_ARGS__), printf("\n"))
#endif

#define BUF_SIZE (16 * 1024)
#define FFT_N 256

/* ---- Shared state (lock) ------------------------------------------------- */

typedef enum { CMD_PLAY, CMD_TOGGLE, CMD_STOP, CMD_NEXT, CMD_PREV, CMD_SEEK } cmd_t;
typedef struct {
    cmd_t cmd;
    int arg;
} cmd_msg_t;

static char s_dir[192];
static char *s_names[PLAYER_MAX_TRACKS];
static int s_count;
static int s_order[PLAYER_MAX_TRACKS]; /* play order (shuffled or not) */
static cmd_msg_t s_q[8];
static int s_qn;
static char s_title[96], s_artist[96], s_err[64];

static volatile player_state_t s_state = PLAYER_STOPPED;
static volatile int s_index            = -1;
static volatile uint32_t s_pos_ms, s_dur_ms;
static volatile bool s_hp;
static int s_volume = 60;
static bool s_shuffle;
static player_repeat_t s_repeat = REPEAT_ALL;
static uint8_t s_levels[PLAYER_BANDS];

static void post(cmd_t c, int arg)
{
    lock();
    if (s_qn < (int)(sizeof(s_q) / sizeof(s_q[0]))) s_q[s_qn++] = (cmd_msg_t){c, arg};
    unlock();
}

/* ---- Queue --------------------------------------------------------------- */

static bool is_mp3(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".mp3") == 0 && n[0] != '.';
}

static int cmp_names(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

/* Call with the lock held. Shuffled order keeps `first` at the front. */
static void build_order(int first)
{
    for (int i = 0; i < s_count; i++) s_order[i] = i;
    if (!s_shuffle || s_count < 2) return;
    for (int i = s_count - 1; i > 0; i--) {
        int j       = (int)(rnd() % (uint32_t)(i + 1));
        int t       = s_order[i];
        s_order[i]  = s_order[j];
        s_order[j]  = t;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_order[i] == first) {
            s_order[i] = s_order[0];
            s_order[0] = first;
            break;
        }
    }
}

static int order_pos(int index)
{
    for (int i = 0; i < s_count; i++) {
        if (s_order[i] == index) return i;
    }
    return 0;
}

bool player_open_dir(const char *dir, int index)
{
    DIR *d = opendir(dir);
    if (d == NULL) return false;
    char *names[PLAYER_MAX_TRACKS];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < PLAYER_MAX_TRACKS) {
        if (is_mp3(e->d_name)) names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_names);

    post(CMD_STOP, 0);
    lock(); /* the task only touches s_names under the lock */
    for (int i = 0; i < s_count; i++) free(s_names[i]);
    memcpy(s_names, names, sizeof(names[0]) * (size_t)n);
    s_count = n;
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    build_order(index >= 0 && index < n ? index : 0);
    unlock();
    if (index >= 0 && index < n) player_play(index);
    return true;
}

const char *player_dir(void) { return s_dir; }
int player_count(void) { return s_count; }
const char *player_file(int i) { return (i >= 0 && i < s_count) ? s_names[i] : ""; }

/* ---- Controls ------------------------------------------------------------ */

void player_play(int index) { post(CMD_PLAY, index); }
void player_toggle(void) { post(CMD_TOGGLE, 0); }
void player_stop(void) { post(CMD_STOP, 0); }
void player_next(void) { post(CMD_NEXT, 0); }
void player_prev(void) { post(CMD_PREV, 0); }
void player_seek(int delta_s) { post(CMD_SEEK, delta_s); }

void player_set_volume(int v)
{
    s_volume = v < 0 ? 0 : v > 100 ? 100 : v;
    aout_volume(s_volume);
    hal_cfg_set_i32("vol", s_volume);
}
int player_volume(void) { return s_volume; }

void player_set_shuffle(bool on)
{
    lock();
    s_shuffle = on;
    build_order(s_index >= 0 ? s_index : 0);
    unlock();
    hal_cfg_set_i32("shuf", on);
}
bool player_shuffle(void) { return s_shuffle; }
void player_set_repeat(player_repeat_t r)
{
    s_repeat = r;
    hal_cfg_set_i32("rep", r);
}
player_repeat_t player_repeat(void) { return s_repeat; }

player_state_t player_state(void) { return s_state; }
int player_index(void) { return s_index; }
uint32_t player_pos_ms(void) { return s_pos_ms; }
uint32_t player_dur_ms(void) { return s_dur_ms; }
const char *player_title(void) { return s_title; }
const char *player_artist(void) { return s_artist; }
bool player_headphones(void) { return s_hp; }
const char *player_error(void) { return s_err; }

void player_levels(uint8_t out[PLAYER_BANDS])
{
    memcpy(out, s_levels, PLAYER_BANDS);
    for (int i = 0; i < PLAYER_BANDS; i++) s_levels[i] = (uint8_t)(s_levels[i] * 7 / 8); /* decay between frames */
}

/* ---- Spectrum ------------------------------------------------------------ */

/* In-place radix-2 FFT of FFT_N points. */
static void fft(float *re, float *im)
{
    for (int i = 1, j = 0; i < FFT_N; i++) {
        int bit = FFT_N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i]   = re[j];
            re[j]   = t;
            t       = im[i];
            im[i]   = im[j];
            im[j]   = t;
        }
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < FFT_N; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b]    = re[a] - tr;
                im[b]    = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                float n = cr * wr - ci * wi;
                ci      = cr * wi + ci * wr;
                cr      = n;
            }
        }
    }
}

/* 16 roughly log-spaced bands from one frame's first FFT_N samples. */
static void analyse(const int16_t *pcm, int channels)
{
    static float re[FFT_N], im[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
        float s = channels == 2 ? (pcm[2 * i] + pcm[2 * i + 1]) * 0.5f : pcm[i];
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1)); /* Hann */
        re[i]   = s * w / 32768.0f;
        im[i]   = 0;
    }
    fft(re, im);
    static const uint8_t edge[PLAYER_BANDS + 1] = {1, 2, 3, 4, 5, 7, 9, 12, 15, 19, 24, 31, 40, 52, 68, 90, 127};
    for (int b = 0; b < PLAYER_BANDS; b++) {
        float m = 0;
        for (int k = edge[b]; k < edge[b + 1]; k++) {
            float v = sqrtf(re[k] * re[k] + im[k] * im[k]);
            if (v > m) m = v;
        }
        float db = 20.0f * log10f(m + 1e-6f); /* about -60..+20 */
        int lvl  = (int)((db + 50.0f) * 4.0f);
        lvl      = lvl < 0 ? 0 : lvl > 255 ? 255 : lvl;
        if (lvl > s_levels[b]) s_levels[b] = (uint8_t)lvl;
    }
}

/* ---- Decoder task -------------------------------------------------------- */

typedef struct {
    FILE *f;
    id3_info_t tag;
    mp3dec_t dec;
    uint8_t *buf;
    size_t have, pos;
    bool eof;
    uint32_t rate;
    int channels;
    uint64_t samples;  /* decoded since base_ms */
    uint32_t base_ms;  /* position of the last seek */
    uint32_t bytes;    /* audio bytes (for seeking) */
    uint8_t toc[100];  /* Xing seek table */
    bool has_toc;
    bool first;        /* next frame is the first of the file */
} track_t;

static track_t s_t;
static int16_t s_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

static void fill(void)
{
    if (s_t.pos > 0) {
        memmove(s_t.buf, s_t.buf + s_t.pos, s_t.have - s_t.pos);
        s_t.have -= s_t.pos;
        s_t.pos = 0;
    }
    long at    = ftell(s_t.f);
    size_t max = BUF_SIZE - s_t.have;
    if (at >= 0 && (uint32_t)at + max > s_t.tag.audio_end) max = s_t.tag.audio_end > (uint32_t)at ? s_t.tag.audio_end - (uint32_t)at : 0;
    if (max == 0 && s_t.have == BUF_SIZE) return; /* buffer full, not the end */
    size_t n = max ? fread(s_t.buf + s_t.have, 1, max, s_t.f) : 0;
    s_t.have += n;
    if (n == 0) s_t.eof = true;
}

static void close_track(void)
{
    if (s_t.f) fclose(s_t.f);
    s_t.f = NULL;
}

/* Xing/Info header in the first frame: frame count and seek table. */
static bool parse_xing(const uint8_t *fr, size_t len, const mp3dec_frame_info_t *info)
{
    bool mpeg1  = (fr[1] & 0x08) != 0;
    bool mono   = (fr[3] >> 6) == 3;
    size_t off  = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (len < off + 16) return false;
    const uint8_t *x = fr + off;
    if (memcmp(x, "Xing", 4) != 0 && memcmp(x, "Info", 4) != 0) return false;
    uint32_t flags = (uint32_t)x[4] << 24 | x[5] << 16 | x[6] << 8 | x[7];
    const uint8_t *p = x + 8;
    uint32_t frames  = 0;
    if (flags & 1) {
        frames = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
        p += 4;
    }
    if (flags & 2) {
        uint32_t b = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
        if (b) s_t.bytes = b;
        p += 4;
    }
    if ((flags & 4) && p + 100 <= fr + len) {
        memcpy(s_t.toc, p, 100);
        s_t.has_toc = true;
    }
    int spf = mpeg1 ? 1152 : 576;
    if (frames && info->hz) s_dur_ms = (uint32_t)((uint64_t)frames * spf * 1000 / (uint32_t)info->hz);
    return true;
}

static void set_title_from_file(const char *name)
{
    snprintf(s_title, sizeof(s_title), "%.95s", name); /* long names are cut, on purpose */
    char *dot = strrchr(s_title, '.');
    if (dot) *dot = '\0';
}

static bool open_track(int index)
{
    close_track();
    char path[512], name[128];
    lock();
    if (index < 0 || index >= s_count) {
        unlock();
        return false;
    }
    snprintf(path, sizeof(path), "%s/%s", s_dir, s_names[index]);
    snprintf(name, sizeof(name), "%s", s_names[index]);
    unlock();

    s_t.f = fopen(path, "rb");
    if (s_t.f == NULL) {
        snprintf(s_err, sizeof(s_err), "cannot open %.40s", name);
        return false;
    }
    id3_read(s_t.f, &s_t.tag);
    fseek(s_t.f, (long)s_t.tag.audio_start, SEEK_SET);
    mp3dec_init(&s_t.dec);
    s_t.have = s_t.pos = 0;
    s_t.eof            = false;
    s_t.samples        = 0;
    s_t.base_ms        = 0;
    s_t.has_toc        = false;
    s_t.bytes          = s_t.tag.audio_end - s_t.tag.audio_start;
    s_t.first          = true;
    s_dur_ms           = 0;
    s_pos_ms           = 0;
    s_err[0]           = '\0';
    lock();
    if (s_t.tag.title[0]) {
        snprintf(s_title, sizeof(s_title), "%s", s_t.tag.title);
    } else {
        set_title_from_file(name);
    }
    snprintf(s_artist, sizeof(s_artist), "%s", s_t.tag.artist);
    unlock();
    s_index = index;
    LOG("play %s", path);
    return true;
}

static void seek_to(uint32_t ms)
{
    if (s_t.f == NULL || s_dur_ms == 0) return;
    if (ms >= s_dur_ms) ms = s_dur_ms > 1000 ? s_dur_ms - 1000 : 0;
    uint32_t off;
    if (s_t.has_toc) {
        float pct = (float)ms * 100.0f / (float)s_dur_ms;
        int i     = (int)pct;
        if (i > 99) i = 99;
        float a = s_t.toc[i], b = i < 99 ? s_t.toc[i + 1] : 256.0f;
        off     = (uint32_t)((a + (b - a) * (pct - i)) / 256.0f * s_t.bytes);
    } else {
        off = (uint32_t)((uint64_t)ms * s_t.bytes / s_dur_ms);
    }
    fseek(s_t.f, (long)(s_t.tag.audio_start + off), SEEK_SET);
    s_t.have = s_t.pos = 0;
    s_t.eof            = false;
    mp3dec_init(&s_t.dec);
    s_t.samples = 0;
    s_t.base_ms = ms;
    s_pos_ms    = ms;
}

/* Index to play after the current one, or -1 to stop. */
static int following(int step, bool natural)
{
    if (s_count == 0) return -1;
    if (natural && s_repeat == REPEAT_ONE) return s_index;
    lock();
    int p = order_pos(s_index) + step;
    int r = -1;
    if (p >= 0 && p < s_count) {
        r = s_order[p];
    } else if (s_repeat != REPEAT_OFF || !natural) {
        r = s_order[(p + s_count) % s_count];
    }
    unlock();
    return r;
}

static void start(int index)
{
    if (open_track(index)) {
        s_state = PLAYER_PLAYING;
    } else {
        s_state = PLAYER_STOPPED;
    }
}

static void handle(cmd_msg_t m)
{
    switch (m.cmd) {
        case CMD_PLAY: start(m.arg); break;
        case CMD_TOGGLE:
            if (s_state == PLAYER_PLAYING) {
                s_state = PLAYER_PAUSED;
            } else if (s_state == PLAYER_PAUSED && s_t.f) {
                s_state = PLAYER_PLAYING;
            } else if (s_count) {
                start(s_index >= 0 ? s_index : s_order[0]);
            }
            break;
        case CMD_STOP:
            close_track();
            aout_close();
            s_state  = PLAYER_STOPPED;
            s_pos_ms = 0;
            break;
        case CMD_NEXT:
        case CMD_PREV: {
            if (m.cmd == CMD_PREV && s_pos_ms > 3000 && s_t.f) {
                seek_to(0);
                break;
            }
            int n = following(m.cmd == CMD_NEXT ? 1 : -1, false);
            if (n >= 0) start(n);
            break;
        }
        case CMD_SEEK: {
            int64_t t = (int64_t)s_pos_ms + (int64_t)m.arg * 1000;
            seek_to(t < 0 ? 0 : (uint32_t)t);
            break;
        }
    }
}

/* Decode and play one frame. False at the end of the track. */
static bool play_frame(void)
{
    int stalls = 0;
    for (;;) {
        if (s_t.have - s_t.pos < 4096 && !s_t.eof) fill();
        if (s_t.have == s_t.pos) return false;
        mp3dec_frame_info_t info;
        const uint8_t *fr = s_t.buf + s_t.pos;
        int n             = mp3dec_decode_frame(&s_t.dec, fr, (int)(s_t.have - s_t.pos), s_pcm, &info);
        if (info.frame_bytes == 0) {
            /* No frame in what we have: either too little data, or a window
             * of junk. Drop junk, then read more. */
            if (s_t.eof) return false;
            if (s_t.have - s_t.pos >= 4096) s_t.pos = s_t.have;
            fill();
            if (++stalls > 64) return false;
            continue;
        }
        s_t.pos += (size_t)info.frame_bytes;
        if (s_t.first) {
            s_t.first = false;
            bool xing = parse_xing(fr, (size_t)info.frame_bytes, &info);
            if (!s_dur_ms && info.bitrate_kbps) {
                s_dur_ms = (uint32_t)((uint64_t)s_t.bytes * 8 / (uint32_t)info.bitrate_kbps);
            }
            if (xing) continue; /* the info frame is silence */
        }
        if (n == 0) continue;
        if ((uint32_t)info.hz != s_t.rate || info.channels != s_t.channels) {
            s_t.rate     = (uint32_t)info.hz;
            s_t.channels = info.channels;
            if (!aout_open(s_t.rate, s_t.channels)) {
                snprintf(s_err, sizeof(s_err), "audio output failed");
                return false;
            }
        }
        analyse(s_pcm, info.channels);
        aout_write(s_pcm, (size_t)n * (size_t)info.channels); /* n is per channel */
        s_t.samples += (uint64_t)n;
        s_pos_ms = s_t.base_ms + (uint32_t)(s_t.samples * 1000 / s_t.rate);
        return true;
    }
}

static void task(void *arg)
{
    (void)arg;
    uint32_t last_route = 0;
    for (;;) {
        cmd_msg_t m;
        bool have = false;
        lock();
        if (s_qn) {
            m = s_q[0];
            memmove(s_q, s_q + 1, sizeof(s_q[0]) * (size_t)--s_qn);
            have = true;
        }
        unlock();
        if (have) {
            handle(m);
            continue;
        }
        if (now_ms() - last_route > 500) {
            last_route = now_ms();
            s_hp       = hal_headphones();
            aout_route(s_hp);
        }
        if (s_state != PLAYER_PLAYING) {
            if (s_state == PLAYER_STOPPED) s_t.rate = 0; /* reopen on next start */
            sleep_ms(20);
            continue;
        }
        if (!play_frame()) {
            int n = s_err[0] ? following(1, false) : following(1, true);
            if (n >= 0 && !(s_err[0] && n == s_index)) {
                start(n); /* the next track, or this one again (repeat one) */
            } else {
                close_track();
                aout_close();
                s_state  = PLAYER_STOPPED;
                s_pos_ms = 0;
            }
        }
    }
}

#ifndef ESP_PLATFORM
static void *task_thread(void *arg)
{
    task(arg);
    return NULL;
}
#endif

void player_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    lock_init();
    s_volume  = (int)hal_cfg_get_i32("vol", 60);
    s_shuffle = hal_cfg_get_i32("shuf", 0) != 0;
    s_repeat  = (player_repeat_t)hal_cfg_get_i32("rep", REPEAT_ALL);
    aout_volume(s_volume);
    s_t.buf = malloc(BUF_SIZE);
#ifdef ESP_PLATFORM
    /* minimp3 keeps ~16 KB of scratch on the stack; internal RAM, core 1,
     * above the UI so the music keeps going while screens redraw. */
    xTaskCreatePinnedToCore(task, "player", 28 * 1024, NULL, 6, NULL, 1);
#else
    pthread_t t;
    pthread_create(&t, NULL, task_thread, NULL);
    pthread_detach(t);
#endif
}
