// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sdkconfig.h"

/* Shared by LVGL and its consumers; the proxy is initialized before widgets. */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_CUSTOM_DECLARE extern lv_font_t micropixel_system_font_default;
#define LV_FONT_DEFAULT (&micropixel_system_font_default)

/* Never allocate kerning entries on a render-time miss. */
#define LV_TINY_TTF_CACHE_KERNING_CNT 0

/* Existing build directories may retain the former 96 KiB bitmap-font pool.
 * Keep the prepared TTF caches safe while honoring larger configured pools. */
#if CONFIG_LV_MEM_SIZE < (1024U * 1024U)
#define LV_MEM_SIZE (1024U * 1024U)
#endif
