/*
 * term_chat over Ollama's HTTP API. A worker task owns the conversation: it
 * reads keys from the TERMINAL through one stream buffer, runs a small line
 * editor and prompt, and streams replies back through another, like the SSH
 * link does.
 *
 * Requests are plain HTTP/1.0 to /api/chat with "stream": true, so the reply
 * is newline-delimited JSON with no chunked encoding and ends when the
 * server closes the connection. Ollama has no authentication: point it at a
 * server on a network you trust.
 *
 * Commands: /help, /models, /model NAME, /clear, /bye. Ctrl+C stops a reply.
 */
#include "term_chat.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "ollama";

#define RX_BUF (32 * 1024)
#define TX_BUF 1024
#define CONNECT_TIMEOUT_S 5
#define INPUT_MAX 2048            /* one prompt line */
#define JSON_LINE_MAX (64 * 1024) /* one NDJSON line, or a whole /api/tags body */
#define HISTORY_MAX (48 * 1024)   /* serialized conversation; oldest turns drop first */

#define SGR_RESET "\x1b[0m"
#define SGR_DIM "\x1b[2m"
#define SGR_ERR "\x1b[31m"
#define SGR_WARN "\x1b[33m"
#define PROMPT "\x1b[1;36m>>> " SGR_RESET

static volatile link_state_t s_state = LINK_IDLE;
static char s_status[96];
static volatile bool s_stop;
static StreamBufferHandle_t s_rx, s_tx;
static TaskHandle_t s_task;
static char s_host[64];
static uint16_t s_port;
static char s_model[96];

/* Worker-only from here down. */
static cJSON *s_msgs; /* [{"role","content"}, ...] sent with every request */
static char s_line[INPUT_MAX];
static size_t s_len;
static int s_esc; /* 0 text, 1 after ESC, 2 inside a CSI/SS3 sequence */

static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

/* ---- Output to the terminal ---------------------------------------------- */

static void out(const char *d, size_t n)
{
    while (n > 0 && !s_stop) {
        size_t sent = xStreamBufferSend(s_rx, d, n, pdMS_TO_TICKS(100));
        d += sent;
        n -= sent;
    }
}

static void outs(const char *s) { out(s, strlen(s)); }

static void outf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

/* Model text: LF becomes CRLF; other control bytes, escape sequences
 * included, are dropped so a reply can't drive the terminal. */
static void out_text(const char *s)
{
    char buf[256];
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') {
            buf[n++] = '\r';
            buf[n++] = '\n';
        } else if (c == '\t' || (c >= 0x20 && c != 0x7f)) {
            buf[n++] = (char)c;
        }
        if (n >= sizeof(buf) - 2) {
            out(buf, n);
            n = 0;
        }
    }
    if (n) out(buf, n);
}

/* ---- HTTP ---------------------------------------------------------------- */

