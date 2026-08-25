#include "runtime/key_event_bridge.hpp"

#include <cinttypes>
#include <cstring>

#include "abi/micropixel_abi.h"
#include "device/device_services.hpp"
#include "esp_log.h"
#include "runtime/event_queue.hpp"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_key_events";

}  // namespace

KeyEventBridge::KeyEventBridge(EventQueue& events, device::InputService& input, int64_t clock_origin_us)
    : events_(events), input_(input), clock_origin_us_(clock_origin_us) {
    input_.BindKeySink(Deliver, this);
}

KeyEventBridge::~KeyEventBridge() { Shutdown(); }

bool KeyEventBridge::Deliver(void* context, const device::KeySample& sample) {
    return context != nullptr && static_cast<KeyEventBridge*>(context)->Push(sample);
}

bool KeyEventBridge::Push(const device::KeySample& sample) {
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_INPUT_EVENT_KEY;
    event.service_id = MICROPIXEL_SERVICE_INPUT;
    event.status = MICROPIXEL_STATUS_OK;
    event.source = static_cast<uint32_t>(sample.code);
    const uint64_t origin = static_cast<uint64_t>(clock_origin_us_);
    event.timestamp_us = sample.timestamp_us >= origin ? sample.timestamp_us - origin : 0U;
    event.sequence = sequence_.fetch_add(1U, std::memory_order_relaxed) + 1U;

    micropixel_key_event_payload_t payload{};
    payload.code = static_cast<uint16_t>(sample.code);
    payload.repeat_count = sample.repeat_count;
    switch (sample.phase) {
        case device::KeyPhase::kDown:
            payload.phase = MICROPIXEL_KEY_DOWN_PHASE;
            break;
        case device::KeyPhase::kUp:
            payload.phase = MICROPIXEL_KEY_UP_PHASE;
            break;
        case device::KeyPhase::kRepeat:
            payload.phase = MICROPIXEL_KEY_REPEAT_PHASE;
            break;
        case device::KeyPhase::kCancel:
            payload.phase = MICROPIXEL_KEY_CANCEL_PHASE;
            break;
    }
    std::memcpy(event.payload, &payload, sizeof(payload));
    if (!events_.PushRequired(event)) {
        dropped_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    delivered_.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void KeyEventBridge::Suspend() {
    if (!bound_) {
        return;
    }
    input_.UnbindKeySink(this);
    bound_ = false;
}

void KeyEventBridge::Resume() {
    if (bound_) {
        return;
    }
    input_.BindKeySink(Deliver, this);
    bound_ = true;
}

void KeyEventBridge::Shutdown() {
    if (!bound_) {
        return;
    }
    input_.UnbindKeySink(this);
    bound_ = false;
    ESP_LOGI(kTag, "key events: delivered=%" PRIu32 " dropped=%" PRIu32, delivered_.load(std::memory_order_relaxed),
             dropped_.load(std::memory_order_relaxed));
}

}  // namespace micropixel::runtime
