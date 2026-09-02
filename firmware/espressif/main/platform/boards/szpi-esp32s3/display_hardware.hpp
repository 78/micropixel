#pragma once

#include "esp_err.h"

namespace micropixel::platform::esp32_s3_common {
struct Landscape320State;
}

namespace micropixel::platform::szpi_esp32s3 {

class BoardHardware;

[[nodiscard]] esp_err_t InitializeDisplayHardware(BoardHardware& hardware, esp32_s3_common::Landscape320State& state);
[[nodiscard]] esp_err_t InitializeTouch(BoardHardware& hardware, esp32_s3_common::Landscape320State& state);

}  // namespace micropixel::platform::szpi_esp32s3
