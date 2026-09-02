#include "platform/boards/esp32-s3-common/lvgl_display.hpp"

#include "esp_lcd_panel_ops.h"
#include "esp_lv_adapter.h"
#include "platform/boards/esp32-s3-common/landscape_320_state.hpp"

namespace micropixel::platform::esp32_s3_common {

esp_err_t RegisterLvglDisplay(Landscape320State& state, bool double_buffer) {
    if (state.panel == nullptr || state.panel_io == nullptr || state.display != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_config{};
        adapter_config.task_stack_size = 10240U;
        adapter_config.task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
        adapter_config.task_core_id = kLvglTaskCore;
        adapter_config.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
#ifdef ESP_LV_ADAPTER_HAS_TICK_MODE
        adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_PERIODIC;
#endif
        adapter_config.task_min_delay_ms = 1U;
        adapter_config.task_max_delay_ms = 1U;
        adapter_config.stack_in_psram = true;
        adapter_config.auto_sleep.enable = false;
        adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
        adapter_config.auto_sleep.idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS;
        const esp_err_t status = esp_lv_adapter_init(&adapter_config);
        if (status != ESP_OK) {
            return status;
        }
    }

    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.panel;
    display_config.panel_io = state.panel_io;
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_config.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.profile.hor_res = kWidth;
    display_config.profile.ver_res = kHeight;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#if CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.enable_ppa_accel = false;
    display_config.profile.require_double_buffer = double_buffer;
    display_config.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.te_sync = {};
    display_config.te_sync.gpio_num = -1;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    return esp_lcd_panel_disp_on_off(state.panel, true);
}

}  // namespace micropixel::platform::esp32_s3_common
