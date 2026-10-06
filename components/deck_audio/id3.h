/* ID3 tags: title and artist for the player, and where the audio starts. */
#pragma once

#include <stdio.h>
#include <stdint.h>

typedef struct {
    char title[96];
    char artist[96];
    uint32_t audio_start; /* byte offset past an ID3v2 tag (0 if none) */
    uint32_t audio_end;   /* file size, minus a trailing ID3v1 tag */
} id3_info_t;

/* Fill `out` from an open file (any position; it is rewound). Missing tags
 * leave the strings empty. Text is converted to UTF-8. */
void id3_read(FILE *f, id3_info_t *out);
