/*
 * Self-hosted LiveSync end-to-end encryption, "v2" (HKDF) format, the
 * default since plugin 0.25:
 *
 *   master = PBKDF2-HMAC-SHA256(passphrase, pbkdf2salt, 310000, 32)
 *   per string: key = HKDF-SHA256(master, salt32, "", 32)
 *               "%=" + base64(iv12 || salt32 || AES-256-GCM(key, iv, text) || tag16)
 *
 * The master key takes seconds on the P4, so callers derive it once and
 * keep it (ls_master_t) for the whole session.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t key[32];
    bool valid;
} ls_master_t;

/* Derive the master key. `salt_b64` is the pbkdf2salt string from
 * _local/obsidian_livesync_sync_parameters. */
bool ls_master_derive(const char *passphrase, const char *salt_b64, ls_master_t *out);

/* Decrypt a "%=..." string into a malloc'd, NUL-terminated buffer (caller
 * frees). NULL on a wrong key, a damaged string or an older format. */
char *ls_decrypt(const ls_master_t *m, const char *data, size_t *len);

/* Encrypt text into a malloc'd "%=..." string (caller frees). */
char *ls_encrypt(const ls_master_t *m, const char *text, size_t len);

/* Platform random bytes (esp_fill_random on the device, /dev/urandom in
 * the simulator). */
void ls_random(uint8_t *buf, size_t n);

#ifdef __cplusplus
}
#endif
