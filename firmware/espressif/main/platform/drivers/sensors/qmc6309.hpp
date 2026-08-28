#pragma once

#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::drivers {

class Qmc6309 final : public VectorSensor {
   public:
    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus) override;
    [[nodiscard]] esp_err_t Configure(uint32_t interval_us) override;
    [[nodiscard]] esp_err_t Suspend() override;
    [[nodiscard]] esp_err_t Read(float (&values)[3]) override;
    [[nodiscard]] bool available() const override { return device_ != nullptr; }
    [[nodiscard]] uint32_t minimum_interval_us() const override { return 5000U; }

   private:
    i2c_master_dev_handle_t device_{};
};

}  // namespace micropixel::platform::drivers
