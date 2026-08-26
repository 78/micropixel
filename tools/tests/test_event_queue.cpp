#include <cstdlib>
#include <cstring>

#include "runtime/event_queue.hpp"

namespace {

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

micropixel_event_t GpioEvent(uint32_t source, uint32_t sequence, uint32_t value, uint32_t edge) {
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_GPIO_EVENT_EDGE;
    event.service_id = MICROPIXEL_SERVICE_GPIO;
    event.source = source;
    event.timestamp_us = 1000U + sequence;
    event.sequence = sequence;
    event.status = MICROPIXEL_STATUS_OK;
    const micropixel_gpio_event_payload_t payload{0x30005U, value, edge, 0U};
    std::memcpy(event.payload, &payload, sizeof(payload));
    return event;
}

}  // namespace

int main() {
    using micropixel::runtime::EventQueue;
    using micropixel::runtime::EventWaitResult;
    using micropixel::runtime::GpioPushResult;

    EventQueue events;
    Require(events.valid());
    const micropixel_event_t first = GpioEvent(0x101U, 1U, 0U, MICROPIXEL_GPIO_EDGE_FALLING);
    const micropixel_event_t latest = GpioEvent(0x101U, 2U, 1U, MICROPIXEL_GPIO_EDGE_RISING);
    Require(events.PushGpioCoalesced(first) == GpioPushResult::kEnqueued);
    Require(events.PushGpioCoalesced(latest) == GpioPushResult::kCoalesced);

    micropixel_event_t received{};
    Require(events.Wait(received, 0U) == EventWaitResult::kReceived);
    micropixel_gpio_event_payload_t payload{};
    std::memcpy(&payload, received.payload, sizeof(payload));
    Require(received.source == latest.source && received.sequence == latest.sequence &&
            received.timestamp_us == latest.timestamp_us);
    Require(payload.device == 0x30005U && payload.value == 1U && payload.edge == MICROPIXEL_GPIO_EDGE_RISING);

    micropixel_event_t invalid = first;
    invalid.flags = 1U;
    Require(events.PushGpioCoalesced(invalid) == GpioPushResult::kFailed);
    return 0;
}
