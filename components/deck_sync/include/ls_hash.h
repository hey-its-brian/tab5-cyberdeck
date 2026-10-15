/*
 * Hashes that Self-hosted LiveSync uses for chunk ids, ported so the deck
 * produces the same ids as the Obsidian plugin (otherwise every edit from
 * the deck would duplicate chunks, or worse, confuse the plugin's cache).
 *
 *   xxh64            XXH64 over UTF-8 bytes, seed 0 (xxhash-wasm h64)
 *   ls_mixed_hash    octagonal-wheels fallbackMixedHashEach: Murmur3-ish
 *                    plus FNV-1a over UTF-16 code units, with JavaScript's
 *                    double-precision arithmetic reproduced where it leaks
 *   ls_chunk_id      "h:+<base36 xxh64(piece-hashedPassphrase-len)>" with
 *                    E2EE, "h:<base36 xxh64(piece-len)>" without
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t ls_xxh64(const void *data, size_t len, uint64_t seed);

/* Number of UTF-16 code units in a UTF-8 string (what JS calls .length). */
size_t ls_utf16_len(const char *s, size_t n);

/* JavaScript Number.prototype.toString(36) for an unsigned value. `out`
 * needs 14 bytes for 64-bit, 8 for 32-bit. */
void ls_base36_u64(uint64_t v, char *out);
void ls_base36_u32(uint32_t v, char *out);

/* fallbackMixedHashEach(src): 2 base-36 numbers glued together, up to 14
 * chars plus NUL. */
void ls_mixed_hash(const char *src, size_t n, char out[16]);

/* The salt mixed into every chunk id when E2EE is on:
 * fallbackMixedHashEach(SALT_OF_ID + passphrase[0 .. floor(len * 3 / 4)]). */
void ls_hashed_passphrase(const char *passphrase, char out[16]);

/* Chunk document id for `piece` (UTF-8, `n` bytes). With `hashed_pass` NULL
 * the id is the unencrypted form. `out` needs 24 bytes. */
void ls_chunk_id(const char *piece, size_t n, const char *hashed_pass, char *out);

#ifdef __cplusplus
}
#endif
