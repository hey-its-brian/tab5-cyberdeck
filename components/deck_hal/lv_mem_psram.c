/*
 * LVGL's allocator, pointed at PSRAM. Widgets, styles and draw tasks are
 * thousands of small blocks; with the C library's malloc they would all
 * land in internal RAM (anything under 4 KB does), and a big screen such as
 * a vault folder with a hundred rows left too little for the TLS handshakes
 * and DMA buffers that have no other place to go. PSRAM on the P4 is fast
 * enough for widget bookkeeping; the frame buffers were already there.
 */
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <stdlib.h>

#include "esp_heap_caps.h"

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS);
    return p ? p : malloc(size); /* PSRAM full: fall back rather than fail */
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, CAPS);
    return n ? n : realloc(p, new_size);
}

void lv_free_core(void *p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, CAPS);
    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_size  = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_pct = mon_p->total_size ? (uint8_t)(100 - 100 * info.total_free_bytes / mon_p->total_size) : 0;
    mon_p->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif
