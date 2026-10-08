/* term_chat for the simulator: Ollama chats need the device's network
 * stack, so the session says so and closes. */
#include "term_chat.h"

#include <stdio.h>
#include <string.h>

static link_state_t s_state = LINK_IDLE;
static const char *s_msg;

bool chat_open(const char *host, uint16_t port, const char *model, int cols, int rows)
{
    (void)host;
    (void)port;
    (void)model;
    (void)cols;
    (void)rows;
    s_msg   = "\x1b[33mOllama chat runs on the device only.\x1b[0m\r\n";
    s_state = LINK_CLOSED;
    return false;
}

void chat_write(const char *data, size_t len)
{
    (void)data;
    (void)len;
}

void chat_resize(int cols, int rows)
{
    (void)cols;
    (void)rows;
}

void chat_close(void) { s_state = LINK_IDLE; }

size_t chat_read(char *buf, size_t max)
{
    if (s_msg == NULL) return 0;
    size_t n = strlen(s_msg);
    if (n > max) n = max;
    memcpy(buf, s_msg, n);
    s_msg = NULL;
    return n;
}

link_state_t chat_state(void) { return s_state; }
const char *chat_status(void) { return "device only"; }
