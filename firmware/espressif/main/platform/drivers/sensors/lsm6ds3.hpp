// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "driver/i2c_master.h"
#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::drivers {

class Lsm6ds3 final {
   public:
    enum class Kind : uint8_t {
        kAcceleration,
        kAngularVelocity,
    };

    class Vector final : public VectorSensor {
       public:
        Vector(Lsm6ds3& sensor, Kind kind) : sensor_(sensor), kind_(kind) {}

        [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus) override;
        [[nodiscard]] esp_err_t Configure(uint32_t interval_us) override;
        [[nodiscard]] esp_err_t Suspend() override;
        [[nodiscard]] esp_err_t Read(float (&values)[3]) override;
        [[nodiscard]] bool available() const override { return sensor_.available(); }
        [[nodiscard]] uint32_t min_interval_us() const override { return 2404U; }

       private:
        Lsm6ds3& sensor_;
        Kind kind_;
    };

    Lsm6ds3() = default;
    Lsm6ds3(const Lsm6ds3&) = delete;
    Lsm6ds3& operator=(const Lsm6ds3&) = delete;
    ~Lsm6ds3();

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus);
    [[nodiscard]] esp_err_t Configure(Kind kind, uint32_t interval_us);
    [[nodiscard]] esp_err_t Suspend(Kind kind);
    [[nodiscard]] esp_err_t Read(Kind kind, float (&values)[3]);
    [[nodiscard]] bool available() const { return device_ != nullptr; }

   private:
    [[nodiscard]] esp_err_t WriteRegister(uint8_t address, uint8_t value);
    [[nodiscard]] esp_err_t ReadRegisters(uint8_t address, uint8_t* data, size_t length);
    [[nodiscard]] static uint8_t OutputDataRate(uint32_t interval_us);

    i2c_master_dev_handle_t device_{};
};

}  // namespace micropixel::platform::drivers
