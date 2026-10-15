#include "ls_crypto.h"

#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

#ifdef ESP_PLATFORM
#include "esp_random.h"
void ls_random(uint8_t *buf, size_t n) { esp_fill_random(buf, n); }
#else
#include <stdio.h>
void ls_random(uint8_t *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == NULL || fread(buf, 1, n, f) != n) {
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rand();
    }
    if (f) fclose(f);
}
#endif

#define IV_LEN 12
#define SALT_LEN 32
#define TAG_LEN 16
#define PBKDF2_ITERATIONS 310000

bool ls_master_derive(const char *passphrase, const char *salt_b64, ls_master_t *out)
{
    out->valid = false;
    uint8_t salt[64];
    size_t slen = 0;
    if (mbedtls_base64_decode(salt, sizeof(salt), &slen, (const unsigned char *)salt_b64, strlen(salt_b64)) != 0 ||
        slen == 0) {
        return false;
    }
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)passphrase, strlen(passphrase),
                                           salt, slen, PBKDF2_ITERATIONS, sizeof(out->key), out->key);
    out->valid = rc == 0;
    return out->valid;
}

static bool chunk_key(const ls_master_t *m, const uint8_t *salt, uint8_t key[32])
{
    return mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, SALT_LEN, m->key, sizeof(m->key), NULL,
                        0, key, 32) == 0;
}

char *ls_decrypt(const ls_master_t *m, const char *data, size_t *len)
{
    if (!m->valid || data == NULL || data[0] != '%' || data[1] != '=') return NULL;
    const char *b64 = data + 2;
    size_t blen     = strlen(b64), raw_len = 0;
    uint8_t *raw    = malloc(blen + 1);
    if (raw == NULL) return NULL;
    if (mbedtls_base64_decode(raw, blen + 1, &raw_len, (const unsigned char *)b64, blen) != 0 ||
        raw_len < IV_LEN + SALT_LEN + TAG_LEN) {
        free(raw);
        return NULL;
    }
    const uint8_t *iv   = raw;
    const uint8_t *salt = raw + IV_LEN;
    const uint8_t *ct   = raw + IV_LEN + SALT_LEN;
    size_t ct_len       = raw_len - IV_LEN - SALT_LEN - TAG_LEN;
    const uint8_t *tag  = ct + ct_len;

    uint8_t key[32];
    char *out = NULL;
    if (chunk_key(m, salt, key)) {
        out = malloc(ct_len + 1);
        mbedtls_gcm_context g;
        mbedtls_gcm_init(&g);
        if (out == NULL || mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) != 0 ||
            mbedtls_gcm_auth_decrypt(&g, ct_len, iv, IV_LEN, NULL, 0, tag, TAG_LEN, ct, (unsigned char *)out) != 0) {
            free(out);
            out = NULL;
        } else {
            out[ct_len] = '\0';
            if (len) *len = ct_len;
        }
        mbedtls_gcm_free(&g);
    }
    memset(key, 0, sizeof(key));
    free(raw);
    return out;
}

char *ls_encrypt(const ls_master_t *m, const char *text, size_t len)
{
    if (!m->valid) return NULL;
    size_t raw_len = IV_LEN + SALT_LEN + len + TAG_LEN;
    uint8_t *raw   = malloc(raw_len);
    if (raw == NULL) return NULL;
    ls_random(raw, IV_LEN + SALT_LEN);
    uint8_t key[32];
    bool ok = chunk_key(m, raw + IV_LEN, key);
    if (ok) {
        mbedtls_gcm_context g;
        mbedtls_gcm_init(&g);
        ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
             mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, raw, IV_LEN, NULL, 0, (const unsigned char *)text,
                                       raw + IV_LEN + SALT_LEN, TAG_LEN, raw + IV_LEN + SALT_LEN + len) == 0;
        mbedtls_gcm_free(&g);
    }
    memset(key, 0, sizeof(key));
    char *out = NULL;
    if (ok) {
        size_t cap = 2 + (raw_len + 2) / 3 * 4 + 1, olen = 0;
        out        = malloc(cap);
        if (out) {
            out[0] = '%';
            out[1] = '=';
            if (mbedtls_base64_encode((unsigned char *)out + 2, cap - 2, &olen, raw, raw_len) != 0) {
                free(out);
                out = NULL;
            }
        }
    }
    free(raw);
    return out;
}
