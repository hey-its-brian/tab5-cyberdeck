/*
 * The byte pipe behind the TERMINAL module. One link at a time.
 *
 *   device:    SSH via libssh2 (term_link_ssh.c)
 *   simulator: a local shell on a pseudo-terminal (sim/term_link_pty.c),
 *              so the emulator can be tested against real programs
 *
 * link_open() returns immediately; connection work happens in the
 * background. The UI polls link_state() and drains link_read() from the
 * LVGL task. link_write()/link_resize() may be called any time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LINK_IDLE,
    LINK_CONNECTING,
    LINK_VERIFY_HOST,   /* waiting for the user to accept an unknown host key */
    LINK_AUTH,
    LINK_OPEN,
    LINK_CLOSED,
} link_state_t;

typedef struct {
    char host[64];
    uint16_t port;
    char user[32];
    char password[64];  /* empty: use the device key */
} link_params_t;

bool         link_open(const link_params_t *p, int cols, int rows);
void         link_write(const char *data, size_t len);
void         link_resize(int cols, int rows);
void         link_close(void);
size_t       link_read(char *buf, size_t max);
link_state_t link_state(void);
const char  *link_status(void);      /* human readable progress / error */

/* Host key check (LINK_VERIFY_HOST): fingerprint "SHA256:...", whether it
 * changed from a stored one, and the user's decision. */
const char *link_fingerprint(void);
bool        link_hostkey_changed(void);
void        link_hostkey_decide(bool accept);

/* Device key (generated on first use). Public key in OpenSSH format. */
const char *link_pubkey(void);

/* True on builds that can open a local shell (the simulator). */
bool link_has_local(void);
