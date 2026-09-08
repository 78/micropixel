#include <cstdlib>

#include "runtime/services/gpio_service.hpp"
#include "runtime/services/sensor_service.hpp"
#include "runtime/services/timer_service.hpp"

namespace {

constexpr micropixel_device_id_t kSensor = 42U;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

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
        info.min_interval_us = 2500U;
        info.max_interval_us = 60000000U;
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

    void StopSampling(micropixel_device_id_t device) override {
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

void TestSensorService() {
    Sensors backend;
    micropixel::device::SensorsService devices{backend};
    micropixel::runtime::TimerService clock;
    micropixel::runtime::SensorService service{devices, clock};
    Require(service.valid());
    Require(!backend.started && backend.start_count == 0U);

    auto opened = service.Open(kSensor, MICROPIXEL_SENSOR_ACCELERATION);
    Require(opened.has_value());
    Require(backend.started && backend.last_interval_us == 10000U);
    auto pending = service.Read(opened->sensor_handle);
    Require(!pending && pending.error().status == MICROPIXEL_STATUS_WOULD_BLOCK);

    auto configured = service.SetSampleInterval(opened->sensor_handle, 5000U);
    Require(configured.has_value() && backend.last_interval_us == 5000U);
    backend.PublishSample();
    auto sample = service.Read(opened->sensor_handle);
    Require(sample.has_value() && sample->values[1] == 2.0F && backend.read_count != 0U);

    service.Suspend();
    Require(!backend.started);
    Require(service.Resume());
    Require(backend.started && backend.last_interval_us == 5000U);

    Require(service.Release(opened->sensor_handle).has_value());
    Require(!backend.started);
}

constexpr micropixel_device_id_t kGpio = 47U;

class Gpio final : public micropixel::device::Gpio {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_gpio_info_t& info) const override {
        if (device != kGpio) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        info = {};
        info.size = sizeof(info);
        info.device = device;
        info.line_number = 47U;
        info.capabilities = MICROPIXEL_GPIO_CAP_INPUT | MICROPIXEL_GPIO_CAP_EDGE_EVENTS;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t Open(micropixel_device_id_t device, uint16_t mode, uint16_t, uint16_t edge, uint32_t,
                               uint32_t, micropixel::device::GpioEdgeSink, void*) override {
        if (device != kGpio || mode != MICROPIXEL_GPIO_MODE_INPUT) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        if (events_suspended && edge != MICROPIXEL_GPIO_EDGE_NONE) {
            return MICROPIXEL_STATUS_CLOSED;
        }
        open = true;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t Read(micropixel_device_id_t device, bool& value) const override {
        if (device != kGpio || !open) {
            return MICROPIXEL_STATUS_CLOSED;
        }
        value = false;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t Write(micropixel_device_id_t, bool) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t SetPwmDuty(micropixel_device_id_t, uint16_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    void Close(micropixel_device_id_t device) override {
        if (device == kGpio) {
            open = false;
        }
    }

    void SuspendEvents() override { events_suspended = true; }

    [[nodiscard]] int32_t ResumeEvents() override {
        events_suspended = false;
        ++resume_count;
        return MICROPIXEL_STATUS_OK;
    }

    bool open{};
    bool events_suspended{};
    uint32_t resume_count{};
};

micropixel_gpio_open_request_t EdgeInputRequest() {
    micropixel_gpio_open_request_t request{};
    request.size = sizeof(request);
    request.mode = MICROPIXEL_GPIO_MODE_INPUT;
    request.device = kGpio;
    request.pull = MICROPIXEL_GPIO_PULL_DOWN;
    request.edge = MICROPIXEL_GPIO_EDGE_BOTH;
    return request;
}

void TestGpioService() {
    Gpio backend;
    micropixel::device::GpioService devices{backend};
    micropixel::runtime::EventQueue events;
    micropixel::runtime::TimerService clock;

    {
        micropixel::runtime::GpioService first{devices, events, clock};
        Require(first.valid());
        Require(first.Open(EdgeInputRequest()).has_value());
        first.Suspend();
        Require(backend.events_suspended);
    }

    Require(!backend.open);
    Require(!backend.events_suspended);
    Require(backend.resume_count == 1U);

    {
        micropixel::runtime::GpioService second{devices, events, clock};
        Require(second.Open(EdgeInputRequest()).has_value());
    }
}

}  // namespace

int main() {
    TestSensorService();
    TestGpioService();
    return 0;
}
