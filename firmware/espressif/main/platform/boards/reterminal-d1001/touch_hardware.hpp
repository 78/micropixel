// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

namespace micropixel::platform::reterminal_d1001 {

class BoardHardware;
struct D1001State;

// Create the touch I2C bus, the GSL3670 panel IO and the controller itself.
// The PCA9535 expander must already be initialized: the driver pulses the
// controller's reset line through the callback in BoardHardware.
[[nodiscard]] esp_err_t InitializeTouchHardware(D1001State& state, BoardHardware& hardware);

}  // namespace micropixel::platform::reterminal_d1001