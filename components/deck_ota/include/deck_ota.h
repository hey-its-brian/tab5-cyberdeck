/*
 * Over-the-air firmware updates from GitHub Releases.
 *
 * Releases carry an app-only image named "*-ota.bin" next to the full USB
 * image. ota_check() finds the newest release (optionally including
 * pre-releases), ota_install() streams it into the idle app slot, verifies
 * it and reboots into it. A freshly installed build must run for
 * OTA_CONFIRM_S seconds before it is marked good; if it resets earlier the
 * bootloader rolls back to the previous build.
 *
 * Network work runs on a task; the UI polls ota_state().
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_CONFIRM_S 15

typedef enum {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_AVAILABLE,
    OTA_DOWNLOADING,
    OTA_REBOOTING,
    OTA_ERROR,
} ota_state_t;

/* Call once at boot with the running version ("0.5.0"). Starts the
 * mark-valid countdown if this is a fresh, unconfirmed build. */
void ota_init(const char *running_version);

void ota_check(bool include_prereleases);
void ota_install(void);

ota_state_t ota_state(void);
const char *ota_running(void);
const char *ota_latest(void);       /* "0.5.1" once a check found one */
const char *ota_notes(void);        /* release notes (markdown), may be "" */
const char *ota_error(void);
int         ota_progress(void);     /* 0..100 while downloading */

/* True on the first boot of a build installed over the air. */
bool ota_just_updated(void);
/* True while this build still has to prove itself (rollback armed). */
bool ota_pending_verify(void);

/* Compare "MAJOR.MINOR.PATCH" (leading "v" and "-suffix" ignored).
 * Returns >0 if a is newer than b. Exposed for tests. */
int ota_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
