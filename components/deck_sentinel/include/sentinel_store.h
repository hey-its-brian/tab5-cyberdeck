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
 * Plain stdio on hal_storage_root(), the same code on the device and in the
 * simulator. Not thread safe: on the device one writer task makes every
 * call except sentinel_store_tail(), which only reads.
 * sentinel_core formats the rows and the pcap headers.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

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

/* The end of today's events file, starting at the beginning of a row, as a
 * NUL-terminated string of at most n - 1 bytes (the header row is included
 * when the whole file fits). Returns the length, 0 when there is no file. */
int sentinel_store_tail(char *out, size_t n);

#ifdef __cplusplus
}
#endif
