#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_POWER_CONTROLLER_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_POWER_CONTROLLER_HPP

#include "device/contracts/power.hpp"
#include "esp_err.h"
#include "platform/audio/audio_power_controller.hpp"

namespace micropixel::platform::esp_mosaico {

class PowerController final : public device::Power, public audio::AudioPowerController {
   public:
    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t SetPeripheralPower(bool enabled);

    [[nodiscard]] esp_err_t PowerOnAudio() override;
    [[nodiscard]] esp_err_t PowerOffAudio() override;

    void SetPowerButtonSink(device::PowerButtonSink sink, void* context) override;
    void SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context) override;
    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override;
    [[noreturn]] void PowerOff() override;

   private:
    [[nodiscard]] esp_err_t SetCodecPower(bool enabled);

    bool initialized_{};
    bool peripheral_power_enabled_{};
    bool codec_power_enabled_{};
};

}  // namespace micropixel::platform::esp_mosaico

#endif
