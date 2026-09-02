#pragma once

#include "esp_err.h"

namespace micropixel::platform::gpio {

[[nodiscard]] esp_err_t EnsureIsrServiceInstalled();

}  // namespace micropixel::platform::gpio
