// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/ksdiy-p4c5/sensor_peripheral.hpp"

#include "platform/boards/ksdiy-p4c5/board_config.hpp"

namespace micropixel::platform::ksdiy_p4c5 {

void SensorPeripheral::Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor) {
    peripheral_.Initialize(bus, i2c_executor);
}

int32_t SensorPeripheral::GetInfo(device::PeripheralChannelId channel, micropixel_sensor_info_t& info_out) const {
    return peripheral_.GetInfo(channel, info_out);
}

int32_t SensorPeripheral::Start(device::PeripheralChannelId channel, uint32_t interval_us) {
    return peripheral_.Start(channel, interval_us);
}

int32_t SensorPeripheral::Read(device::PeripheralChannelId channel, device::SensorValues& values_out) {
    const int32_t status = peripheral_.Read(channel, values_out);
    if (status == MICROPIXEL_STATUS_OK && (channel == kAcceleration || channel == kAngularVelocity)) {
        // Both IMU channels share the mounting frame: swap X/Y and invert all axes.
        const float sensor_x = values_out.values[0];
        values_out.values[0] = -values_out.values[1];
        values_out.values[1] = -sensor_x;
        values_out.values[2] = -values_out.values[2];
    }
    if (status == MICROPIXEL_STATUS_OK && board::kLandscape) {
        // Rotate native sensor X/Y with the display: X=-rawY, Y=rawX.
        const float sensor_x = values_out.values[0];
        values_out.values[0] = -values_out.values[1];
        values_out.values[1] = sensor_x;
    }
    return status;
}

void SensorPeripheral::Stop(device::PeripheralChannelId channel) { peripheral_.Stop(channel); }

}  // namespace micropixel::platform::ksdiy_p4c5