static int connect_tcp(void)
{
    char ps[8];
    snprintf(ps, sizeof(ps), "%u", s_port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(s_host, ps, &hints, &res) != 0 || res == NULL) {
        status("cannot resolve %s", s_host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        status("out of sockets");
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        status("connect failed");
        return -1;
    }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {CONNECT_TIMEOUT_S, 0};
    int err = 0;
    socklen_t el = sizeof(err);
    if (select(fd + 1, NULL, &w, NULL, &tv) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
        close(fd);
        status("no answer from %s:%u", s_host, s_port);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

static bool send_all(int fd, const char *d, size_t n)
{
    while (n > 0) {
        ssize_t r = send(fd, d, n, 0);
        if (r <= 0) return false;
        d += r;
        n -= (size_t)r;
    }
    return true;
}

/* Sends the request and returns the socket, or -1 (reason in s_status). */
static int http_request(const char *method, const char *path, const char *body)
{
    int fd = connect_tcp();
    if (fd < 0) return -1;
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "%s %s HTTP/1.0\r\nHost: %s:%u\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n",
                     method, path, s_host, s_port, body ? (unsigned)strlen(body) : 0u);
    if (!send_all(fd, head, (size_t)n) || (body && !send_all(fd, body, strlen(body)))) {
        close(fd);
        status("send failed");
        return -1;
    }
    return fd;
}

/* Ctrl+C or close while waiting on the server. Other keys typed meanwhile
 * are dropped. */
static bool interrupted(void)
{
    if (s_stop) return true;
    char k[32];
    size_t n;
    while ((n = xStreamBufferReceive(s_tx, k, sizeof(k), 0)) > 0) {
        if (memchr(k, 0x03, n)) return true;
    }
    return false;
}

typedef enum { RESP_OK, RESP_INTERRUPTED, RESP_IO_ERROR } resp_t;
typedef void (*line_cb_t)(char *line, void *user);

/* Reads the response: the status code into *code, then each body line to
 * `cb` (a last line without a newline included). */
static resp_t http_lines(int fd, int *code, line_cb_t cb, void *user)
{
    char *acc = heap_caps_malloc(JSON_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (acc == NULL) return RESP_IO_ERROR;
    size_t len    = 0;
    bool in_body  = false, overflow = false;
    int crlf      = 0; /* progress through the "\r\n\r\n" that ends the headers */
    resp_t result = RESP_OK;
    *code         = 0;
    for (;;) {
        if (interrupted()) {
            result = RESP_INTERRUPTED;
            break;
        }
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        struct timeval tv = {0, 50 * 1000};
        if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) continue;
        char buf[1024];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            result = RESP_IO_ERROR;
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (!in_body) {
                if (len < 63) acc[len++] = c; /* enough for the status line */
                crlf = (c == (crlf % 2 ? '\n' : '\r')) ? crlf + 1 : (c == '\r');
                if (crlf == 4) {
                    acc[len] = '\0';
                    sscanf(acc, "HTTP/%*s %d", code);
                    in_body = true;
                    len     = 0;
                }
            } else if (c == '\n') {
                acc[len] = '\0';
                if (len && !overflow) cb(acc, user);
                len      = 0;
                overflow = false;
            } else if (len < JSON_LINE_MAX - 1) {
                acc[len++] = c;
            } else {
                overflow = true;
            }
        }
    }
    if (result == RESP_OK && in_body && len && !overflow) {
        acc[len] = '\0';
        cb(acc, user);
    }
    if (result == RESP_OK && !in_body) result = RESP_IO_ERROR;
    heap_caps_free(acc);
    return result;
}

/* ---- Models -------------------------------------------------------------- */

static void keep_json(char *line, void *user)
{
    cJSON **j = user;
    if (*j == NULL) *j = cJSON_Parse(line);
}

/* /api/tags as parsed JSON, or NULL (reason in s_status). */
static cJSON *fetch_models(void)
{
    int fd = http_request("GET", "/api/tags", NULL);
    if (fd < 0) return NULL;
    cJSON *j = NULL;
    int code;
    resp_t r = http_lines(fd, &code, keep_json, &j);
    close(fd);
    if (r != RESP_OK || code != 200 || !cJSON_IsArray(cJSON_GetObjectItem(j, "models"))) {
        if (r == RESP_OK) status("unexpected answer (HTTP %d)", code);
        if (r == RESP_IO_ERROR) status("connection dropped");
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}

/* Servers that list capabilities mark chat models "completion"; older ones
 * don't list them, so every model counts. */
static bool is_chat_model(const cJSON *m)
{
    const cJSON *caps = cJSON_GetObjectItem(m, "capabilities");
    if (!cJSON_IsArray(caps)) return true;
    const cJSON *c;
    cJSON_ArrayForEach(c, caps)
    {
        if (cJSON_IsString(c) && strcmp(c->valuestring, "completion") == 0) return true;
    }
    return false;
}

/* "llama3.2" names the same model as "llama3.2:latest". */
static bool same_model(const char *a, const char *b)
{
    size_t la = strcspn(a, ":"), lb = strcspn(b, ":");
    const char *ta = a[la] ? a + la + 1 : "latest", *tb = b[lb] ? b + lb + 1 : "latest";
    return la == lb && strncmp(a, b, la) == 0 && strcmp(ta, tb) == 0;
}

static const char *model_name(const cJSON *m) { return cJSON_GetStringValue(cJSON_GetObjectItem(m, "name")); }

static void list_models(void)
{
    cJSON *j = fetch_models();
    if (j == NULL) {
        outf(SGR_ERR "cannot list models: %s" SGR_RESET "\r\n", s_status);
        return;
    }
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(j, "models"))
    {
        const char *name = model_name(m);
        if (name == NULL) continue;
        bool cur = same_model(name, s_model);
        if (is_chat_model(m)) {
            outf("%s %s%s\r\n", cur ? "*" : " ", name, cur ? "  (current)" : "");
        } else {
            outf(SGR_DIM "  %s  (not a chat model)" SGR_RESET "\r\n", name);
        }
    }
    cJSON_Delete(j);
}

/* ---- Conversation -------------------------------------------------------- */

static cJSON *message(const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    return m;
}

/* Drop the oldest exchanges until the history fits HISTORY_MAX, always
 * keeping the newest message. */
static void trim_history(void)
{
    while (cJSON_GetArraySize(s_msgs) > 1) {
        char *s  = cJSON_PrintUnformatted(s_msgs);
        size_t n = s ? strlen(s) : 0;
        cJSON_free(s);
        if (n <= HISTORY_MAX) return;
        cJSON_DeleteItemFromArray(s_msgs, 0);
        const cJSON *first = cJSON_GetArrayItem(s_msgs, 0);
        const char *role   = cJSON_GetStringValue(cJSON_GetObjectItem(first, "role"));
        if (role && strcmp(role, "assistant") == 0) cJSON_DeleteItemFromArray(s_msgs, 0);
    }
}

typedef struct {
    char *reply;
    size_t len, cap;
    bool waiting;  /* the "..." placeholder is still on screen */
    bool thinking; /* inside dimmed thinking text */
    char err[160];
} turn_t;

static void turn_text(turn_t *t, const char *s, bool thinking)
{
    if (t->waiting) {
        outs("\r\x1b[K");
        t->waiting = false;
    }
    if (thinking != t->thinking) {
        outs(thinking ? SGR_DIM : SGR_RESET "\r\n\r\n");
        t->thinking = thinking;
    }
    out_text(s);
    if (thinking) return;
    size_t n = strlen(s);
    if (t->len + n + 1 > t->cap) {
        size_t cap = (t->len + n + 1) * 2;
        char *r    = heap_caps_realloc(t->reply, cap, MALLOC_CAP_SPIRAM);
        if (r == NULL) return;
        t->reply = r;
        t->cap   = cap;
    }
    memcpy(t->reply + t->len, s, n + 1);
    t->len += n;
}

static void chat_chunk(char *line, void *user)
{
    turn_t *t = user;
    cJSON *j  = cJSON_Parse(line);
    if (j == NULL) return;
    const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(j, "error"));
    if (err) snprintf(t->err, sizeof(t->err), "%s", err);
    const cJSON *m  = cJSON_GetObjectItem(j, "message");
    const char *th  = cJSON_GetStringValue(cJSON_GetObjectItem(m, "thinking"));
    const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
    if (th && *th) turn_text(t, th, true);
    if (txt && *txt) turn_text(t, txt, false);
    cJSON_Delete(j);
}

