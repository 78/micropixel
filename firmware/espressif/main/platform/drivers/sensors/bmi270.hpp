#pragma once

#include <cstdint>

#include "bmi270.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::drivers {

class Bmi270 final {
   public:
    enum class Kind : uint8_t {
        kAcceleration,
        kAngularVelocity,
    };

    Bmi270() = default;
    Bmi270(const Bmi270&) = delete;
    Bmi270& operator=(const Bmi270&) = delete;
    ~Bmi270();

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus);
    [[nodiscard]] esp_err_t Configure(Kind kind, uint32_t interval_us);
    [[nodiscard]] esp_err_t Suspend(Kind kind);
    [[nodiscard]] esp_err_t Read(Kind kind, float (&values)[3]);
    [[nodiscard]] bool available() const { return device_handle_ != nullptr; }

   private:
    static int8_t ReadRegisters(uint8_t register_address, uint8_t* data, uint32_t length, void* context);
    static int8_t WriteRegisters(uint8_t register_address, const uint8_t* data, uint32_t length, void* context);
    static void DelayUs(uint32_t period_us, void* context);
    [[nodiscard]] static uint8_t AccelerationOdr(uint32_t interval_us);
    [[nodiscard]] static uint8_t GyroscopeOdr(uint32_t interval_us);

    i2c_master_dev_handle_t device_handle_{};
    bmi2_dev device_{};
};

class Bmi270Vector final : public VectorSensor {
   public:
    Bmi270Vector(Bmi270& device, Bmi270::Kind kind) : device_(device), kind_(kind) {}

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t) override { return ESP_ERR_INVALID_STATE; }
    [[nodiscard]] esp_err_t Configure(uint32_t interval_us) override { return device_.Configure(kind_, interval_us); }
    [[nodiscard]] esp_err_t Suspend() override { return device_.Suspend(kind_); }
    [[nodiscard]] esp_err_t Read(float (&values)[3]) override { return device_.Read(kind_, values); }
    [[nodiscard]] bool available() const override { return device_.available(); }
    [[nodiscard]] uint32_t minimum_interval_us() const override { return 2500U; }

   private:
    Bmi270& device_;
    Bmi270::Kind kind_;
};

}  // namespace micropixel::platform::drivers
