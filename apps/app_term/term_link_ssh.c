/*
 * term_link over SSH (libssh2 on mbedTLS). A worker task owns the socket
 * and session; bytes cross to the LVGL task through stream buffers.
 *
 * Host keys: SHA256 fingerprints pinned in <storage>/ssh/known_hosts as
 * "host:port SHA256:..." lines. Unknown or changed keys pause in
 * LINK_VERIFY_HOST until the user decides.
 *
 * Device key: an ECDSA P-256 key generated on first use, stored as PEM in
 * <storage>/ssh/id_ecdsa. Its OpenSSH public key is shown in TERMINAL so
 * it can be added to a server's authorized_keys.
 */
#include "term_link.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "deck_hal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "libssh2.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"

static const char *TAG = "ssh";

#define RX_BUF (64 * 1024)
#define TX_BUF (8 * 1024)
#define CONNECT_TIMEOUT_S 10

static volatile link_state_t s_state = LINK_IDLE;
static char s_status[96];
static char s_fpr[64];
static bool s_fpr_changed;
static volatile int s_decision; /* 0 pending, 1 accept, -1 reject */
static volatile bool s_stop;
static volatile int s_cols, s_rows;
static volatile bool s_resize;
static StreamBufferHandle_t s_rx, s_tx;
static TaskHandle_t s_task;
static link_params_t s_p;
static char s_pubkey[256];

bool link_has_local(void) { return false; }

static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", s_status);
}

/* ---- Files --------------------------------------------------------------- */

static void ssh_path(char *out, size_t n, const char *name)
{
    const char *root = hal_storage_root();
    snprintf(out, n, "%s/ssh/%s", root ? root : "", name);
}

static void ssh_dir(void)
{
    char d[96];
    const char *root = hal_storage_root();
    if (root == NULL) return;
    snprintf(d, sizeof(d), "%s/ssh", root);
    mkdir(d, 0755);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (n > 0 && n < 16384) ? malloc((size_t)n + 1) : NULL;
    if (b) {
        b[fread(b, 1, (size_t)n, f)] = '\0';
        if (len) *len = (size_t)n;
    }
    fclose(f);
    return b;
}

/* ---- Device key ---------------------------------------------------------- */

static void put_string(unsigned char **p, const void *s, size_t n)
{
    (*p)[0] = (unsigned char)(n >> 24);
    (*p)[1] = (unsigned char)(n >> 16);
    (*p)[2] = (unsigned char)(n >> 8);
    (*p)[3] = (unsigned char)n;
    memcpy(*p + 4, s, n);
    *p += 4 + n;
}

/* OpenSSH "ecdsa-sha2-nistp256 AAAA... deck@tab5" from a PEM private key. */
static bool make_pubkey(const char *pem, size_t pem_len)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context ent;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, NULL, 0);
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)pem, pem_len + 1, NULL, 0, mbedtls_ctr_drbg_random, &drbg) ==
            0 &&
        mbedtls_pk_get_type(&pk) == MBEDTLS_PK_ECKEY) {
        mbedtls_ecp_keypair *ec = mbedtls_pk_ec(pk);
        unsigned char q[65];
        size_t qlen = 0;
        if (mbedtls_ecp_point_write_binary(&ec->MBEDTLS_PRIVATE(grp), &ec->MBEDTLS_PRIVATE(Q),
                                           MBEDTLS_ECP_PF_UNCOMPRESSED, &qlen, q, sizeof(q)) == 0) {
            unsigned char blob[128], *p = blob;
            put_string(&p, "ecdsa-sha2-nistp256", 19);
            put_string(&p, "nistp256", 8);
            put_string(&p, q, qlen);
            unsigned char b64[200];
            size_t olen = 0;
            if (mbedtls_base64_encode(b64, sizeof(b64), &olen, blob, (size_t)(p - blob)) == 0) {
                snprintf(s_pubkey, sizeof(s_pubkey), "ecdsa-sha2-nistp256 %.*s deck@tab5", (int)olen, b64);
                ok = true;
            }
        }
    }
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

/* Load the key, generating it the first time. Returns PEM (caller frees). */
static char *device_key(size_t *len)
{
    char path[96];
    ssh_path(path, sizeof(path), "id_ecdsa");
    char *pem = read_file(path, len);
    if (pem) return pem;

    ssh_dir();
    mbedtls_pk_context pk;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context ent;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    unsigned char *buf = malloc(1024);
    bool ok            = buf && mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, NULL, 0) == 0 &&
              mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
              mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg) == 0 &&
              mbedtls_pk_write_key_pem(&pk, buf, 1024) == 0;
    if (ok) {
        FILE *f = fopen(path, "w");
        if (f) {
            fputs((char *)buf, f);
            fclose(f);
            ESP_LOGI(TAG, "generated device key");
        }
    }
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    free(buf);
    return ok ? read_file(path, len) : NULL;
}

