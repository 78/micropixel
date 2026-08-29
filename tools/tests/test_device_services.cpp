#include <cstdlib>

#include "device/device_services.hpp"

namespace {

constexpr micropixel_device_id_t kSensor = 101U;
constexpr micropixel_device_id_t kGpio = 102U;
constexpr micropixel_device_id_t kPower = 103U;
constexpr micropixel_device_id_t kHaptics = 104U;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

class Catalog final : public micropixel::device::DeviceCatalog {
   public:
    [[nodiscard]] uint32_t Generation() const override { return 7U; }
    [[nodiscard]] uint32_t Count() const override { return 4U; }
    [[nodiscard]] int32_t GetByIndex(uint32_t index, micropixel_device_info_t& info) const override {
        static constexpr micropixel_device_id_t kIds[]{kSensor, kGpio, kPower, kHaptics};
        static constexpr uint16_t kKinds[]{MICROPIXEL_DEVICE_KIND_SENSOR, MICROPIXEL_DEVICE_KIND_GPIO_LINE,
                                           MICROPIXEL_DEVICE_KIND_POWER, MICROPIXEL_DEVICE_KIND_HAPTICS};
        if (index >= 4U) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        info = {};
        info.size = sizeof(info);
        info.device = kIds[index];
        info.kind = kKinds[index];
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t GetById(micropixel_device_id_t device, micropixel_device_info_t& info) const override {
        for (uint32_t index = 0U; index < Count(); ++index) {
            if (GetByIndex(index, info) == MICROPIXEL_STATUS_OK && info.device == device) {
                return MICROPIXEL_STATUS_OK;
            }
        }
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
};

class Sensors final : public micropixel::device::Sensors {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info) const override {
        if (device != kSensor) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        info = {};
        info.size = sizeof(info);
        info.kind = MICROPIXEL_SENSOR_ACCELERATION;
        info.device = device;
        info.value_count = 3U;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Start(micropixel_device_id_t device, uint32_t interval_us) override {
        if (device != kSensor) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        interval = interval_us;
        started = true;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Read(micropixel_device_id_t device, micropixel::device::SensorValues& values) override {
        if (device != kSensor || !started) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        values.values[0] = 1.0F;
        values.values[1] = 2.0F;
        values.values[2] = 3.0F;
        return MICROPIXEL_STATUS_OK;
    }
    void StopSampling(micropixel_device_id_t device) override {
        if (device == kSensor) {
            started = false;
        }
    }

    bool started{};
    uint32_t interval{};
};

class Gpio final : public micropixel::device::Gpio {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_gpio_info_t& info) const override {
        if (device != kGpio) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        info = {};
        info.size = sizeof(info);
        info.device = device;
        info.line_number = 14U;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Open(micropixel_device_id_t device, uint16_t, uint16_t, uint16_t, uint32_t initial_value,
                               uint32_t, micropixel::device::GpioEdgeSink, void*) override {
        if (device != kGpio) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        open = true;
        value = initial_value != 0U;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Read(micropixel_device_id_t device, bool& result) const override {
        if (device != kGpio || !open) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        result = value;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Write(micropixel_device_id_t device, bool next) override {
        if (device != kGpio || !open) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        value = next;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t SetPwmDuty(micropixel_device_id_t device, uint16_t duty) override {
        return device == kGpio && duty <= 1000U ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    void SuspendEvents() override { events_suspended = true; }
    [[nodiscard]] int32_t ResumeEvents() override {
        events_suspended = false;
        return MICROPIXEL_STATUS_OK;
    }
    void Close(micropixel_device_id_t device) override {
        if (device == kGpio) {
            open = false;
        }
    }

    bool open{};
    bool value{};
    bool events_suspended{};
};

class Haptics final : public micropixel::device::Haptics {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_haptics_info_t& info) const override {
        if (device != kHaptics) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        info = {};
        info.size = sizeof(info);
        info.device = device;
        info.maximum_duration_ms = 5000U;
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t Play(micropixel_device_id_t device, uint16_t strength, uint32_t duration) override {
        playing = device == kHaptics && strength <= 1000U && duration <= 5000U;
        return playing ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    [[nodiscard]] int32_t Stop(micropixel_device_id_t device) override {
        if (device != kHaptics) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        playing = false;
        return MICROPIXEL_STATUS_OK;
    }
    void SetCompletionSink(micropixel::device::HapticCompletionSink, void*) override {}

    bool playing{};
};

class Battery final : public micropixel::device::Battery {
   public:
    [[nodiscard]] micropixel::device::BatterySnapshot Snapshot() override {
        return {.percent = 42U,
                .available = true,
                .charging = true,
                .charging_available = true,
                .external_power_connected = true,
                .external_power_available = true};
    }
    void SetStateChangeSink(micropixel::device::BatteryStateChangeSink, void*) override {}
};

}  // namespace

int main() {
    Catalog catalog;
    Sensors sensors;
    Gpio gpio;
    Haptics haptics;
    Battery battery;

    micropixel::device::DevicesService devices_service{catalog};
    auto all = devices_service.List(MICROPIXEL_DEVICE_KIND_ANY);
    Require(all && all->count == 4U && all->generation == 7U && all->devices[1] == kGpio);
    auto filtered = devices_service.List(MICROPIXEL_DEVICE_KIND_SENSOR);
    Require(filtered && filtered->count == 1U && filtered->devices[0] == kSensor);
    Require(!devices_service.List(UINT16_MAX));
    Require(devices_service.GetInfo(kPower)->kind == MICROPIXEL_DEVICE_KIND_POWER);

    micropixel::device::SensorsService sensors_service{sensors};
    Require(sensors_service.GetInfo(kSensor)->value_count == 3U);
    Require(sensors_service.Start(kSensor, 10000U).has_value());
    Require(sensors.interval == 10000U);
    Require(sensors_service.Read(kSensor)->values[2] == 3.0F);
    sensors_service.Stop(kSensor);
    Require(!sensors.started);

    micropixel::device::GpioService gpio_service{gpio};
    Require(gpio_service
                .Open(kGpio, MICROPIXEL_GPIO_MODE_OUTPUT, MICROPIXEL_GPIO_PULL_NONE, MICROPIXEL_GPIO_EDGE_NONE, 0U, 0U,
                      nullptr, nullptr)
                .has_value());
    Require(gpio_service.Write(kGpio, true).has_value());
    Require(*gpio_service.Read(kGpio));
    gpio_service.SuspendEvents();
    Require(gpio.events_suspended);
    Require(gpio_service.ResumeEvents().has_value() && !gpio.events_suspended);
    gpio_service.Close(kGpio);
    Require(!gpio.open);

    micropixel::device::HapticsService haptics_service{haptics};
    Require(haptics_service.GetInfo(kHaptics)->maximum_duration_ms == 5000U);
    Require(haptics_service.Play(kHaptics, 500U, 200U).has_value());
    Require(haptics.playing);
    Require(haptics_service.Stop(kHaptics).has_value());

    micropixel::device::PowerInfoService power_service{catalog, battery};
    auto power = power_service.Get(kPower);
    Require(power && power->battery_percent == 42U && power->source == MICROPIXEL_POWER_SOURCE_EXTERNAL);
    Require((power->flags & MICROPIXEL_POWER_STATE_CHARGING) != 0U);
    Require(!power_service.Get(kSensor));
    return 0;
}
