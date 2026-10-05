/*
 * Markdown preview: parses CommonMark + GitHub extensions (tables, task
 * lists, strikethrough, autolinks) with md4c and builds LVGL widgets in the
 * DECK//OS style.
 *
 * Inline styles map to color because there are no bold or italic faces:
 *   **strong** accent    *em* secondary accent    `code` green
 *   [links] accent, underlined    ~~del~~ dim, struck through
 */
#pragma once

#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Replace the children of `parent` with the rendered document. `parent`
 * should be a column that scrolls vertically; it is given a column flex
 * layout. Returns the number of top-level blocks rendered. */
int deck_md_render(lv_obj_t *parent, const char *text, size_t len);

#ifdef __cplusplus
}
#endif
