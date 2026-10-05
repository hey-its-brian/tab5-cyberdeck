/*
 * File portal server (esp_http_server). The browser page (www/index.html)
 * talks to a small JSON API:
 *
 *   POST /api/login       body "pin=123456"  -> session cookie
 *   GET  /api/info        storage type and free space
 *   GET  /api/list?path=  directory listing
 *   GET  /api/file?path=  download
 *   POST /api/upload?path= raw file body (streamed to storage)
 *   POST /api/mkdir?path=   /api/delete?path=   /api/rename?from=&to=
 *
 * Paths are relative to the storage root, start with "/", and may not
 * contain ".." or backslashes. Every /api call except login needs the cookie.
 */
#include "deck_portal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "deck_hal.h"
#include "deck_net.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "portal";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define PATH_MAX_LEN 192
#define IO_BUF (32 * 1024)
#define EVENTS 32
#define MAX_PIN_FAILS 5

static httpd_handle_t s_server;
static char s_pin[7];
static char s_token[33];
static int s_pin_fails;
static int64_t s_lock_until;
static volatile int64_t s_last_req;
static volatile bool s_busy;
static bool s_mdns_up;

static SemaphoreHandle_t s_ev_lock;
static portal_event_t s_ev[EVENTS];
static uint32_t s_ev_count; /* total ever logged; index = n % EVENTS */

/* ---- Events -------------------------------------------------------------- */

static void event(bool error, const char *fmt, ...)
{
    if (s_ev_lock == NULL) return;
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    portal_event_t *e = &s_ev[s_ev_count % EVENTS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
    e->error = error;
    s_ev_count++;
    xSemaphoreGive(s_ev_lock);
    ESP_LOGI(TAG, "%s", e->text);
}

int portal_events(portal_event_t *out, int max, uint32_t *cursor)
{
    if (s_ev_lock == NULL) return 0;
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    if (s_ev_count - *cursor > EVENTS) *cursor = s_ev_count - EVENTS; /* fell behind: skip */
    int n = 0;
    while (*cursor < s_ev_count && n < max) out[n++] = s_ev[(*cursor)++ % EVENTS];
    xSemaphoreGive(s_ev_lock);
    return n;
}

/* ---- Helpers ------------------------------------------------------------- */

static void touch(void) { s_last_req = esp_timer_get_time(); }

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            *o++        = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Read query parameter `key` into out (decoded). */
static bool query(httpd_req_t *req, const char *key, char *out, size_t n)
{
    char q[512];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, out, n) != ESP_OK) return false;
    url_decode(out);
    return true;
}

/* Validate a client path and map it under the storage root. */
static bool full_path(const char *rel, char *out, size_t n)
{
    const char *root = hal_storage_root();
    if (root == NULL || rel[0] != '/' || strlen(rel) >= PATH_MAX_LEN) return false;
    if (strstr(rel, "..") || strchr(rel, '\\') || strstr(rel, "//")) return false;
    size_t len = strlen(rel);
    snprintf(out, n, "%s%.*s", root, (int)(len > 1 && rel[len - 1] == '/' ? len - 1 : len), rel);
    return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, buf);
}