const char *link_pubkey(void)
{
    if (s_pubkey[0] == '\0') {
        size_t len = 0;
        char *pem  = device_key(&len);
        if (pem == NULL || !make_pubkey(pem, len)) {
            snprintf(s_pubkey, sizeof(s_pubkey), "no key: storage unavailable");
        }
        free(pem);
    }
    return s_pubkey;
}

/* ---- known_hosts --------------------------------------------------------- */

typedef enum { HK_UNKNOWN, HK_MATCH, HK_CHANGED } hk_t;

static hk_t known_lookup(const char *id, const char *fpr)
{
    char path[96];
    ssh_path(path, sizeof(path), "known_hosts");
    FILE *f = fopen(path, "r");
    if (f == NULL) return HK_UNKNOWN;
    char line[200], h[100], k[80];
    hk_t r = HK_UNKNOWN;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%99s %79s", h, k) == 2 && strcmp(h, id) == 0) {
            r = strcmp(k, fpr) == 0 ? HK_MATCH : HK_CHANGED;
            break;
        }
    }
    fclose(f);
    return r;
}

static void known_store(const char *id, const char *fpr)
{
    char path[96], tmp[100];
    ssh_dir();
    ssh_path(path, sizeof(path), "known_hosts");
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (out == NULL) {
        if (in) fclose(in);
        return;
    }
    char line[200], h[100];
    while (in && fgets(line, sizeof(line), in)) {
        if (sscanf(line, "%99s", h) == 1 && strcmp(h, id) == 0) continue; /* replaced below */
        fputs(line, out);
    }
    fprintf(out, "%s %s\n", id, fpr);
    if (in) fclose(in);
    fclose(out);
    unlink(path);
    rename(tmp, path);
}

/* ---- Socket -------------------------------------------------------------- */

static int connect_tcp(const char *host, uint16_t port)
{
    char ps[8];
    snprintf(ps, sizeof(ps), "%u", port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || res == NULL) {
        status("cannot resolve %s", host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
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
        status("no answer from %s:%u", host, port);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK); /* blocking for the handshake */
    return fd;
}

static void wait_socket(int fd, LIBSSH2_SESSION *s, int ms)
{
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    int dir = libssh2_session_block_directions(s);
    FD_SET(fd, &r); /* always watch for incoming data */
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(fd, &w);
    struct timeval tv = {0, ms * 1000};
    select(fd + 1, &r, &w, NULL, &tv);
}

/* ---- Worker -------------------------------------------------------------- */

static void fingerprint(LIBSSH2_SESSION *s)
{
    const unsigned char *h = (const unsigned char *)libssh2_hostkey_hash(s, LIBSSH2_HOSTKEY_HASH_SHA256);
    unsigned char b64[64];
    size_t olen = 0;
    s_fpr[0]    = '\0';
    if (h && mbedtls_base64_encode(b64, sizeof(b64), &olen, h, 32) == 0) {
        while (olen && b64[olen - 1] == '=') olen--; /* OpenSSH style: no padding */
        snprintf(s_fpr, sizeof(s_fpr), "SHA256:%.*s", (int)olen, b64);
    }
}

static void worker(void *arg)
{
    (void)arg;
    LIBSSH2_SESSION *sess = NULL;
    LIBSSH2_CHANNEL *ch   = NULL;
    char id[100];
    snprintf(id, sizeof(id), "%s:%u", s_p.host, s_p.port);

    status("connecting to %s", id);
    int fd = connect_tcp(s_p.host, s_p.port);
    if (fd < 0) goto done;

    sess = libssh2_session_init();
    if (sess == NULL || libssh2_session_handshake(sess, fd) != 0) {
        status("SSH handshake failed");
        goto done;
    }

    fingerprint(sess);
    hk_t hk = known_lookup(id, s_fpr);
    if (hk != HK_MATCH) {
        s_fpr_changed = hk == HK_CHANGED;
        s_decision    = 0;
        s_state       = LINK_VERIFY_HOST;
        status("verify host key");
        while (s_decision == 0 && !s_stop) vTaskDelay(pdMS_TO_TICKS(50));
        if (s_decision < 0 || s_stop) {
            status("host key rejected");
            goto done;
        }
        known_store(id, s_fpr);
    }

    s_state = LINK_AUTH;
    status("authenticating %s", s_p.user);
    int rc;
    bool used_password = s_p.password[0] != '\0';
    if (used_password) {
        rc = libssh2_userauth_password(sess, s_p.user, s_p.password);
    } else {
        size_t klen = 0;
        char *pem   = device_key(&klen);
        rc          = pem ? libssh2_userauth_publickey_frommemory(sess, s_p.user, strlen(s_p.user), NULL, 0, pem, klen, NULL)
                          : -1;
        free(pem);
    }
    memset(s_p.password, 0, sizeof(s_p.password));
    if (rc != 0) {
        status(used_password ? "password rejected" : "key rejected: add the DEVICE KEY to authorized_keys, or use a password");
        goto done;
    }

    ch = libssh2_channel_open_session(sess);
    if (ch == NULL || libssh2_channel_request_pty_ex(ch, "xterm-256color", 14, NULL, 0, s_cols, s_rows, 0, 0) != 0 ||
        libssh2_channel_shell(ch) != 0) {
        status("could not start a shell");
        goto done;
    }

    libssh2_keepalive_config(sess, 1, 30);
    libssh2_session_set_blocking(sess, 0);
    s_state = LINK_OPEN;
    status("online");

    char buf[2048];
    char out[512];
    size_t pending = 0, sent = 0;
    while (!s_stop) {
        ssize_t n;
        while ((n = libssh2_channel_read(ch, buf, sizeof(buf))) > 0) {
            xStreamBufferSend(s_rx, buf, (size_t)n, pdMS_TO_TICKS(100));
        }
        while ((n = libssh2_channel_read_stderr(ch, buf, sizeof(buf))) > 0) {
            xStreamBufferSend(s_rx, buf, (size_t)n, pdMS_TO_TICKS(100));
        }
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
            status("connection lost");
            break;
        }
        if (libssh2_channel_eof(ch)) {
            status("remote closed the session");
            break;
        }
        if (sent == pending) {
            pending = xStreamBufferReceive(s_tx, out, sizeof(out), 0);
            sent    = 0;
        }
        while (sent < pending) {
            n = libssh2_channel_write(ch, out + sent, pending - sent);
            if (n == LIBSSH2_ERROR_EAGAIN) break;
            if (n < 0) {
                status("write failed");
                goto done;
            }
            sent += (size_t)n;
        }
        if (s_resize) {
            if (libssh2_channel_request_pty_size(ch, s_cols, s_rows) != LIBSSH2_ERROR_EAGAIN) s_resize = false;
        }
        int next = 0;
        libssh2_keepalive_send(sess, &next);
        wait_socket(fd, sess, (sent < pending || xStreamBufferBytesAvailable(s_tx)) ? 1 : 20);
    }

done:
    if (ch) {
        libssh2_session_set_blocking(sess, 1);
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
    }
    if (sess) {
        libssh2_session_disconnect(sess, "bye");
        libssh2_session_free(sess);
    }
    if (fd >= 0) close(fd);
    s_state = LINK_CLOSED;
    s_task  = NULL;
    vTaskDelete(NULL);
}

