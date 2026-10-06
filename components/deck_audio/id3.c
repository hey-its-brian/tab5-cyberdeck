/*
 * Minimal ID3 reader: TIT2/TPE1 from ID3v2.3 or v2.4 at the start of the
 * file, falling back to an ID3v1 tag at the end. Everything else is skipped.
 */
#include "id3.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static uint32_t syncsafe(const uint8_t *b) { return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F); }
static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

/* Append one code point as UTF-8 (BMP only; others become '?'). */
static size_t put_utf8(char *out, size_t k, size_t n, uint32_t cp)
{
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';
    if (cp < 0x80) {
        if (k + 1 < n) out[k++] = (char)cp;
    } else if (cp < 0x800) {
        if (k + 2 < n) {
            out[k++] = (char)(0xC0 | cp >> 6);
            out[k++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (k + 3 < n) {
        out[k++] = (char)(0xE0 | cp >> 12);
        out[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[k++] = (char)(0x80 | (cp & 0x3F));
    }
    return k;
}

/* ID3 text frame body (encoding byte + text) to UTF-8. */
static void text_frame(const uint8_t *d, size_t len, char *out, size_t n)
{
    size_t k = 0;
    if (len < 1 || n == 0) return;
    uint8_t enc = d[0];
    d++, len--;
    if (enc == 0 || enc == 3) { /* Latin-1, UTF-8 */
        for (size_t i = 0; i < len && d[i]; i++) {
            if (enc == 3 || d[i] < 0x80) {
                if (k + 1 < n) out[k++] = (char)d[i];
            } else {
                k = put_utf8(out, k, n, d[i]);
            }
        }
    } else { /* UTF-16 with BOM (1) or big-endian (2) */
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && len >= 2) {
            be = d[0] == 0xFE && d[1] == 0xFF;
            i  = 2;
        }
        for (; i + 1 < len; i += 2) {
            uint32_t cp = be ? (uint32_t)(d[i] << 8 | d[i + 1]) : (uint32_t)(d[i + 1] << 8 | d[i]);
            if (cp == 0) break;
            k = put_utf8(out, k, n, cp);
        }
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = '\0';
}

static void read_v2(FILE *f, id3_info_t *out)
{
    uint8_t h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) return;
    uint8_t ver    = h[3];
    uint32_t size  = syncsafe(h + 6);
    out->audio_start = 10 + size + ((h[5] & 0x10) ? 10 : 0); /* footer flag */
    if (ver < 3 || ver > 4) return;                             /* v2.2 frames differ: skip */
    if (h[5] & 0x40) {                                          /* extended header */
        uint8_t eh[4];
        if (fread(eh, 1, 4, f) != 4) return;
        uint32_t es = ver == 4 ? syncsafe(eh) : be32(eh) + 4;
        fseek(f, (long)(10 + es), SEEK_SET);
    }
    long end = 10 + (long)size;
    uint8_t fh[10];
    while (ftell(f) + 10 <= end && fread(fh, 1, 10, f) == 10 && fh[0]) {
        uint32_t fs = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        if (fs == 0 || ftell(f) + (long)fs > end) break;
        char *dst = memcmp(fh, "TIT2", 4) == 0 ? out->title : memcmp(fh, "TPE1", 4) == 0 ? out->artist : NULL;
        if (dst && fs < 1024) {
            uint8_t buf[1024];
            if (fread(buf, 1, fs, f) != fs) break;
            text_frame(buf, fs, dst, sizeof(out->title));
        } else {
            fseek(f, (long)fs, SEEK_CUR);
        }
        if (out->title[0] && out->artist[0]) break;
    }
}

static void v1_field(const char *src, char *dst, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < 30 && src[i]; i++) k = put_utf8(dst, k, n, (uint8_t)src[i]);
    while (k > 0 && dst[k - 1] == ' ') k--;
    dst[k] = '\0';
}

void id3_read(FILE *f, id3_info_t *out)
{
    memset(out, 0, sizeof(*out));
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    out->audio_end = size > 0 ? (uint32_t)size : 0;
    rewind(f);
    read_v2(f, out);

    if (size >= 128) {
        char t[128];
        fseek(f, size - 128, SEEK_SET);
        if (fread(t, 1, 128, f) == 128 && memcmp(t, "TAG", 3) == 0) {
            out->audio_end = (uint32_t)(size - 128);
            if (!out->title[0]) v1_field(t + 3, out->title, sizeof(out->title));
            if (!out->artist[0]) v1_field(t + 33, out->artist, sizeof(out->artist));
        }
    }
    if (out->audio_start >= out->audio_end) out->audio_start = 0;
    rewind(f);
}
