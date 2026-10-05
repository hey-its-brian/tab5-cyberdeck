/*
 * File portal: a small web file manager for the deck's storage, served on
 * the LAN at http://deck.local (or the IP). A 6 digit PIN, shown on the
 * deck and new every time the portal starts, gates access.
 *
 * The server runs on its own task; these functions are safe to call from
 * the LVGL task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORTAL_IDLE_OFF_S (15 * 60)   /* stops itself after this long unused */

typedef struct {
    char text[72];
    bool error;
} portal_event_t;

/* Start/stop. portal_start fails without storage or a network connection. */
bool portal_start(void);
void portal_stop(void);
bool portal_running(void);

const char *portal_pin(void);
const char *portal_host(void);      /* "deck.local" */

/* Copy events newer than *cursor (oldest first); updates *cursor. */
int portal_events(portal_event_t *out, int max, uint32_t *cursor);

/* Seconds since the last request, and whether an upload is in progress. */
uint32_t portal_idle_s(void);
bool     portal_busy(void);

/* Call about once a second: stops the portal when it has been idle too long
 * (returns true if it just did). */
bool portal_tick(void);

#ifdef __cplusplus
}
#endif
