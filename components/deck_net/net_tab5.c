/*
 * Wi-Fi on the Tab5: the radio is an ESP32-C6 running M5Stack's ESP-Hosted
 * slave firmware, reached over SDIO. esp_wifi_remote makes the usual
 * esp_wifi_* API work across that link, so this reads like ordinary
 * ESP-IDF station code. The first esp_wifi_init() resets the C6 and waits
 * for it, which can take a few seconds, so everything runs on a task.
 */
#include "deck_net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "deck_hal.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net";

#define SCAN_MAX 24
#define HTTP_MAX (128 * 1024)

static volatile net_state_t s_state = NET_STARTING;
static esp_netif_t *s_sta;
static char s_ssid[33];
static char s_ip[16];
static int s_fails;
static esp_timer_handle_t s_retry;
static volatile bool s_ready;
static volatile bool s_synced;
static bool s_sntp_started;

static net_ap_t s_scan[SCAN_MAX];
static size_t s_scan_n;
static volatile bool s_scan_done = true;

/* ---- Clock --------------------------------------------------------------- */

static void time_synced(struct timeval *tv)
{
    (void)tv;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    hal_rtc_set(&local); /* keep the RTC right for boots without Wi-Fi */
    s_synced = true;
    ESP_LOGI(TAG, "clock synced: %04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900, local.tm_mon + 1,
             local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

static void sntp_start(void)
{
    if (s_sntp_started) {
        esp_sntp_restart();
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    cfg.sync_cb           = time_synced;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp_started = true;
}

/* ---- Wi-Fi events -------------------------------------------------------- */

static void retry_cb(void *arg)
{
    (void)arg;
    if (s_state == NET_CONNECTING && s_ssid[0]) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            s_state = NET_CONNECTING;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        if (!s_ssid[0]) {
            s_state = NET_IDLE;
            return;
        }
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        s_state                           = NET_CONNECTING;
        s_fails++;
        int delay_s = 1 << (s_fails < 5 ? s_fails : 5); /* 2, 4, 8, 16, 32 */
        if (delay_s > 30) delay_s = 30;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %ds", ev ? ev->reason : -1, delay_s);
        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, (uint64_t)delay_s * 1000000ULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n             = SCAN_MAX;
        wifi_ap_record_t *recs = calloc(SCAN_MAX, sizeof(*recs));
        s_scan_n               = 0;
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (uint16_t i = 0; i < n; i++) {
                if (recs[i].ssid[0] == '\0') continue;
                bool dup = false; /* mesh networks repeat; records come strongest first */
                for (size_t j = 0; j < s_scan_n && !dup; j++) dup = !strcmp(s_scan[j].ssid, (char *)recs[i].ssid);
                if (dup) continue;
                net_ap_t *ap = &s_scan[s_scan_n++];
                strlcpy(ap->ssid, (char *)recs[i].ssid, sizeof(ap->ssid));
                ap->rssi   = recs[i].rssi;
                ap->secure = recs[i].authmode != WIFI_AUTH_OPEN;
            }
        }
        free(recs);
        ESP_LOGI(TAG, "scan: %d networks%s%s", (int)s_scan_n, s_scan_n ? ", strongest " : "",
                 s_scan_n ? s_scan[0].ssid : "");
        s_scan_done = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_state = NET_CONNECTED;
        s_fails = 0;
        ESP_LOGI(TAG, "connected to '%s', IP %s", s_ssid, s_ip);
        sntp_start();
    }
}

static void apply_config(const char *ssid, const char *pass)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;
    sta.sta.pmf_cfg.capable    = true;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
}

/* ---- Startup ------------------------------------------------------------- */

static void net_task(void *arg)
{
    (void)arg;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err          = esp_wifi_init(&cfg); /* resets the C6 and waits for the SDIO link */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi radio (ESP32-C6) not available: %s", esp_err_to_name(err));
        s_state = NET_NO_RADIO;
        vTaskDelete(NULL);
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM); /* credentials live in our own NVS */
    esp_wifi_set_mode(WIFI_MODE_STA);

    char pass[65] = "";
    hal_cfg_get_str("wifi_ssid", s_ssid, sizeof(s_ssid));
    hal_cfg_get_str("wifi_pass", pass, sizeof(pass));
    if (s_ssid[0]) apply_config(s_ssid, pass);
    s_state = s_ssid[0] ? NET_CONNECTING : NET_IDLE;
    s_ready = true;
    esp_wifi_start(); /* STA_START then connects if configured */