/* ---- API ----------------------------------------------------------------- */

bool link_open(const link_params_t *p, int cols, int rows)
{
    if (s_task) return false;
    static bool inited;
    if (!inited) {
        libssh2_init(0);
        inited = true;
    }
    if (s_rx == NULL) {
        s_rx = xStreamBufferCreateWithCaps(RX_BUF, 1, MALLOC_CAP_SPIRAM);
        s_tx = xStreamBufferCreate(TX_BUF, 1);
    }
    xStreamBufferReset(s_rx);
    xStreamBufferReset(s_tx);
    s_p      = *p;
    s_cols   = cols;
    s_rows   = rows;
    s_stop   = false;
    s_resize = false;
    s_state  = LINK_CONNECTING;
    /* mbedTLS handshakes need a deep stack. */
    if (xTaskCreatePinnedToCore(worker, "ssh", 20480, NULL, 5, &s_task, 0) != pdPASS) {
        status("out of memory");
        s_state = LINK_CLOSED;
        return false;
    }
    return true;
}

void link_write(const char *d, size_t n)
{
    if (s_tx && s_state == LINK_OPEN) xStreamBufferSend(s_tx, d, n, pdMS_TO_TICKS(50));
}

void link_resize(int cols, int rows)
{
    s_cols   = cols;
    s_rows   = rows;
    s_resize = true;
}

void link_close(void)
{
    s_stop = true; /* the worker notices within ~20 ms and tears down */
    if (s_state != LINK_OPEN && s_state != LINK_CONNECTING && s_state != LINK_AUTH && s_state != LINK_VERIFY_HOST) {
        s_state = LINK_IDLE;
    }
}

size_t link_read(char *buf, size_t max) { return s_rx ? xStreamBufferReceive(s_rx, buf, max, 0) : 0; }
link_state_t link_state(void) { return s_state; }
const char *link_status(void) { return s_status; }
const char *link_fingerprint(void) { return s_fpr; }
bool link_hostkey_changed(void) { return s_fpr_changed; }
void link_hostkey_decide(bool accept) { s_decision = accept ? 1 : -1; }
