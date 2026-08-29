#include <cstdlib>

#include "runtime/services/gpio_service.hpp"
#include "runtime/services/timer_service.hpp"

namespace {

constexpr micropixel_device_id_t kGpio = 47U;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

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

}  // namespace

int main() {
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
    return 0;
}
