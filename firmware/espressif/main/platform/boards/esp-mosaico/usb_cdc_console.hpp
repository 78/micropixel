#pragma once

#include "esp_err.h"

namespace micropixel::platform {

// Starts bounded PSRAM-backed log capture before the USB CDC device exists.
// Calling this more than once is harmless.
[[nodiscard]] bool BeginUsbCdcEarlyLogCapture();

// Initializes the application-mode CDC endpoint on ESP-Mosaico's onboard USB
// 2.0 HS Type-C connector and redirects standard Host logs to it. Early ESP_LOG
// output uses a bounded PSRAM cache that is released after replay or timeout.
[[nodiscard]] esp_err_t InitializeUsbCdcConsole();

}  // namespace micropixel::platform
