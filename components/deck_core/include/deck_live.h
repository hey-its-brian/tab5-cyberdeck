/*
 * The live panel on the home screen: up to three sections drawn by enabled
 * optional apps through on_panel, in registration order. Tapping a section
 * opens its app. With nothing to show it carries a quiet DECK//OS mark.
 */
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *deck_live_create(lv_obj_t *parent, int32_t w, int32_t h);

#ifdef __cplusplus
}
#endif
