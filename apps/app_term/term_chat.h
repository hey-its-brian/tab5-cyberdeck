/*
 * Ollama chat behind the TERMINAL screen. Same byte-pipe shape as
 * term_link.h: keys go in through chat_write(), text for the emulator comes
 * out of chat_read(). The prompt, line editing and slash commands live on
 * the chat side, so the terminal only shows what it is fed.
 *
 *   device:    HTTP to Ollama's /api/chat (term_chat_ollama.c)
 *   simulator: a stub that reports it is device-only (sim/term_chat_sim.c)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term_link.h"

#define CHAT_DEFAULT_PORT 11434

/* `model` may be "": the first chat model the server lists is used. */
bool         chat_open(const char *host, uint16_t port, const char *model, int cols, int rows);
void         chat_write(const char *data, size_t len);
void         chat_resize(int cols, int rows);
void         chat_close(void);
size_t       chat_read(char *buf, size_t max);
link_state_t chat_state(void);
const char  *chat_status(void);
