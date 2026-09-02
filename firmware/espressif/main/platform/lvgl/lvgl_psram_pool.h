#ifndef MICROPIXEL_PLATFORM_LVGL_LVGL_PSRAM_POOL_H
#define MICROPIXEL_PLATFORM_LVGL_LVGL_PSRAM_POOL_H

#include "esp_heap_caps.h"

#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#endif
