// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_lcd_touch.h"

namespace micropixel::platform::input {

// FT6336-specific read_data callback for an initialized esp_lcd_touch handle.
// Keeps the upstream initialization, coordinate transform and track-ID getters.
[[nodiscard]] esp_err_t ReadFt6336Report(esp_lcd_touch_handle_t touch);

}  // namespace micropixel::platform::input
