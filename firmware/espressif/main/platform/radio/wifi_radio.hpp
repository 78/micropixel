#pragma once

#include "esp_err.h"

namespace micropixel::platform::radio {

// Owns only the target-specific radio transport lifecycle. WifiBackend keeps
// scan/connect/persistence policy shared across hosted and native radios.
class WifiRadio {
   public:
    virtual ~WifiRadio() = default;
    WifiRadio(const WifiRadio&) = delete;
    WifiRadio& operator=(const WifiRadio&) = delete;

    [[nodiscard]] virtual esp_err_t Initialize() = 0;
    [[nodiscard]] virtual esp_err_t OnStationStarted() = 0;
    [[nodiscard]] virtual const char* Name() const = 0;

   protected:
    WifiRadio() = default;
};

}  // namespace micropixel::platform::radio
