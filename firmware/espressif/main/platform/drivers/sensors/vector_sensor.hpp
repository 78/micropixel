#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform::drivers {

class VectorSensor {
   public:
    virtual ~VectorSensor() = default;
    VectorSensor(const VectorSensor&) = delete;
    VectorSensor& operator=(const VectorSensor&) = delete;

    [[nodiscard]] virtual esp_err_t Initialize(i2c_master_bus_handle_t bus) = 0;
    [[nodiscard]] virtual esp_err_t Configure(uint32_t interval_us) = 0;
    [[nodiscard]] virtual esp_err_t Suspend() = 0;
    [[nodiscard]] virtual esp_err_t Read(float (&values)[3]) = 0;
    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual uint32_t minimum_interval_us() const = 0;

   protected:
    VectorSensor() = default;
};

}  // namespace micropixel::platform::drivers
