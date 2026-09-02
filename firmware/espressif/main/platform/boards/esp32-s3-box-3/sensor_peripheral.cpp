#include "platform/boards/esp32-s3-box-3/sensor_peripheral.hpp"

namespace micropixel::platform::esp32_s3_box_3 {

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
    return peripheral_.Read(channel, values_out);
}

void SensorPeripheral::Stop(device::PeripheralChannelId channel) { peripheral_.Stop(channel); }

}  // namespace micropixel::platform::esp32_s3_box_3
