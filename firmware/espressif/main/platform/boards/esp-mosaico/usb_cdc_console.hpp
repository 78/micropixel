#pragma once

#include "esp_err.h"

namespace micropixel::platform {

// Initializes the application-mode CDC endpoint on ESP-Mosaico's onboard
// USB 2.0 HS Type-C connector and redirects standard Host logs to it.
[[nodiscard]] esp_err_t InitializeUsbCdcConsole();

}  // namespace micropixel::platform
