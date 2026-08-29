#include <cstdlib>
#include <cstring>

#include "device/device_registry.hpp"

namespace {

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

class SensorPeripheral final : public micropixel::device::SensorPeripheral {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel::device::PeripheralChannelId endpoint,
                                  micropixel_sensor_info_t& info) const override {
        info = {};
        info.size = sizeof(info);
        info.kind = endpoint == 1U ? MICROPIXEL_SENSOR_ACCELERATION : MICROPIXEL_SENSOR_MAGNETIC_FIELD;
        return endpoint == 1U || endpoint == 2U ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_NOT_FOUND;
    }
    [[nodiscard]] int32_t Start(micropixel::device::PeripheralChannelId, uint32_t) override {
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Read(micropixel::device::PeripheralChannelId,
                               micropixel::device::SensorValues&) override {
        return MICROPIXEL_STATUS_OK;
    }
    void Stop(micropixel::device::PeripheralChannelId) override {}
};

class GpioPeripheral final : public micropixel::device::GpioPeripheral {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel::device::PeripheralChannelId endpoint,
                                  micropixel_gpio_info_t& info) const override {
        info = {};
        info.size = sizeof(info);
        info.line_number = endpoint;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Open(micropixel::device::PeripheralChannelId, uint16_t, uint16_t, uint16_t, uint32_t,
                               uint32_t, micropixel::device::GpioPeripheralEdgeSink, void*) override {
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Read(micropixel::device::PeripheralChannelId, bool&) const override {
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Write(micropixel::device::PeripheralChannelId, bool) override {
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t SetPwmDuty(micropixel::device::PeripheralChannelId, uint16_t) override {
        return MICROPIXEL_STATUS_OK;
    }
    void SuspendEvents() override {}
    [[nodiscard]] int32_t ResumeEvents() override { return MICROPIXEL_STATUS_OK; }
    void Close(micropixel::device::PeripheralChannelId) override {}
};

class HapticsPeripheral final : public micropixel::device::HapticsPeripheral {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel::device::PeripheralChannelId,
                                  micropixel_haptics_info_t& info) const override {
        info = {};
        info.size = sizeof(info);
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Play(micropixel::device::PeripheralChannelId, uint16_t, uint32_t) override {
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Stop(micropixel::device::PeripheralChannelId) override { return MICROPIXEL_STATUS_OK; }
    void SetCompletionSink(micropixel::device::HapticsPeripheralCompletionSink, void*) override {}
};

micropixel_device_id_t FindByName(const micropixel::device::DeviceRegistry& registry, const char* name) {
    for (uint32_t index = 0U; index < registry.Count(); ++index) {
        micropixel_device_info_t info{};
        if (registry.GetByIndex(index, info) == MICROPIXEL_STATUS_OK && std::strcmp(info.name, name) == 0) {
            return info.device;
        }
    }
    return 0U;
}

}  // namespace

int main() {
    micropixel::device::DeviceRegistry registry;
    SensorPeripheral sensors;
    GpioPeripheral gpio;
    HapticsPeripheral haptics;

    Require(registry.RegisterDisplay());
    Require(registry.RegisterTouch());
    Require(registry.RegisterAudioOutput());
    Require(registry.RegisterPower());
    Require(registry.RegisterSensor(sensors, 1U, "Built-in accelerometer"));
    Require(registry.RegisterSensor(sensors, 2U, "Built-in magnetometer"));
    Require(registry.RegisterGpio(gpio, 15U, "P15"));
    Require(registry.RegisterHaptics(haptics, 1U, "Built-in vibration motor"));
    Require(registry.Count() == 8U);

    const micropixel_device_id_t gpio_id = FindByName(registry, "P15");
    Require(gpio_id != 0U && gpio_id != 15U);
    micropixel_gpio_info_t gpio_info{};
    Require(registry.GetInfo(gpio_id, gpio_info) == MICROPIXEL_STATUS_OK);
    Require(gpio_info.device == gpio_id && gpio_info.line_number == 15U);

    const micropixel_device_id_t acceleration_id = FindByName(registry, "Built-in accelerometer");
    micropixel_sensor_info_t sensor_info{};
    Require(registry.GetInfo(acceleration_id, sensor_info) == MICROPIXEL_STATUS_OK);
    Require(sensor_info.device == acceleration_id && acceleration_id != 1U);

    const uint32_t generation = registry.Generation();
    registry.Reset();
    Require(registry.Count() == 0U && registry.Generation() != generation);
    return 0;
}
