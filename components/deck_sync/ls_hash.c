#include "ls_hash.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- XXH64 (Yann Collet's algorithm, public domain), streaming ---------- */

#define P1 0x9E3779B185EBCA87ULL
#define P2 0xC2B2AE3D27D4EB4FULL
#define P3 0x165667B19E3779F9ULL
#define P4 0x85EBCA77C2B2AE63ULL
#define P5 0x27D4EB2F165667C5ULL

typedef struct {
    uint64_t v[4];
    uint64_t total;
    uint8_t buf[32];
    size_t buffered;
} xxh64_t;

static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8); /* little endian hosts only (RISC-V, x86, arm64) */
    return v;
}
static inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline uint64_t round64(uint64_t acc, uint64_t in)
{
    acc += in * P2;
    acc = rotl64(acc, 31);
    return acc * P1;
}
static inline uint64_t merge64(uint64_t acc, uint64_t v)
{
    v = round64(0, v);
    acc ^= v;
    return acc * P1 + P4;
}

static void xxh64_init(xxh64_t *s, uint64_t seed)
{
    s->v[0]     = seed + P1 + P2;
    s->v[1]     = seed + P2;
    s->v[2]     = seed;
    s->v[3]     = seed - P1;
    s->total    = 0;
    s->buffered = 0;
}

static void xxh64_block(xxh64_t *s, const uint8_t *p)
{
    s->v[0] = round64(s->v[0], rd64(p));
    s->v[1] = round64(s->v[1], rd64(p + 8));
    s->v[2] = round64(s->v[2], rd64(p + 16));
    s->v[3] = round64(s->v[3], rd64(p + 24));
}

static void xxh64_update(xxh64_t *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->total += len;
    if (s->buffered) {
        size_t take = 32 - s->buffered;
        if (take > len) take = len;
        memcpy(s->buf + s->buffered, p, take);
        s->buffered += take;
        p += take;
        len -= take;
        if (s->buffered < 32) return;
        xxh64_block(s, s->buf);
        s->buffered = 0;
    }
    while (len >= 32) {
        xxh64_block(s, p);
        p += 32;
        len -= 32;
    }
    memcpy(s->buf, p, len);
    s->buffered = len;
}

