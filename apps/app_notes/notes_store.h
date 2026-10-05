/*
 * Markdown files in <storage root>/notes. Plain POSIX I/O, so the same code
 * runs on the SD card, the internal flash partition and the simulator.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define NOTES_NAME_MAX 64
#define NOTES_FILE_MAX (256 * 1024)

typedef struct {
    char name[NOTES_NAME_MAX];
    long size;
    time_t mtime;
} note_entry_t;

/* Directory holding the notes (created if needed), or NULL without storage. */
const char *notes_dir(void);

/* Up to `max` notes, newest first. Returns the count. */
int notes_list(note_entry_t *out, int max);

/* Whole file, NUL terminated, caller frees. NULL on error. */
char *notes_read(const char *name, size_t *len);

/* Writes atomically enough for FAT: temp file, then replace. */
bool notes_write(const char *name, const char *text, size_t len);

bool notes_delete(const char *name);
bool notes_rename(const char *from, const char *to);
bool notes_exists(const char *name);

/* Turn user input into a safe file name ending in .md. Returns false if
 * nothing usable is left. */
bool notes_sanitize(const char *in, char *out, size_t n);

/* First run: drop a README.md that shows off the preview. */
void notes_seed(void);
