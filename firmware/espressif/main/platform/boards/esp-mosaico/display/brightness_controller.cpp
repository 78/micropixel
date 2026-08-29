#include "platform/boards/esp-mosaico/display/brightness_controller.hpp"

#include "esp_lcd_co5300.h"
#include "esp_lv_adapter.h"
#include "platform/controllers/brightness_curve.hpp"

namespace micropixel::platform::esp_mosaico {
namespace {

inline constexpr uint8_t kMinimumUserBrightnessPercent = 30U;
inline constexpr uint32_t kBrightnessOutputFloor =
    controllers::BrightnessOutputPerTenThousand(kMinimumUserBrightnessPercent);

}  // namespace

esp_err_t SetDisplayBrightness(esp_lcd_panel_handle_t panel, uint8_t percent) {
    if (panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t locked = esp_lv_adapter_lock(-1);
    if (locked != ESP_OK) {
        return locked;
    }
    const esp_err_t status = esp_lcd_panel_co5300_set_brightness(
        panel, controllers::VisibleBrightnessPercent(percent, kBrightnessOutputFloor));
    esp_lv_adapter_unlock();
    return status;
}

}  // namespace micropixel::platform::esp_mosaico