static uint64_t xxh64_digest(const xxh64_t *s, uint64_t seed)
{
    uint64_t h;
    if (s->total >= 32) {
        h = rotl64(s->v[0], 1) + rotl64(s->v[1], 7) + rotl64(s->v[2], 12) + rotl64(s->v[3], 18);
        h = merge64(h, s->v[0]);
        h = merge64(h, s->v[1]);
        h = merge64(h, s->v[2]);
        h = merge64(h, s->v[3]);
    } else {
        h = seed + P5;
    }
    h += s->total;
    const uint8_t *p   = s->buf;
    const uint8_t *end = s->buf + s->buffered;
    while (p + 8 <= end) {
        h ^= round64(0, rd64(p));
        h = rotl64(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)rd32(p) * P1;
        h = rotl64(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (*p++) * P5;
        h = rotl64(h, 11) * P1;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

uint64_t ls_xxh64(const void *data, size_t len, uint64_t seed)
{
    xxh64_t s;
    xxh64_init(&s, seed);
    xxh64_update(&s, data, len);
    return xxh64_digest(&s, seed);
}

/* ---- UTF-16 view of UTF-8 ------------------------------------------------ */

/* Decode one code point; returns bytes consumed (1 on malformed input, which
 * then counts as one unit, like a lone byte would in the plugin's view). */
static size_t utf8_next(const unsigned char *s, size_t n, uint32_t *cp)
{
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (len == 1 || len > n) {
        *cp = c;
        return 1;
    }
    uint32_t v = c & (0xFF >> (len + 1));
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = c;
            return 1;
        }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

size_t ls_utf16_len(const char *s, size_t n)
{
    size_t units = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_next((const unsigned char *)s + i, n - i, &cp);
        units += cp >= 0x10000 ? 2 : 1;
    }
    return units;
}

/* ---- Base 36 ------------------------------------------------------------- */

static const char DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";

void ls_base36_u64(uint64_t v, char *out)
{
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = DIGITS[v % 36];
        v /= 36;
    } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

void ls_base36_u32(uint32_t v, char *out) { ls_base36_u64(v, out); }

/* ---- fallbackMixedHashEach ------------------------------------------------
 * A line-by-line port of octagonal-wheels/hash/purejs.js mixedHash(). Most of
 * it is 32-bit, but two multiplications are plain JavaScript Number math on
 * values that exceed 2^53, so they are done in doubles here too and brought
 * back with ECMAScript ToInt32. */

static int32_t to_int32(double d)
{
    if (!isfinite(d)) return 0;
    double t = trunc(d);
    double m = fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

static void mixed_hash(const char *s, size_t n, uint32_t seed, uint32_t fnv_in, uint32_t *murmur, uint32_t *fnv)
{
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    int32_t h1     = (int32_t)seed;
    uint32_t fnv1a = fnv_in;
    size_t len     = 0; /* UTF-16 units seen */
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += utf8_next((const unsigned char *)s + i, n - i, &cp);
        uint32_t units[2];
        int count = 1;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            units[0] = 0xD800 + (cp >> 10);
            units[1] = 0xDC00 + (cp & 0x3FF);
            count    = 2;
        } else {
            units[0] = cp;
        }
        for (int u = 0; u < count; u++, len++) {
            fnv1a ^= units[u];
            fnv1a *= 0x01000193u; /* Math.imul, then >>> 0 */
            /* k1 *= c1: exact in a double (< 2^53); the shifts coerce to int32 */
            int32_t k = to_int32((double)units[u] * (double)c1);
            k = (int32_t)(((uint32_t)k << 15) | ((uint32_t)k >> 17));
            /* k1 *= c2: an int32 times ~2^29 overflows 2^53, rounded like JS */
            k = to_int32((double)k * (double)c2);
            h1 ^= k;
            h1 = (int32_t)(((uint32_t)h1 << 13) | ((uint32_t)h1 >> 19));
            /* h1 = h1 * 5 + n: exact in a double; coerced by the next ^= */
            h1 = to_int32((double)h1 * 5.0 + 3864292196.0);
        }
    }
    h1 ^= (int32_t)(uint32_t)len;
    uint32_t h = (uint32_t)h1;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    *murmur = h;
    *fnv    = fnv1a;
}

void ls_mixed_hash(const char *src, size_t n, char out[16])
{
    /* mixedHash(`${src.length}${src}`, 1, FNV epoch): the length prefix is
     * hashed first, then the string, as one stream. */
    char pre[16];
    int pn = snprintf(pre, sizeof(pre), "%u", (unsigned)ls_utf16_len(src, n));
    static char buf[4096]; /* not on the caller's stack; only the sync task hashes */
    uint32_t m, f;
    if ((size_t)pn + n <= sizeof(buf)) {
        memcpy(buf, pre, (size_t)pn);
        memcpy(buf + pn, src, n);
        mixed_hash(buf, (size_t)pn + n, 1, 2166136261u, &m, &f);
    } else {
        /* Only the passphrase salt and short strings come through here. */
        mixed_hash(pre, (size_t)pn, 1, 2166136261u, &m, &f);
    }
    char a[8], b[8];
    ls_base36_u32(m, a);
    ls_base36_u32(f, b);
    snprintf(out, 16, "%s%s", a, b);
}

void ls_hashed_passphrase(const char *passphrase, char out[16])
{
    static const char SALT_OF_ID[] = "a83hrf7f\x03y7sa8g31";
    /* passphrase.substring(0, ~~((len / 4) * 3)) in UTF-16 units */
    size_t n     = strlen(passphrase);
    size_t units = ls_utf16_len(passphrase, n);
    size_t keep  = (units * 3) / 4;
    size_t bytes = 0, seen = 0;
    while (bytes < n && seen < keep) {
        uint32_t cp;
        size_t adv = utf8_next((const unsigned char *)passphrase + bytes, n - bytes, &cp);
        size_t u   = cp >= 0x10000 ? 2 : 1;
        if (seen + u > keep) break;
        seen += u;
        bytes += adv;
    }
    char buf[sizeof(SALT_OF_ID) + 256];
    size_t pre = sizeof(SALT_OF_ID) - 1;
    memcpy(buf, SALT_OF_ID, pre);
    if (bytes > sizeof(buf) - pre) bytes = sizeof(buf) - pre;
    memcpy(buf + pre, passphrase, bytes);
    ls_mixed_hash(buf, pre + bytes, out);
}

void ls_chunk_id(const char *piece, size_t n, const char *hashed_pass, char *out)
{
    /* xxh64(`${piece}-${hashedPassphrase}-${piece.length}`), or without the
     * passphrase part when E2EE is off. */
    char tail[48];
    int tn = hashed_pass ? snprintf(tail, sizeof(tail), "-%s-%u", hashed_pass, (unsigned)ls_utf16_len(piece, n))
                         : snprintf(tail, sizeof(tail), "-%u", (unsigned)ls_utf16_len(piece, n));
    xxh64_t s;
    xxh64_init(&s, 0);
    xxh64_update(&s, piece, n);
    xxh64_update(&s, tail, (size_t)tn);
    char b36[16];
    ls_base36_u64(xxh64_digest(&s, 0), b36);
    snprintf(out, 24, "%s%s", hashed_pass ? "h:+" : "h:", b36);
}
