#include "platform/sensors/polled_inertial_sensor_peripheral.hpp"

#include <array>
#include <cstdint>

#include "esp_err.h"
#include "esp_log.h"

namespace micropixel::platform::sensors {

PolledInertialSensorPeripheral::PolledInertialSensorPeripheral(drivers::VectorSensor& acceleration,
                                                               drivers::VectorSensor& angular_velocity,
                                                               PolledInertialSensorConfig config)
    : acceleration_(acceleration),
      angular_velocity_(angular_velocity),
      config_(config),
      channels_{{{kAcceleration, MICROPIXEL_SENSOR_ACCELERATION, acceleration, config.acceleration_timer_name},
                 {kAngularVelocity, MICROPIXEL_SENSOR_ANGULAR_VELOCITY, angular_velocity,
                  config.angular_velocity_timer_name}}},
      peripheral_(channels_, config.log_tag) {}

void PolledInertialSensorPeripheral::Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor) {
    if (bus == nullptr) {
        ESP_LOGW(config_.log_tag, "shared I2C bus unavailable");
        return;
    }
    struct Request final {
        PolledInertialSensorPeripheral* peripheral;
        i2c_master_bus_handle_t bus;
    } request{this, bus};
    const esp_err_t invoked = i2c_executor.Invoke(
        buses::I2cExecutor::Priority::kNormal,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.peripheral->InitializeOnWorker(requested.bus);
            return ESP_OK;
        },
        &request);
    if (invoked != ESP_OK || (!acceleration_.available() && !angular_velocity_.available())) {
        ESP_LOGW(config_.log_tag, "%s unavailable", config_.model);
        return;
    }

    if (const esp_err_t status = peripheral_.Initialize(i2c_executor); status != ESP_OK) {
        ESP_LOGW(config_.log_tag, "sampler initialization failed: %s", esp_err_to_name(status));
    }
}

void PolledInertialSensorPeripheral::InitializeOnWorker(i2c_master_bus_handle_t bus) {
    if (const esp_err_t status = acceleration_.Initialize(bus); status != ESP_OK) {
        ESP_LOGW(config_.log_tag, "%s initialization failed: %s", config_.model, esp_err_to_name(status));
    }
}

int32_t PolledInertialSensorPeripheral::GetInfo(device::PeripheralChannelId channel,
                                                micropixel_sensor_info_t& info_out) const {
    return peripheral_.GetInfo(channel, info_out);
}

int32_t PolledInertialSensorPeripheral::Start(device::PeripheralChannelId channel, uint32_t interval_us) {
    return peripheral_.Start(channel, interval_us);
}

int32_t PolledInertialSensorPeripheral::Read(device::PeripheralChannelId channel, device::SensorValues& values_out) {
    return peripheral_.Read(channel, values_out);
}

void PolledInertialSensorPeripheral::Stop(device::PeripheralChannelId channel) { peripheral_.Stop(channel); }

}  // namespace micropixel::platform::sensors
