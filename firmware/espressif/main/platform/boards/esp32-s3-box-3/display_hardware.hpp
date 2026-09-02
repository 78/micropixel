#pragma once

#include "esp_err.h"
#include "platform/boards/esp32-s3-box-3/platform_state.hpp"

namespace micropixel::platform::esp32_s3_box_3 {

class BoardHardware;

namespace detail {

// Initializes the BOX-3 panel IO with direct PSRAM DMA enabled. The upstream
// BSP helper does not expose this ESP-IDF capability and otherwise stages each
// LVGL draw block through a large internal-SRAM copy.
[[nodiscard]] esp_err_t InitializeDisplayHardware(BoardHardware& hardware, Box3BoardState& state);
[[nodiscard]] esp_err_t InitializeTouchHardware(BoardHardware& hardware, Box3BoardState& state);

}  // namespace detail
}  // namespace micropixel::platform::esp32_s3_box_3
