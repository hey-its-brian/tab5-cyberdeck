#include "deck_sync.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "deck_hal.h"
#include "deck_net.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "sync";
#else
#include <pthread.h>
#endif

#define LOG_LINES 24

static const deck_sync_backend_t *s_backend;
static volatile sync_state_t s_state = SYNC_IDLE;
static volatile bool s_cancel;
static char s_lines[LOG_LINES][96];
static int s_count;
static time_t s_last_ok;
static char s_dir[128];
static bool s_full;

void deck_sync_set_backend(const deck_sync_backend_t *backend) { s_backend = backend; }
const deck_sync_backend_t *deck_sync_backend(void) { return s_backend; }

bool deck_sync_configured(void) { return s_backend && s_backend->configured(); }

static void log_line(const char *line)
{
    if (s_count == LOG_LINES) {
        memmove(s_lines[0], s_lines[1], sizeof(s_lines) - sizeof(s_lines[0]));
        s_count--;
    }
    snprintf(s_lines[s_count++], sizeof(s_lines[0]), "%s", line);
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "%s", line);
#else
    fprintf(stderr, "[sync] %s\n", line);
#endif
}

static void run(void)
{
    bool ok = s_backend->sync(s_dir, log_line, &s_cancel, s_full);
    net_http_idle();
#ifdef ESP_PLATFORM
    char buf[64];
    snprintf(buf, sizeof(buf), "stack headroom %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    log_line(buf);
#endif
    if (ok) s_last_ok = time(NULL);
    s_state = ok ? SYNC_DONE : SYNC_FAILED;
}

#ifdef ESP_PLATFORM
static void worker(void *arg)
{
    (void)arg;
    run();
    vTaskDelete(NULL);
}
#else
static void *worker(void *arg)
{
    (void)arg;
    run();
    return NULL;
}
#endif

static bool start(bool full);

bool deck_sync_start(void) { return start(false); }
bool deck_sync_start_full(void) { return start(true); }

static bool start(bool full)
{
    if (s_state == SYNC_RUNNING || !deck_sync_configured()) return false;
    s_full = full;
    if (net_state() != NET_CONNECTED) {
        log_line("no network");
        s_state = SYNC_FAILED;
        return false;
    }
    const char *root = hal_storage_root();
    if (root == NULL) {
        log_line("no storage for the vault");
        s_state = SYNC_FAILED;
        return false;
    }
    snprintf(s_dir, sizeof(s_dir), "%s/vault", root);
    s_cancel = false;
    s_state  = SYNC_RUNNING;
#ifdef ESP_PLATFORM
    /* TLS, JSON and a note's chunks in flight: a roomy stack. It has to be
     * internal RAM: the task reads settings, and flash access from a PSRAM
     * stack trips the cache-disabled assert. */
    if (xTaskCreatePinnedToCore(worker, "sync", 40960, NULL, 3, NULL, 0) != pdPASS) {
        log_line("could not start the sync task");
        s_state = SYNC_FAILED;
        return false;
    }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) {
        s_state = SYNC_FAILED;
        return false;
    }
    pthread_detach(t);
#endif
    return true;
}

void deck_sync_cancel(void) { s_cancel = true; }

sync_state_t deck_sync_state(void) { return s_state; }

long deck_sync_age(void) { return s_last_ok ? (long)(time(NULL) - s_last_ok) : -1; }

int deck_sync_log(char (*lines)[96], int max)
{
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) memcpy(lines[i], s_lines[s_count - n + i], 96);
    return n;
}

const char *deck_sync_last(void) { return s_count ? s_lines[s_count - 1] : ""; }
