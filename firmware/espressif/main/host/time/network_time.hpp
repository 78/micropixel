#ifndef MICROPIXEL_FIRMWARE_NETWORK_TIME_HPP
#define MICROPIXEL_FIRMWARE_NETWORK_TIME_HPP

#include "esp_err.h"

namespace micropixel::firmware::network_time {

// Starts the Host-wide SNTP client. The ESP-IDF service owns its lifetime and
// periodically maintains the system UTC independently of Remote Control.
[[nodiscard]] esp_err_t Initialize();

}  // namespace micropixel::firmware::network_time

#endif
