/*
 * Vault sync: keeps <storage>/vault in step with an Obsidian vault through a
 * sync backend. The first backend is Self-hosted LiveSync (CouchDB); the
 * backend interface is here so others can follow.
 *
 * Everything runs on a worker task; the UI polls deck_sync_state() and reads
 * the log. Settings live in NVS under "ls_*".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNC_IDLE,    /* never run this boot */
    SYNC_RUNNING,
    SYNC_DONE,    /* last run finished */
    SYNC_FAILED,  /* last run stopped on an error; see the log */
} sync_state_t;

/* A line of progress or a problem, appended to the log the UI shows. */
typedef void (*sync_log_fn)(const char *line);

typedef struct {
    const char *name; /* "LIVESYNC" */
    /* Is enough configured to try. */
    bool (*configured)(void);
    /* Full two-way sync of `dir`. Blocking; called on the worker task.
     * Returns false on failure after logging why. */
    bool (*sync)(const char *dir, sync_log_fn log, volatile bool *cancel, bool full);
} deck_sync_backend_t;

void deck_sync_set_backend(const deck_sync_backend_t *backend);
const deck_sync_backend_t *deck_sync_backend(void);

bool deck_sync_configured(void);

/* Start a sync on the worker task. False when one is running, nothing is
 * configured, or there is no network. */
bool deck_sync_start(void);
/* The same, but walk the whole database again (notes already on the card
 * are matched by revision and skipped): picks up anything missed. */
bool deck_sync_start_full(void);
void deck_sync_cancel(void);

sync_state_t deck_sync_state(void);
/* Seconds since the last successful sync, -1 if none this boot. */
long deck_sync_age(void);

/* Log lines, newest last; `max` lines of `n` bytes each. Returns the count. */
int deck_sync_log(char (*lines)[96], int max);
/* The newest line, "" if none. */
const char *deck_sync_last(void);

/* The Self-hosted LiveSync (CouchDB) backend. */
extern const deck_sync_backend_t g_sync_livesync;

/* Settings keys the backends share with the UI. */
#define SYNC_KEY_URL  "ls_url"   /* http(s)://host:5984 */
#define SYNC_KEY_DB   "ls_db"
#define SYNC_KEY_USER "ls_user"
#define SYNC_KEY_PASS "ls_pass"
#define SYNC_KEY_PHRASE "ls_phrase" /* E2EE passphrase, "" when off */
#define SYNC_KEY_AUTO "ls_auto"   /* i32: sync when NOTES opens if older than this many minutes, 0 = off */

#ifdef __cplusplus
}
#endif
