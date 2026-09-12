// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <mutex>
inline constexpr int ESP_OK = 0;
inline constexpr int LV_RESULT_OK = 0;
inline constexpr int LV_COLOR_FORMAT_RGB888 = 0;
struct lv_draw_buf_t {};
inline std::recursive_mutex test_lvgl_mutex;
inline int esp_lv_adapter_lock(int) {
    test_lvgl_mutex.lock();
    return ESP_OK;
}
inline void esp_lv_adapter_unlock() { test_lvgl_mutex.unlock(); }
inline int lv_draw_buf_init(lv_draw_buf_t*, uint32_t, uint32_t, int, uint32_t, void*, uint32_t) { return LV_RESULT_OK; }
inline void lv_draw_buf_flush_cache(lv_draw_buf_t*, const void*) {}
