#pragma once

#include <cstdint>

#include "icm42670.h"
#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::drivers {

class Icm42670 final {
   public:
    enum class Kind : uint8_t { kAcceleration, kAngularVelocity };

    class Vector final : public VectorSensor {
       public:
        Vector(Icm42670& sensor, Kind kind) : sensor_(sensor), kind_(kind) {}

        [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus) override;
        [[nodiscard]] esp_err_t Configure(uint32_t interval_us) override;
        [[nodiscard]] esp_err_t Suspend() override;
        [[nodiscard]] esp_err_t Read(float (&values)[3]) override;
        [[nodiscard]] bool available() const override { return sensor_.available(); }
        [[nodiscard]] uint32_t minimum_interval_us() const override { return 2500U; }

       private:
        Icm42670& sensor_;
        Kind kind_;
    };

    ~Icm42670();

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus);
    [[nodiscard]] esp_err_t Configure(Kind kind, uint32_t interval_us);
    [[nodiscard]] esp_err_t Suspend(Kind kind);
    [[nodiscard]] esp_err_t Read(Kind kind, float (&values)[3]);
    [[nodiscard]] bool available() const { return handle_ != nullptr; }

   private:
    [[nodiscard]] esp_err_t ApplyConfiguration();
    [[nodiscard]] esp_err_t ApplyPowerState();
    static icm42670_acce_odr_t AccelerationOdr(uint32_t interval_us);
    static icm42670_gyro_odr_t GyroscopeOdr(uint32_t interval_us);

    icm42670_handle_t handle_{};
    icm42670_cfg_t configuration_{
        .acce_fs = ACCE_FS_2G,
        .acce_odr = ACCE_ODR_100HZ,
        .gyro_fs = GYRO_FS_2000DPS,
        .gyro_odr = GYRO_ODR_100HZ,
    };
    bool acceleration_active_{};
    bool angular_velocity_active_{};
};

}  // namespace micropixel::platform::drivers
