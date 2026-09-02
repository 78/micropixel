#pragma once

#include "driver/i2c_master.h"
#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::drivers {

class Qmi8658 final {
   public:
    enum class Kind : uint8_t {
        kAcceleration,
        kAngularVelocity,
    };

    class Vector final : public VectorSensor {
       public:
        Vector(Qmi8658& sensor, Kind kind) : sensor_(sensor), kind_(kind) {}

        [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus) override;
        [[nodiscard]] esp_err_t Configure(uint32_t interval_us) override;
        [[nodiscard]] esp_err_t Suspend() override;
        [[nodiscard]] esp_err_t Read(float (&values)[3]) override;
        [[nodiscard]] bool available() const override { return sensor_.available(); }
        [[nodiscard]] uint32_t minimum_interval_us() const override { return 2500U; }

       private:
        Qmi8658& sensor_;
        Kind kind_;
    };

    Qmi8658() = default;
    Qmi8658(const Qmi8658&) = delete;
    Qmi8658& operator=(const Qmi8658&) = delete;
    ~Qmi8658();

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus);
    [[nodiscard]] esp_err_t Configure(Kind kind, uint32_t interval_us);
    [[nodiscard]] esp_err_t Suspend(Kind kind);
    [[nodiscard]] esp_err_t Read(Kind kind, float (&values)[3]);
    [[nodiscard]] bool available() const { return device_ != nullptr; }

   private:
    [[nodiscard]] esp_err_t WriteRegister(uint8_t address, uint8_t value);
    [[nodiscard]] esp_err_t ReadRegisters(uint8_t address, uint8_t* data, size_t length);
    [[nodiscard]] esp_err_t ApplyPowerState();
    [[nodiscard]] static uint8_t AccelerationOdr(uint32_t interval_us);
    [[nodiscard]] static uint8_t GyroscopeOdr(uint32_t interval_us);

    i2c_master_dev_handle_t device_{};
    uint8_t acceleration_control_{0x16U};
    uint8_t angular_velocity_control_{0x66U};
    bool acceleration_active_{};
    bool angular_velocity_active_{};
};

}  // namespace micropixel::platform::drivers