static void ask(const char *text)
{
    cJSON_AddItemToArray(s_msgs, message("user", text));
    trim_history();
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", s_model);
    cJSON_AddItemReferenceToObject(req, "messages", s_msgs);
    cJSON_AddBoolToObject(req, "stream", true);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req); /* the reference leaves s_msgs alone */

    turn_t t = {.waiting = true};
    outs(SGR_DIM "..." SGR_RESET);
    int code   = 0;
    resp_t r   = RESP_IO_ERROR;
    int fd     = body ? http_request("POST", "/api/chat", body) : -1;
    cJSON_free(body);
    if (fd >= 0) {
        r = http_lines(fd, &code, chat_chunk, &t);
        close(fd);
    }
    if (t.waiting) outs("\r\x1b[K");
    if (t.thinking) outs(SGR_RESET);

    bool ok = r == RESP_OK && code == 200 && t.err[0] == '\0' && t.len > 0;
    if (ok) {
        cJSON_AddItemToArray(s_msgs, message("assistant", t.reply));
    } else {
        /* Forget the question too, so the history stays question/answer. */
        cJSON_DeleteItemFromArray(s_msgs, cJSON_GetArraySize(s_msgs) - 1);
        if (r == RESP_INTERRUPTED) {
            outs(SGR_DIM " ^C" SGR_RESET);
        } else if (fd < 0) {
            outf(SGR_ERR "%s" SGR_RESET, s_status);
        } else if (t.err[0]) {
            outf(SGR_ERR "error: %s" SGR_RESET, t.err);
        } else if (r == RESP_IO_ERROR) {
            outs(SGR_ERR "connection dropped" SGR_RESET);
        } else if (code != 200) {
            outf(SGR_ERR "HTTP %d" SGR_RESET, code);
        } else {
            outs(SGR_DIM "(empty reply)" SGR_RESET);
        }
        ESP_LOGW(TAG, "chat failed: r=%d http=%d %s", (int)r, code, t.err[0] ? t.err : s_status);
    }
    heap_caps_free(t.reply);
    outs("\r\n\r\n");
}