static bool authed(httpd_req_t *req)
{
    char cookie[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    char want[48];
    snprintf(want, sizeof(want), "deck=%s", s_token);
    return strstr(cookie, want) != NULL;
}

#define REQUIRE_AUTH(req)                                       \
    do {                                                        \
        touch();                                                \
        if (!authed(req)) return fail(req, "401 Unauthorized", "login"); \
    } while (0)

/* ---- Handlers ------------------------------------------------------------ */

static esp_err_t h_index(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_login(httpd_req_t *req)
{
    touch();
    if (esp_timer_get_time() < s_lock_until) return fail(req, "429 Too Many Requests", "locked, wait 30 s");
    char body[32] = {0};
    int n         = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) return fail(req, "400 Bad Request", "no pin");
    body[n]         = '\0';
    const char *pin = strstr(body, "pin=");
    if (pin && strncmp(pin + 4, s_pin, 6) == 0 && strlen(pin + 4) >= 6) {
        s_pin_fails = 0;
        char cookie[96];
        snprintf(cookie, sizeof(cookie), "deck=%s; Path=/; HttpOnly; SameSite=Strict", s_token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        event(false, "login from browser");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (++s_pin_fails >= MAX_PIN_FAILS) {
        s_pin_fails  = 0;
        s_lock_until = esp_timer_get_time() + 30 * 1000000LL;
        event(true, "too many wrong PINs, locked 30 s");
    }
    return fail(req, "403 Forbidden", "wrong pin");
}

static esp_err_t h_info(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "storage", hal_storage_is_sd() ? "SD CARD" : "INTERNAL FLASH");
    cJSON_AddNumberToObject(j, "free", (double)hal_sd_free_bytes());
    cJSON_AddNumberToObject(j, "total", (double)hal_sd_total_bytes());
    return send_json(req, j);
}

static esp_err_t h_list(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char rel[PATH_MAX_LEN] = "/", path[256];
    query(req, "path", rel, sizeof(rel));
    if (!full_path(rel, path, sizeof(path))) return fail(req, "400 Bad Request", "bad path");
    DIR *d = opendir(path);
    if (d == NULL) return fail(req, "404 Not Found", "no such folder");

    cJSON *j   = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(j, "entries");
    struct dirent *e;
    char child[512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode) && strcmp(e->d_name, "weather.json") == 0 && strcmp(rel, "/") == 0) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", e->d_name);
        cJSON_AddBoolToObject(o, "dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(o, "size", (double)st.st_size);
        cJSON_AddNumberToObject(o, "mtime", (double)st.st_mtime);
        cJSON_AddItemToArray(arr, o);
    }
    closedir(d);
    return send_json(req, j);
}

static esp_err_t h_file(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char rel[PATH_MAX_LEN], path[256];
    if (!query(req, "path", rel, sizeof(rel)) || !full_path(rel, path, sizeof(path))) {
        return fail(req, "400 Bad Request", "bad path");
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) return fail(req, "404 Not Found", "no such file");

    const char *name = strrchr(rel, '/') + 1;
    char disp[PATH_MAX_LEN + 48];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char *buf = malloc(IO_BUF);
    size_t n;
    esp_err_t err = ESP_OK;
    s_busy        = true;
    while (buf && (n = fread(buf, 1, IO_BUF, f)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, (ssize_t)n)) != ESP_OK) break;
        touch();
    }
    s_busy = false;
    fclose(f);
    free(buf);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        event(false, "sent %s", name);
    }
    return err;
}

