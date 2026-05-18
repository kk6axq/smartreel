// Tiny shim so LVGL's two-argument allocator hooks can call
// heap_caps_* (which take a 3rd `caps` argument).
#ifndef LV_HEAP_CAPS_GLUE_H
#define LV_HEAP_CAPS_GLUE_H

#include <esp_heap_caps.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void* lv_heap_psram_malloc(size_t s) {
    return heap_caps_malloc(s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void  lv_heap_psram_free(void* p) {
    heap_caps_free(p);
}
static inline void* lv_heap_psram_realloc(void* p, size_t s) {
    return heap_caps_realloc(p, s, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#ifdef __cplusplus
}
#endif

#endif
