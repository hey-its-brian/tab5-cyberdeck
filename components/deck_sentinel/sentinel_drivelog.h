/*
 * A wardrive in progress: the part of deck_sentinel that is the same on the
 * Tab5 and in the simulator. It is handed every access point and Bluetooth
 * device the node reports, asks sentinel_core's drive rules which of them
 * become rows, formats those and passes them to `out` (a queue in front of
 * the SD writer on the device, the store itself in the simulator). Every 5 s
 * with a fix it adds a survey row from the two swept bands.
 *
 * Not thread safe: the caller holds its own lock around every call.
 */
#pragma once

#include "deck_sentinel.h"

#define SENTINEL_DRIVELOG_SLOTS     2048 /* devices remembered for the 25 m rule: 40 KB */
#define SENTINEL_DRIVELOG_SURVEY_MS 5000

/* Takes one line for one of the drive's files. False when it could not be
 * passed on (the queue to the card is full): the sighting then counts as
 * not written, and is written the next time the device is heard. */
typedef bool (*sentinel_drivelog_row_t)(sentinel_drive_file_t file, const char *row, int len);

void sentinel_drivelog_begin(uint32_t now_ms);
void sentinel_drivelog_end(void);
bool sentinel_drivelog_on(void);
void sentinel_drivelog_ap(const sentinel_state_t *s, const sentinel_wbs_t *a, uint32_t now_ms, sentinel_drivelog_row_t out);
void sentinel_drivelog_device(const sentinel_state_t *s, const sentinel_bdv_t *d, uint32_t now_ms,
                              sentinel_drivelog_row_t out);
/* A few times a second: the distance covered, and the survey row when one is due. */
void sentinel_drivelog_tick(const sentinel_state_t *s, uint32_t now_ms, sentinel_drivelog_row_t out);
/* The counts and the last few rows. `writing`, `file` and `lost` are the caller's to fill. */
void sentinel_drivelog_info(sentinel_drive_info_t *out);