/* ---- Prompt -------------------------------------------------------------- */

static void help(void)
{
    outs("  /models        list the server's models\r\n"
         "  /model NAME    switch model (the conversation is kept)\r\n"
         "  /clear         forget the conversation\r\n"
         "  /bye           close (Alt+Esc also leaves)\r\n"
         "  Ctrl+C stops a reply, Ctrl+L clears the screen\r\n");
}

/* Runs the submitted line. False ends the session. */
static bool submit(void)
{
    s_line[s_len] = '\0';
    char *line    = s_line;
    while (*line == ' ') line++;
    outs("\r\n");
    if (line[0] != '/') {
        if (*line) ask(line);
        return true;
    }
    char *arg = line + strcspn(line, " ");
    if (*arg) *arg++ = '\0';
    while (*arg == ' ') arg++;
    if (!strcmp(line, "/bye") || !strcmp(line, "/exit") || !strcmp(line, "/quit")) return false;
    if (!strcmp(line, "/clear")) {
        cJSON_Delete(s_msgs);
        s_msgs = cJSON_CreateArray();
        outs(SGR_DIM "conversation cleared" SGR_RESET "\r\n");
    } else if (!strcmp(line, "/models")) {
        list_models();
    } else if (!strcmp(line, "/model")) {
        if (*arg) snprintf(s_model, sizeof(s_model), "%s", arg);
        outf("model: %s\r\n", s_model);
    } else if (!strcmp(line, "/help") || !strcmp(line, "/?")) {
        help();
    } else {
        outs(SGR_WARN "unknown command; /help lists them" SGR_RESET "\r\n");
    }
    outs("\r\n");
    return true;
}

static void erase_char(void)
{
    if (s_len == 0) return;
    while (s_len > 0 && ((unsigned char)s_line[s_len - 1] & 0xC0) == 0x80) s_len--; /* UTF-8 continuation */
    if (s_len > 0) s_len--;
    outs("\b \b");
}

/* One key byte from the terminal. False ends the session. */
static bool edit(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (s_esc == 1) { /* arrows, function keys and Alt+key are not used */
        s_esc = (c == '[' || c == 'O') ? 2 : 0;
        return true;
    }
    if (s_esc == 2) {
        if (c >= 0x40 && c <= 0x7e) s_esc = 0;
        return true;
    }
    switch (c) {
    case 0x1b:
        s_esc = 1;
        break;
    case '\r':
        if (!submit()) return false;
        s_len = 0;
        outs(PROMPT);
        break;
    case 0x7f:
    case '\b':
        erase_char();
        break;
    case 0x15: /* Ctrl+U */
        while (s_len) erase_char();
        break;
    case 0x03: /* Ctrl+C */
        outs("^C\r\n" PROMPT);
        s_len = 0;
        break;
    case 0x04: /* Ctrl+D */
        if (s_len == 0) return false;
        break;
    case 0x0c: /* Ctrl+L */
        outs("\x1b[H\x1b[2J" PROMPT);
        out(s_line, s_len);
        break;
    default:
        if (c >= 0x20 && s_len < sizeof(s_line) - 1) {
            s_line[s_len++] = (char)c;
            out(&ch, 1);
        }
        break;
    }
    return true;
}

/* ---- Worker -------------------------------------------------------------- */

