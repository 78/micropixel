#pragma once

#include "device/contracts/gpio.hpp"
#include "esp_err.h"

namespace micropixel::platform::esp_mosaico {

class StatusLed final : public device::GpioPeripheral {
   public:
    static constexpr device::PeripheralChannelId kChannel = 1U;

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t Set(bool enabled);

    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel, micropixel_gpio_info_t& info_out) const override;
    [[nodiscard]] int32_t Open(device::PeripheralChannelId channel, uint16_t mode, uint16_t pull, uint16_t edge,
                               uint32_t initial_value, uint32_t pwm_frequency_hz,
                               device::GpioPeripheralEdgeSink edge_sink, void* edge_context) override;
    [[nodiscard]] int32_t Read(device::PeripheralChannelId channel, bool& value_out) const override;
    [[nodiscard]] int32_t Write(device::PeripheralChannelId channel, bool value) override;
    [[nodiscard]] int32_t SetPwmDuty(device::PeripheralChannelId channel, uint16_t duty_per_mille) override;
    void SuspendEvents() override {}
    [[nodiscard]] int32_t ResumeEvents() override { return MICROPIXEL_STATUS_OK; }
    void Close(device::PeripheralChannelId channel) override;

   private:
    bool initialized_{};
    bool active_{};
    bool enabled_{};
};

}  // namespace micropixel::platform::esp_mosaico
