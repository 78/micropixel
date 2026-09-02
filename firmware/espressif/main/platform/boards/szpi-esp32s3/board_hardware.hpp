#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::szpi_esp32s3 {

class BoardHardware final {
   public:
    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t SelectDisplay();
    [[nodiscard]] esp_err_t SetBrightness(int percent);
    [[nodiscard]] esp_err_t SetAmplifier(bool enabled, buses::I2cExecutor& executor);
    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }

   private:
    [[nodiscard]] esp_err_t WriteExpanderOutput(uint8_t value);

    i2c_master_bus_handle_t i2c_bus_{};
    i2c_master_dev_handle_t expander_{};
    uint8_t expander_output_{0x03U};
    bool brightness_initialized_{};
};

}  // namespace micropixel::platform::szpi_esp32s3