/* Checks the server and settles the model. False if there is nothing to
 * talk to. */
static bool hello(void)
{
    outf("\x1b[1mOLLAMA\x1b[0m  %s:%u\r\n", s_host, s_port);
    cJSON *j = fetch_models();
    if (j == NULL) {
        outf(SGR_ERR "cannot reach Ollama: %s" SGR_RESET "\r\n", s_status);
        outs(SGR_DIM "The server must listen on the network (OLLAMA_HOST=0.0.0.0), not just localhost." SGR_RESET
                     "\r\n");
        return false;
    }
    const cJSON *m;
    const char *first = NULL;
    bool found        = false;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(j, "models"))
    {
        const char *name = model_name(m);
        if (name == NULL || !is_chat_model(m)) continue;
        if (first == NULL) first = name;
        if (same_model(name, s_model)) found = true;
    }
    if (s_model[0] == '\0' && first) {
        snprintf(s_model, sizeof(s_model), "%s", first);
        found = true;
    }
    cJSON_Delete(j);
    if (s_model[0] == '\0') {
        outs(SGR_ERR "no chat models on the server; pull one there, e.g. ollama pull llama3.2" SGR_RESET "\r\n");
        status("no chat models");
        return false;
    }
    outf("model \x1b[1m%s\x1b[0m   " SGR_DIM "/help for commands" SGR_RESET "\r\n", s_model);
    if (!found) outs(SGR_WARN "that model isn't on the server; /models lists what is" SGR_RESET "\r\n");
    outs("\r\n");
    return true;
}

static void worker(void *arg)
{
    (void)arg;
    s_msgs = cJSON_CreateArray();
    s_len  = 0;
    s_esc  = 0;
    if (hello()) {
        s_state = LINK_OPEN;
        status("");
        outs(PROMPT);
        bool alive = true;
        while (alive && !s_stop) {
            char k[64];
            size_t n = xStreamBufferReceive(s_tx, k, sizeof(k), pdMS_TO_TICKS(20));
            for (size_t i = 0; i < n && alive; i++) alive = edit(k[i]);
        }
        if (!s_stop) status("session ended");
    }
    cJSON_Delete(s_msgs);
    s_msgs  = NULL;
    s_state = LINK_CLOSED;
    s_task  = NULL;
    vTaskDelete(NULL);
}

/* ---- API ----------------------------------------------------------------- */

bool chat_open(const char *host, uint16_t port, const char *model, int cols, int rows)
{
    (void)cols;
    (void)rows; /* replies are plain text; the emulator wraps them */
    if (s_task) return false;
    if (s_rx == NULL) {
        s_rx = xStreamBufferCreateWithCaps(RX_BUF, 1, MALLOC_CAP_SPIRAM);
        s_tx = xStreamBufferCreate(TX_BUF, 1);
    }
    xStreamBufferReset(s_rx);
    xStreamBufferReset(s_tx);
    snprintf(s_host, sizeof(s_host), "%s", host);
    snprintf(s_model, sizeof(s_model), "%s", model ? model : "");
    s_port  = port ? port : CHAT_DEFAULT_PORT;
    s_stop  = false;
    s_state = LINK_CONNECTING;
    status("connecting");
    if (xTaskCreatePinnedToCore(worker, "ollama", 10240, NULL, 5, &s_task, 0) != pdPASS) {
        status("out of memory");
        s_state = LINK_CLOSED;
        return false;
    }
    return true;
}

void chat_write(const char *d, size_t n)
{
    if (s_tx && (s_state == LINK_OPEN || s_state == LINK_CONNECTING)) xStreamBufferSend(s_tx, d, n, pdMS_TO_TICKS(50));
}

void chat_resize(int cols, int rows)
{
    (void)cols;
    (void)rows;
}

void chat_close(void)
{
    s_stop = true; /* the worker notices within ~50 ms and ends */
    if (s_state != LINK_OPEN && s_state != LINK_CONNECTING) s_state = LINK_IDLE;
}

size_t chat_read(char *buf, size_t max) { return s_rx ? xStreamBufferReceive(s_rx, buf, max, 0) : 0; }
link_state_t chat_state(void) { return s_state; }
const char *chat_status(void) { return s_status; }
