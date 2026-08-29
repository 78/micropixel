#ifndef MICROPIXEL_FIRMWARE_NETWORK_TIME_HPP
#define MICROPIXEL_FIRMWARE_NETWORK_TIME_HPP

#include "esp_err.h"

namespace micropixel::firmware::network_time {

// Invoked from the SNTP callback after the system UTC clock has been updated.
// The sink must be non-blocking.
using TimeStateChangeSink = void (*)(void* context);

// Starts the Host-wide SNTP client. The ESP-IDF service owns its lifetime and
// periodically maintains the system UTC independently of Remote Control.
[[nodiscard]] esp_err_t Initialize(TimeStateChangeSink sink, void* context);

}  // namespace micropixel::firmware::network_time

#endif
