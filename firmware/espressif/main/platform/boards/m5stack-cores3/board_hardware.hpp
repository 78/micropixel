#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "platform/audio/audio_power_controller.hpp"
#include "platform/drivers/power/axp2101.hpp"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::m5stack_cores3 {

class BoardHardware final : public audio::AudioPowerController {
   public:
    [[nodiscard]] esp_err_t Initialize();
    void BindExecutor(buses::I2cExecutor& executor) { executor_ = &executor; }
    [[nodiscard]] esp_err_t EnableDisplayAndTouch();
    [[nodiscard]] esp_err_t RecoverTouch();
    [[nodiscard]] esp_err_t SetBrightness(int percent);
    // Must run on the shared I2C executor.
    [[nodiscard]] esp_err_t ReadBatteryOnBus(drivers::Axp2101BatterySample& sample);
    [[nodiscard]] esp_err_t RequestPowerOff();
    [[nodiscard]] esp_err_t PowerOnAudio() override;
    [[nodiscard]] esp_err_t PowerOffAudio() override;
    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }

   private:
    [[nodiscard]] esp_err_t SetExpanderPin(uint8_t pin, bool enabled);
    [[nodiscard]] esp_err_t SetExpanderInput(uint8_t pin);
    [[nodiscard]] esp_err_t SetBrightnessOnBus(int percent);
    [[nodiscard]] esp_err_t SetAudioPowerOnBus(bool enabled);
    [[nodiscard]] esp_err_t RequestPowerOffOnBus();

    i2c_master_bus_handle_t i2c_bus_{};
    i2c_master_dev_handle_t expander_{};
    drivers::Axp2101 pmic_{};
    buses::I2cExecutor* executor_{};
    bool audio_powered_{};
};

}  // namespace micropixel::platform::m5stack_cores3
