#pragma once

#include <cstdint>

#include "bmm150.h"
#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::drivers {

class Bmm150 final : public VectorSensor {
   public:
    explicit Bmm150(uint8_t address) : address_(address) {}
    Bmm150(const Bmm150&) = delete;
    Bmm150& operator=(const Bmm150&) = delete;
    ~Bmm150() override;

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus) override;
    [[nodiscard]] esp_err_t Configure(uint32_t interval_us) override;
    [[nodiscard]] esp_err_t Suspend() override;
    [[nodiscard]] esp_err_t Read(float (&values)[3]) override;
    [[nodiscard]] bool available() const override { return device_handle_ != nullptr; }
    [[nodiscard]] uint32_t minimum_interval_us() const override { return 33334U; }

   private:
    static int8_t ReadRegisters(uint8_t register_address, uint8_t* data, uint32_t length, void* context);
    static int8_t WriteRegisters(uint8_t register_address, const uint8_t* data, uint32_t length, void* context);
    static void DelayUs(uint32_t period_us, void* context);
    [[nodiscard]] static uint8_t DataRate(uint32_t interval_us);

    uint8_t address_{};
    i2c_master_dev_handle_t device_handle_{};
    bmm150_dev device_{};
};

}  // namespace micropixel::platform::drivers
