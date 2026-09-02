#pragma once

#include "esp_err.h"

namespace micropixel::platform::esp32_s3_common {

struct Landscape320State;

[[nodiscard]] esp_err_t RegisterLvglDisplay(Landscape320State& state, bool double_buffer);

}  // namespace micropixel::platform::esp32_s3_common
