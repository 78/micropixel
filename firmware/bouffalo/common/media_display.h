// SPDX-License-Identifier: Apache-2.0
#ifndef LVGL_MEDIA_DESKTOP_MEDIA_DISPLAY_H
#define LVGL_MEDIA_DESKTOP_MEDIA_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

bool media_display_enter_video_mode(void);
bool media_display_leave_video_mode(void);
bool media_display_is_video_mode(void);

/* Returns the framebuffer currently owned by scanout. */
const void *media_display_get_scanout(lv_color_format_t *color_format);

/* Hold the graphics lock across acquisition, presentation, screenshots and panel
 * controls. DBI retains one buffer and blocks until DMA completes; DSI uses a
 * back buffer and switches at a panel frame boundary. */
void media_display_set_rgb565_byte_swap(bool enabled);
void media_display_set_panel_colors(bool bgr, bool inverted);
bool media_display_begin_direct_rgb565(void);
uint16_t *media_display_acquire_direct_rgb565(uint32_t *stride_pixels);
/* Copy a full native frame into the acquired back buffer. Call under the
 * graphics lock; the DMA channel is shared with the paused LVGL path. */
bool media_display_copy_direct_rgb565(const uint8_t *source, uint32_t source_stride, uint16_t *destination);
bool media_display_present_direct_rgb565(uint16_t *buffer, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
bool media_display_end_direct_rgb565(void);
bool media_display_is_direct_rgb565(void);

#ifdef __cplusplus
}
#endif

#endif