static esp_err_t h_upload(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char rel[PATH_MAX_LEN], path[256], tmp[264];
    if (!query(req, "path", rel, sizeof(rel)) || !full_path(rel, path, sizeof(path)) || strlen(rel) < 2) {
        return fail(req, "400 Bad Request", "bad path");
    }
    uint64_t free_b = hal_sd_free_bytes();
    if (free_b && req->content_len + 65536 > free_b) return fail(req, "507 Insufficient Storage", "storage full");

    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return fail(req, "500 Internal Server Error", "cannot create file");
    char *buf      = malloc(IO_BUF);
    char *fbuf     = malloc(IO_BUF);
    if (fbuf) setvbuf(f, fbuf, _IOFBF, IO_BUF);
    size_t left    = req->content_len;
    bool ok        = buf != NULL;
    const char *name = strrchr(rel, '/') + 1;
    s_busy         = true;
    while (ok && left > 0) {
        int r = httpd_req_recv(req, buf, left < IO_BUF ? left : IO_BUF);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || fwrite(buf, 1, (size_t)r, f) != (size_t)r) ok = false;
        left -= r > 0 ? (size_t)r : 0;
        touch();
    }
    ok = (fclose(f) == 0) && ok;
    s_busy = false;
    free(buf);
    free(fbuf);
    if (!ok) {
        unlink(tmp);
        event(true, "upload failed: %s", name);
        return fail(req, "500 Internal Server Error", "write failed");
    }
    unlink(path); /* FAT rename will not replace */
    rename(tmp, path);
    event(false, "received %s (%u KB)", name, (unsigned)(req->content_len / 1024));
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_mkdir(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char rel[PATH_MAX_LEN], path[256];
    if (!query(req, "path", rel, sizeof(rel)) || !full_path(rel, path, sizeof(path))) {
        return fail(req, "400 Bad Request", "bad path");
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return fail(req, "500 Internal Server Error", "mkdir failed");
    event(false, "new folder %s", rel);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Recursive delete, bounded in depth. */
static bool remove_tree(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    if (depth > 8) return false;
    DIR *d = opendir(path);
    if (d == NULL) return false;
    struct dirent *e;
    char child[512];
    bool ok = true;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        ok = remove_tree(child, depth + 1) && ok;
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

static esp_err_t h_delete(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char rel[PATH_MAX_LEN], path[256];
    if (!query(req, "path", rel, sizeof(rel)) || !full_path(rel, path, sizeof(path)) || strlen(rel) < 2) {
        return fail(req, "400 Bad Request", "bad path");
    }
    if (!remove_tree(path, 0)) return fail(req, "500 Internal Server Error", "delete failed");
    event(false, "deleted %s", rel);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_rename(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN], pa[256], pb[256];
    if (!query(req, "from", a, sizeof(a)) || !query(req, "to", b, sizeof(b)) || !full_path(a, pa, sizeof(pa)) ||
        !full_path(b, pb, sizeof(pb))) {
        return fail(req, "400 Bad Request", "bad path");
    }
    struct stat st;
    if (stat(pb, &st) == 0) return fail(req, "409 Conflict", "name already exists");
    if (rename(pa, pb) != 0) return fail(req, "500 Internal Server Error", "rename failed");
    event(false, "renamed %s", strrchr(b, '/') + 1);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---- Lifecycle ----------------------------------------------------------- */

static void ensure_folders(void)
{
    const char *root = hal_storage_root();
    char p[96];
    static const char *dirs[] = {"notes", "music"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", root, dirs[i]);
        mkdir(p, 0755);
    }
}

bool portal_start(void)
{
    if (s_server) return true;
    if (hal_storage_root() == NULL || net_state() != NET_CONNECTED) return false;
    if (s_ev_lock == NULL) s_ev_lock = xSemaphoreCreateMutex();

    snprintf(s_pin, sizeof(s_pin), "%06lu", (unsigned long)(esp_random() % 1000000));
    for (int i = 0; i < 16; i++) snprintf(s_token + i * 2, 3, "%02x", (unsigned)(esp_random() & 0xFF));
    s_pin_fails  = 0;
    s_lock_until = 0;
    ensure_folders();

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.max_uri_handlers  = 12;
    cfg.stack_size        = 10240;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    cfg.max_open_sockets  = 5;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return false;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/login", .method = HTTP_POST, .handler = h_login},
        {.uri = "/api/info", .method = HTTP_GET, .handler = h_info},
        {.uri = "/api/list", .method = HTTP_GET, .handler = h_list},
        {.uri = "/api/file", .method = HTTP_GET, .handler = h_file},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/api/mkdir", .method = HTTP_POST, .handler = h_mkdir},
        {.uri = "/api/delete", .method = HTTP_POST, .handler = h_delete},
        {.uri = "/api/rename", .method = HTTP_POST, .handler = h_rename},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_server, &uris[i]);

    if (!s_mdns_up && mdns_init() == ESP_OK) {
        mdns_hostname_set("deck");
        mdns_instance_name_set("DECK//OS file portal");
        s_mdns_up = true;
    }
    if (s_mdns_up) mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    touch();
    event(false, "portal up at http://%s", net_ip());
    return true;
}

void portal_stop(void)
{
    if (s_server == NULL) return;
    httpd_stop(s_server);
    s_server = NULL;
    if (s_mdns_up) mdns_service_remove("_http", "_tcp");
    s_token[0] = '\0';
    event(false, "portal down");
}

bool portal_running(void) { return s_server != NULL; }
const char *portal_pin(void) { return s_pin; }
const char *portal_host(void) { return "deck.local"; }
uint32_t portal_idle_s(void) { return (uint32_t)((esp_timer_get_time() - s_last_req) / 1000000); }
bool portal_busy(void) { return s_busy; }

bool portal_tick(void)
{
    if (s_server && !s_busy && portal_idle_s() > PORTAL_IDLE_OFF_S) {
        event(false, "idle %d min, shutting down", PORTAL_IDLE_OFF_S / 60);
        portal_stop();
        return true;
    }
    if (s_server && net_state() != NET_CONNECTED) {
        event(true, "network lost, shutting down");
        portal_stop();
        return true;
    }
    return false;
}