#ifdef DECK_NET_BOOT_SCAN
    net_scan_start(); /* bring-up check: proves the C6 link end to end */
#endif
    vTaskDelete(NULL);
}

void net_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, "deck");

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    esp_timer_create(&targs, &s_retry);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    xTaskCreatePinnedToCore(net_task, "net", 6144, NULL, 5, NULL, 0);
}

/* ---- Queries and control ------------------------------------------------- */

net_state_t net_state(void) { return s_state; }

const char *net_state_name(net_state_t s)
{
    switch (s) {
        case NET_STARTING: return "STARTING";
        case NET_NO_RADIO: return "NO RADIO";
        case NET_IDLE: return "NOT SET UP";
        case NET_CONNECTING: return "CONNECTING";
        case NET_CONNECTED: return "ONLINE";
    }
    return "?";
}

const char *net_ssid(void) { return s_ssid; }
const char *net_ip(void) { return s_ip; }

int net_rssi(void)
{
    int rssi = 0;
    if (s_state == NET_CONNECTED && esp_wifi_sta_get_rssi(&rssi) == ESP_OK) return rssi;
    return 0;
}

int net_bars(void)
{
    int r = net_rssi();
    if (r == 0) return 0;
    return r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
}

void net_connect(const char *ssid, const char *pass)
{
    hal_cfg_set_str("wifi_ssid", ssid);
    hal_cfg_set_str("wifi_pass", pass);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    if (!s_ready) return; /* net_task applies it once the radio is up */
    s_fails = 0;
    s_state = NET_CONNECTING;
    esp_wifi_disconnect();
    apply_config(ssid, pass);
    esp_wifi_connect();
}

void net_forget(void)
{
    hal_cfg_set_str("wifi_ssid", NULL);
    hal_cfg_set_str("wifi_pass", NULL);
    s_ssid[0] = '\0';
    if (s_ready) {
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
        s_state = NET_IDLE;
    }
}

bool net_scan_start(void)
{
    if (!s_ready || !s_scan_done) return false;
    s_scan_done = false;
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        s_scan_done = true;
        return false;
    }
    return true;
}

bool net_scan_done(void) { return s_scan_done; }

size_t net_scan_results(net_ap_t *out, size_t max)
{
    size_t n = s_scan_n < max ? s_scan_n : max;
    memcpy(out, s_scan, n * sizeof(net_ap_t));
    return n;
}

bool net_time_synced(void) { return s_synced; }

/* ---- HTTP ---------------------------------------------------------------- */

char *net_http_get(const char *url, size_t *len)
{
    if (s_state != NET_CONNECTED) return NULL;
    esp_http_client_config_t cfg = {0};
    cfg.url                      = url;
    cfg.timeout_ms               = 10000;
    cfg.crt_bundle_attach        = esp_crt_bundle_attach;
    cfg.buffer_size              = 2048;
    esp_http_client_handle_t c   = esp_http_client_init(&cfg);
    if (c == NULL) return NULL;

    char *buf = NULL;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        size_t cap = 8192, n = 0;
        buf        = malloc(cap);
        while (buf && status == 200) {
            if (n + 1024 >= cap) {
                if (cap >= HTTP_MAX) break;
                char *nb = realloc(buf, cap * 2);
                if (nb == NULL) break;
                buf = nb;
                cap *= 2;
            }
            int r = esp_http_client_read(c, buf + n, (int)(cap - n - 1));
            if (r <= 0) break;
            n += (size_t)r;
        }
        if (buf && status == 200 && n > 0) {
            buf[n] = '\0';
            if (len) *len = n;
        } else {
            ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
            free(buf);
            buf = NULL;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}
