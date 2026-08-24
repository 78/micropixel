#include "runtime/touch_event_bridge.hpp"

#include <cinttypes>
#include <cstring>

#include "device/device_services.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/event_queue.hpp"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_touch_events";

}  // namespace

TouchEventBridge::TouchEventBridge(EventQueue& events, device::InputService& input, int64_t clock_origin_us)
    : events_(events), input_(input), clock_origin_us_(clock_origin_us) {
    input_.BindTouchSink(Deliver, this);
}

TouchEventBridge::~TouchEventBridge() { Shutdown(); }

bool TouchEventBridge::Deliver(void* context, const device::TouchSample& sample) {
    return context != nullptr && static_cast<TouchEventBridge*>(context)->Push(sample);
}

bool TouchEventBridge::Push(const device::TouchSample& sample) {
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_INPUT_EVENT_TOUCH;
    event.service_id = MICROPIXEL_SERVICE_INPUT;
    event.status = MICROPIXEL_STATUS_OK;
    micropixel_touch_event_payload_t payload{};
    payload.x = sample.x;
    payload.y = sample.y;
    payload.pressure_per_mille = sample.pressure_per_mille;
    switch (sample.phase) {
        case device::TouchPhase::kDown:
            payload.phase = MICROPIXEL_TOUCH_DOWN;
            break;
        case device::TouchPhase::kMove:
            payload.phase = MICROPIXEL_TOUCH_MOVE;
            break;
        case device::TouchPhase::kUp:
            payload.phase = MICROPIXEL_TOUCH_UP;
            break;
        case device::TouchPhase::kCancel:
            payload.phase = MICROPIXEL_TOUCH_CANCEL;
            break;
    }
    uint64_t origin = static_cast<uint64_t>(clock_origin_us_);
    event.timestamp_us = sample.timestamp_us >= origin ? sample.timestamp_us - origin : 0U;
    event.source = sample.id;
    event.sequence = sequence_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    std::memcpy(event.payload, &payload, sizeof(payload));

    if (sample.phase == device::TouchPhase::kMove) {
        TouchPushResult result = events_.PushTouchMove(event);
        if (result == TouchPushResult::kCoalesced) {
            coalesced_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        if (result == TouchPushResult::kFailed) {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
    } else if (!events_.PushRequired(event)) {
        dropped_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    delivered_.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

void TouchEventBridge::NoteDelivered(const micropixel_event_t& event) {
    if (event.service_id != MICROPIXEL_SERVICE_INPUT || event.event_id != MICROPIXEL_INPUT_EVENT_TOUCH) {
        return;
    }
    uint64_t sample_global_us = static_cast<uint64_t>(clock_origin_us_) + event.timestamp_us;
    uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    uint64_t latency_us = now_us >= sample_global_us ? now_us - sample_global_us : 0U;
    last_sample_global_us_ = sample_global_us;
    to_guest_total_us_ += latency_us;
    ++received_;
    if (received_ == 1U || (received_ % 100U) == 0U) {
        ESP_LOGI(kTag, "touch delivery #%" PRIu32 ": sample-to-Guest=%" PRIu64 " us", received_, latency_us);
    }
}

uint64_t TouchEventBridge::TakeSampleForGraphics() {
    uint64_t sample_global_us = last_sample_global_us_;
    last_sample_global_us_ = 0U;
    return sample_global_us;
}

void TouchEventBridge::NoteGraphicsSubmitComplete(uint64_t touch_sample_global_us) {
    if (touch_sample_global_us == 0U) {
        return;
    }
    uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    uint64_t latency_us = now_us >= touch_sample_global_us ? now_us - touch_sample_global_us : 0U;
    to_screen_total_us_ += latency_us;
    ++screen_updates_;
    if (screen_updates_ == 1U || (screen_updates_ % 100U) == 0U) {
        ESP_LOGI(kTag, "touch submit #%" PRIu32 ": sample-to-submit=%" PRIu64 " us", screen_updates_, latency_us);
    }
}

void TouchEventBridge::Suspend() {
    if (!bound_) {
        return;
    }
    input_.UnbindTouchSink(this);
    bound_ = false;
}

void TouchEventBridge::Resume() {
    if (bound_) {
        return;
    }
    input_.BindTouchSink(Deliver, this);
    bound_ = true;
}

void TouchEventBridge::Shutdown() {
    if (!bound_) {
        return;
    }
    input_.UnbindTouchSink(this);
    bound_ = false;
    ESP_LOGI(kTag,
             "touch events: delivered=%" PRIu32 " coalesced=%" PRIu32 " dropped=%" PRIu32 " guest-avg=%" PRIu64
             " us submit-avg=%" PRIu64 " us",
             delivered_.load(std::memory_order_relaxed), coalesced_.load(std::memory_order_relaxed),
             dropped_.load(std::memory_order_relaxed), received_ > 0U ? to_guest_total_us_ / received_ : 0U,
             screen_updates_ > 0U ? to_screen_total_us_ / screen_updates_ : 0U);
}

}  // namespace micropixel::runtime
