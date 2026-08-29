#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_DISPLAY_BRIGHTNESS_CONTROLLER_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_DISPLAY_BRIGHTNESS_CONTROLLER_HPP

#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

namespace micropixel::platform::esp_mosaico {

[[nodiscard]] esp_err_t SetDisplayBrightness(esp_lcd_panel_handle_t panel, uint8_t percent);

}  // namespace micropixel::platform::esp_mosaico

#endif
