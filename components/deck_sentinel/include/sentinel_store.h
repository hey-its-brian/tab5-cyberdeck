/*
 * Where RF WATCH keeps what it saw: files under <storage root>/sentinel/.
 *
 *   events-YYYYMMDD.csv            one row per event, with the position when
 *                                  there is a fix. Opened, appended and closed
 *                                  per event, so a power cut loses nothing
 *                                  already written. SD card or internal flash.
 *   cap-<link>-YYYYMMDD-HHMMSS.pcap  captured frames, one file per link per
 *                                  session, opened on the first packet.
 *                                  SD card only: captures would fill the
 *                                  3.8 MB internal partition in minutes.
 *
 *   wigle-YYYYMMDD-HHMMSS.csv      a wardrive, in WiGLE's format, and
 *   survey-YYYYMMDD-HHMMSS.csv     the two swept bands along the same
 *                                  route. One pair per drive, synced to the
 *                                  card every few seconds so a power cut
 *                                  costs seconds, not the drive. SD only.
 *
 *   site-YYYYMMDD-HHMMSS[-name].csv  a site survey: every channel of the two
 *                                  swept bands over a minute. Written whole
 *                                  in one call; SD card or internal flash.
 *
 * Plain stdio on hal_storage_root(), the same code on the device and in the
 * simulator. Not thread safe: on the device one writer task makes every
 * call except sentinel_store_tail(), which only reads.
 * sentinel_core formats the rows and the pcap headers.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sentinel_bandhost.h"
#include "sentinel_core.h"
#include "sentinel_wigle.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t events, events_dropped;   /* rows written; rows lost (no storage, a failed write, a full queue) */
    uint32_t packets, packets_dropped; /* the same for captured frames */
    uint32_t drive_rows, drive_dropped; /* and for the rows of a wardrive (both files) */
    bool     drive_open;                /* a drive's files are open */
    char     drive_file[32];            /* the WiGLE file of the current or the last drive */
    char     cap_file[SENTINEL_LINK_N][48]; /* the current or the last capture file per link, "" before any */
    bool     cap_failing[SENTINEL_LINK_N];  /* frames are arriving and the card is not taking them */
} sentinel_store_stats_t;

void sentinel_store_event(const sentinel_event_t *e, uint8_t fix, double lat, double lon);
void sentinel_store_packet(const sentinel_pkt_t *p);
/* Push buffered capture and drive data to the card (the writer calls it when idle). */
void sentinel_store_flush(void);
/* End the session's capture files; the next packet starts new ones. */
void sentinel_store_close(void);
void sentinel_store_stats(sentinel_store_stats_t *out);

/* A wardrive's two files. Open writes the headers (into empty files only)
 * and is false when there is no SD card or the files cannot be made; rows
 * with no drive open are counted as dropped. */
typedef enum { SENTINEL_DRIVE_WIGLE, SENTINEL_DRIVE_SURVEY, SENTINEL_DRIVE_FILES } sentinel_drive_file_t;
bool sentinel_store_drive_open(const sentinel_wigle_id_t *id);
void sentinel_store_drive_row(sentinel_drive_file_t which, const char *row, size_t len);
void sentinel_store_drive_close(void);

/* Write a site survey file from the band statistics (sentinel_bandhost.h)
 * and put its name in `name` (SENTINEL_SITE_NAME_MAX holds any). `tag` is
 * what the place is called, NULL or "" for nothing: it goes in the first
 * row and, as letters, digits and dashes, in the file's name. Unlike the rest of this file it keeps nothing
 * between calls, so it may be called from any task; it blocks for as long as
 * 150 short lines take to write. False with no storage, a failed write, or
 * nothing swept on either band. A band with nothing swept has blank rows. */
bool sentinel_store_site(const sentinel_bandhost_t *h, const char *time_text, const sentinel_where_t *w, const char *tag,
                         uint32_t now_ms, char *name, size_t name_n);

/* The same, with the time and place taken from the node's state: GPS time
 * and position when there is a fix, the deck's own clock and no position
 * when there is not. */
bool sentinel_store_site_from(const sentinel_bandhost_t *h, const sentinel_state_t *s, const char *tag, uint32_t now_ms, char *name,
                              size_t name_n);

/* The end of today's events file, starting at the beginning of a row, as a
 * NUL-terminated string of at most n - 1 bytes (the header row is included
 * when the whole file fits). Returns the length, 0 when there is no file. */
int sentinel_store_tail(char *out, size_t n);

#ifdef __cplusplus
}
#endif
