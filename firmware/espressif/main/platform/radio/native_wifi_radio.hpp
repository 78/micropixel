#pragma once

#include "platform/radio/wifi_radio.hpp"

namespace micropixel::platform::radio {

class NativeWifiRadio final : public WifiRadio {
   public:
    [[nodiscard]] esp_err_t Initialize() override { return ESP_OK; }
    [[nodiscard]] esp_err_t OnStationStarted() override { return ESP_OK; }
    [[nodiscard]] const char* Name() const override { return "native"; }
};

}  // namespace micropixel::platform::radio
