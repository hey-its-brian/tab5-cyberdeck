#include "deck_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "deck_net.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define REPO "hey-its-brian/tab5-cyberdeck"
#define URL_LATEST "https://api.github.com/repos/" REPO "/releases/latest"
#define URL_LIST "https://api.github.com/repos/" REPO "/releases?per_page=8"
#define NOTES_MAX 2048

static volatile ota_state_t s_state = OTA_IDLE;
static volatile int s_progress;
static char s_running[24];
static char s_latest[24];
static char s_notes[NOTES_MAX];
static char s_error[96];
static char s_asset_url[384];
static bool s_just_updated;
static volatile bool s_pending;
static bool s_prerelease;

static void fail(const char *msg)
{
    snprintf(s_error, sizeof(s_error), "%s", msg);
    ESP_LOGW(TAG, "%s", msg);
    s_state = OTA_ERROR;
}

/* ---- Boot: confirm or roll back ----------------------------------------- */

static void confirm_cb(void *arg)
{
    (void)arg;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending = false;
        ESP_LOGI(TAG, "build %s confirmed good", s_running);
    }
}

void ota_init(const char *running_version)
{
    snprintf(s_running, sizeof(s_running), "%s", running_version);
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        /* First boot of an OTA build. If anything resets us before the timer
         * fires, the bootloader marks this slot invalid and boots the old one. */
        s_pending      = true;
        s_just_updated = true;
        ESP_LOGW(TAG, "new build %s on %s: confirming in %d s", s_running, run->label, OTA_CONFIRM_S);
        const esp_timer_create_args_t a = {.callback = confirm_cb, .name = "ota_confirm"};
        esp_timer_handle_t t;
        if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, (uint64_t)OTA_CONFIRM_S * 1000000ULL);
    }
}

/* ---- Check --------------------------------------------------------------- */

/* Pull what we need out of one release object. */
static bool read_release(const cJSON *rel)
{
    const cJSON *tag    = cJSON_GetObjectItemCaseSensitive(rel, "tag_name");
    const cJSON *body   = cJSON_GetObjectItemCaseSensitive(rel, "body");
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(rel, "assets");
    if (!cJSON_IsString(tag)) return false;

    const char *t = tag->valuestring;
    snprintf(s_latest, sizeof(s_latest), "%s", (t[0] == 'v' || t[0] == 'V') ? t + 1 : t);
    snprintf(s_notes, sizeof(s_notes), "%s", cJSON_IsString(body) ? body->valuestring : "");
    s_prerelease = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rel, "prerelease"));

    s_asset_url[0] = '\0';
    const cJSON *a;
    cJSON_ArrayForEach(a, assets)
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
        const cJSON *url  = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
        if (cJSON_IsString(name) && cJSON_IsString(url)) {
            size_t n = strlen(name->valuestring);
            if (n > 8 && strcmp(name->valuestring + n - 8, "-ota.bin") == 0) {
                snprintf(s_asset_url, sizeof(s_asset_url), "%s", url->valuestring);
                break;
            }
        }
    }
    return true;
}

static void check_task(void *arg)
{
    bool pre    = (bool)(intptr_t)arg;
    size_t len  = 0;
    char *body  = net_http_get(pre ? URL_LIST : URL_LATEST, &len);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    free(body);

    bool ok = false;
    if (root && cJSON_IsArray(root)) {
        /* Newest first; skip drafts. */
        const cJSON *rel;
        cJSON_ArrayForEach(rel, root)
        {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rel, "draft"))) continue;
            ok = read_release(rel);
            break;
        }
    } else if (root) {
        ok = read_release(root);
    }
    cJSON_Delete(root);

    if (!ok) {
        fail(net_state() == NET_CONNECTED ? "could not read releases from GitHub" : "no network");
    } else if (ota_version_cmp(s_latest, s_running) <= 0) {
        ESP_LOGI(TAG, "up to date: running %s, latest release %s", s_running, s_latest);
        s_state = OTA_UP_TO_DATE;
    } else if (s_asset_url[0] == '\0') {
        fail("newer release has no OTA image (-ota.bin)");
    } else {
        ESP_LOGI(TAG, "update available: %s -> %s%s", s_running, s_latest, s_prerelease ? " (pre-release)" : "");
        s_state = OTA_AVAILABLE;
    }
    vTaskDelete(NULL);
}

void ota_check(bool include_prereleases)
{
    if (s_state == OTA_CHECKING || s_state == OTA_DOWNLOADING || s_state == OTA_REBOOTING) return;
    if (net_state() != NET_CONNECTED) {
        fail("no network: join Wi-Fi first");
        return;
    }
    s_state = OTA_CHECKING;
    if (xTaskCreate(check_task, "ota_check", 8192, (void *)(intptr_t)include_prereleases, 4, NULL) != pdPASS) {
        fail("out of memory");
    }
}

/* ---- Install ------------------------------------------------------------- */

static void install_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t http = {0};
    http.url               = s_asset_url;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms        = 20000;
    http.buffer_size       = 4096;
    http.buffer_size_tx    = 2048; /* GitHub's redirect URLs are long */
    http.keep_alive_enable = true;

    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h   = NULL;
    if (esp_https_ota_begin(&cfg, &h) != ESP_OK) {
        fail("download failed to start");
        vTaskDelete(NULL);
        return;
    }

    /* Refuse anything that is not DECK//OS before a byte is written. */
    esp_app_desc_t desc;
    const esp_app_desc_t *self = esp_app_get_description();
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK || strcmp(desc.project_name, self->project_name) != 0) {
        esp_https_ota_abort(h);
        fail("image is not DECK//OS firmware");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "installing %s %s", desc.project_name, desc.version);

    esp_err_t err;
    int total = esp_https_ota_get_image_size(h);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        if (total > 0) s_progress = (int)((int64_t)done * 100 / total);
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        fail("download interrupted");
        vTaskDelete(NULL);
        return;
    }
    if (esp_https_ota_finish(h) != ESP_OK) { /* validates the image, sets the boot slot */
        fail("image failed verification");
        vTaskDelete(NULL);
        return;
    }
    s_progress = 100;
    s_state    = OTA_REBOOTING;
    ESP_LOGW(TAG, "update written, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void ota_install(void)
{
    if (s_state != OTA_AVAILABLE) return;
    s_progress = 0;
    s_state    = OTA_DOWNLOADING;
    if (xTaskCreate(install_task, "ota_install", 10240, NULL, 5, NULL) != pdPASS) fail("out of memory");
}

/* ---- Queries ------------------------------------------------------------- */

ota_state_t ota_state(void) { return s_state; }
const char *ota_running(void) { return s_running; }
const char *ota_latest(void) { return s_latest; }
const char *ota_notes(void) { return s_notes; }
const char *ota_error(void) { return s_error; }
int ota_progress(void) { return s_progress; }
bool ota_just_updated(void) { return s_just_updated; }
bool ota_pending_verify(void) { return s_pending; }
