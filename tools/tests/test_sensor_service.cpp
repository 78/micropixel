#include <cstdlib>

#include "runtime/services/sensor_service.hpp"
#include "runtime/services/timer_service.hpp"

namespace {

constexpr micropixel_device_id_t kSensor = 42U;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

class SensorBackend final : public micropixel::device::SensorBackend {
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
        info.minimum_interval_us = 2500U;
        info.maximum_interval_us = 60000000U;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t Start(micropixel_device_id_t device, uint32_t interval_us) override {
        if (device != kSensor) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        started = true;
        sample_ready = false;
        last_interval_us = interval_us;
        ++start_count;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t Read(micropixel_device_id_t device, micropixel::device::SensorValues& values) override {
        if (device != kSensor || !started) {
            return MICROPIXEL_STATUS_CLOSED;
        }
        if (!sample_ready) {
            return MICROPIXEL_STATUS_WOULD_BLOCK;
        }
        const uint32_t sample_number = ++read_count;
        values.timestamp_us = 1234U;
        values.values[0] = static_cast<float>(sample_number);
        values.values[1] = 2.0F;
        values.values[2] = 3.0F;
        return MICROPIXEL_STATUS_OK;
    }

    void Stop(micropixel_device_id_t device) override {
        if (device == kSensor) {
            started = false;
            sample_ready = false;
            ++stop_count;
        }
    }

    void PublishSample() { sample_ready = true; }

    bool started{};
    bool sample_ready{};
    uint32_t last_interval_us{};
    uint32_t start_count{};
    uint32_t stop_count{};
    uint32_t read_count{};
};

}  // namespace

int main() {
    SensorBackend backend;
    micropixel::device::SensorsService devices{backend};
    micropixel::runtime::TimerService clock;
    micropixel::runtime::SensorService service{devices, clock};
    Require(service.valid());
    Require(!backend.started && backend.start_count == 0U);

    auto opened = service.Open(kSensor, MICROPIXEL_SENSOR_ACCELERATION);
    Require(opened.has_value());
    Require(backend.started && backend.last_interval_us == 10000U);
    auto pending = service.Read(opened->sensor);
    Require(!pending && pending.error().status == MICROPIXEL_STATUS_WOULD_BLOCK);

    auto configured = service.SetSampleInterval(opened->sensor, 5000U);
    Require(configured.has_value() && backend.last_interval_us == 5000U);
    backend.PublishSample();
    auto sample = service.Read(opened->sensor);
    Require(sample.has_value() && sample->values[1] == 2.0F && backend.read_count != 0U);

    service.Suspend();
    Require(!backend.started);
    Require(service.Resume());
    Require(backend.started && backend.last_interval_us == 5000U);

    Require(service.Release(opened->sensor).has_value());
    Require(!backend.started);
    const uint32_t starts_after_release = backend.start_count;
    Require(backend.start_count == starts_after_release);
    return 0;
}
