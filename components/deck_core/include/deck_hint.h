/*
 * Which APPS tile carries the "enable in settings" hint: the lowest slot
 * whose app is disabled, skipping offline placeholders. A pure function so
 * the host tests can check it without LVGL or settings.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Index of the first slot with disabled[i] && !offline[i], or -1. */
int deck_hint_slot(const bool *disabled, const bool *offline, size_t n);

#ifdef __cplusplus
}
#endif
