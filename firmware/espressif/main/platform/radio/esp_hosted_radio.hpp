#pragma once

#include "platform/radio/wifi_radio.hpp"

namespace micropixel::platform::radio {

class EspHostedRadio final : public WifiRadio {
   public:
    [[nodiscard]] esp_err_t Initialize() override;
    [[nodiscard]] esp_err_t OnStationStarted() override;
    [[nodiscard]] const char* Name() const override { return "ESP-Hosted"; }
};

}  // namespace micropixel::platform::radio
