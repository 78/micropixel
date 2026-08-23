#include "runtime/event_queue.hpp"

#include <cinttypes>
#include <cstring>
#include <limits>

#include "esp_log.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_events";

TickType_t TimeoutTicks(uint64_t timeout_us) {
    if (timeout_us == std::numeric_limits<uint64_t>::max()) {
        return portMAX_DELAY;
    }
    if (timeout_us == 0U) {
        return 0U;
    }

    constexpr uint64_t kMicrosPerSecond = 1000000U;
    const uint64_t seconds = timeout_us / kMicrosPerSecond;
    const uint64_t remainder = timeout_us % kMicrosPerSecond;
    const uint64_t max_finite_ticks = static_cast<uint64_t>(portMAX_DELAY) - 1U;
    if (seconds > max_finite_ticks / configTICK_RATE_HZ) {
        return static_cast<TickType_t>(max_finite_ticks);
    }
    uint64_t ticks = seconds * configTICK_RATE_HZ;
    ticks += (remainder * configTICK_RATE_HZ + kMicrosPerSecond - 1U) / kMicrosPerSecond;
    if (ticks == 0U) {
        ticks = 1U;
    }
    return static_cast<TickType_t>(ticks > max_finite_ticks ? max_finite_ticks : ticks);
}

bool IsTouchMove(const micropixel_event_t& event) {
    if (event.service_id != MICROPIXEL_SERVICE_INPUT || event.event_id != MICROPIXEL_INPUT_EVENT_TOUCH) {
        return false;
    }
    micropixel_touch_event_payload_t payload{};
    std::memcpy(&payload, event.payload, sizeof(payload));
    return payload.phase == MICROPIXEL_TOUCH_MOVE;
}

}  // namespace

EventQueue::EventQueue() : queue_(xQueueCreate(limits::kEventQueueCapacity, sizeof(micropixel_event_t))) {}

EventQueue::~EventQueue() {
    Close();
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
    }
}

EventWaitResult EventQueue::Wait(micropixel_event_t& event, uint64_t timeout_us) {
    if (queue_ == nullptr || !accepting_.load(std::memory_order_acquire)) {
        return EventWaitResult::kClosed;
    }

    ESP_LOGD(kTag, "Guest blocked: event queue empty or awaiting next event");
    if (xQueueReceive(queue_, &event, TimeoutTicks(timeout_us)) != pdTRUE) {
        return accepting_.load(std::memory_order_acquire) ? EventWaitResult::kTimeout : EventWaitResult::kClosed;
    }
    if (event.service_id == MICROPIXEL_SERVICE_TIMER && event.event_id == MICROPIXEL_TIMER_EVENT_EXPIRED) {
        uint32_t encoded_index = event.source & 0xffU;
        if (encoded_index > 0U && encoded_index <= limits::kMaxTimers) {
            uint32_t expected = event.source;
            (void)periodic_pending_[encoded_index - 1U].compare_exchange_strong(expected, 0U,
                                                                                std::memory_order_acq_rel);
        }
    } else if (IsTouchMove(event)) {
        portENTER_CRITICAL(&touch_lock_);
        for (uint32_t index = 0U; index < MICROPIXEL_MAX_TOUCH_POINTS; ++index) {
            if (touch_pending_id_[index] == event.source + 1U) {
                event = touch_latest_[index];
                touch_pending_id_[index] = 0U;
                break;
            }
        }
        portEXIT_CRITICAL(&touch_lock_);
    }
    ESP_LOGD(kTag, "Guest woke: service=%" PRIu32 " event=%u sequence=%" PRIu32, event.service_id,
             static_cast<unsigned>(event.event_id), event.sequence);
    return EventWaitResult::kReceived;
}

bool EventQueue::PushRequired(const micropixel_event_t& event) {
    if (queue_ == nullptr) {
        return false;
    }
    while (accepting_.load(std::memory_order_acquire)) {
        if (xQueueSend(queue_, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
            return true;
        }
    }
    return false;
}

PeriodicPushResult EventQueue::PushPeriodicCoalesced(const micropixel_event_t& event) {
    uint32_t encoded_index = event.source & 0xffU;
    if (queue_ == nullptr || !accepting_.load(std::memory_order_acquire) || encoded_index == 0U ||
        encoded_index > limits::kMaxTimers) {
        return PeriodicPushResult::kFailed;
    }

    auto& pending = periodic_pending_[encoded_index - 1U];
    uint32_t expected = 0U;
    if (!pending.compare_exchange_strong(expected, event.source, std::memory_order_acq_rel)) {
        return PeriodicPushResult::kCoalesced;
    }

    if (xQueueSend(queue_, &event, 0) != pdTRUE) {
        expected = event.source;
        (void)pending.compare_exchange_strong(expected, 0U, std::memory_order_acq_rel);
        return PeriodicPushResult::kFailed;
    }
    return PeriodicPushResult::kEnqueued;
}

TouchPushResult EventQueue::PushTouchMove(const micropixel_event_t& event) {
    if (queue_ == nullptr || !accepting_.load(std::memory_order_acquire) || !IsTouchMove(event)) {
        return TouchPushResult::kFailed;
    }

    int32_t free_slot = -1;
    portENTER_CRITICAL(&touch_lock_);
    for (uint32_t index = 0U; index < MICROPIXEL_MAX_TOUCH_POINTS; ++index) {
        if (touch_pending_id_[index] == event.source + 1U) {
            touch_latest_[index] = event;
            portEXIT_CRITICAL(&touch_lock_);
            return TouchPushResult::kCoalesced;
        }
        if (touch_pending_id_[index] == 0U && free_slot < 0) {
            free_slot = static_cast<int32_t>(index);
        }
    }
    if (free_slot < 0) {
        portEXIT_CRITICAL(&touch_lock_);
        return TouchPushResult::kFailed;
    }
    uint32_t slot = static_cast<uint32_t>(free_slot);
    touch_latest_[slot] = event;
    touch_pending_id_[slot] = event.source + 1U;
    portEXIT_CRITICAL(&touch_lock_);

    if (xQueueSend(queue_, &event, 0) == pdTRUE) {
        return TouchPushResult::kEnqueued;
    }

    portENTER_CRITICAL(&touch_lock_);
    if (touch_pending_id_[slot] == event.source + 1U) {
        touch_pending_id_[slot] = 0U;
    }
    portEXIT_CRITICAL(&touch_lock_);
    return TouchPushResult::kFailed;
}

void EventQueue::Close() { accepting_.store(false, std::memory_order_release); }

}  // namespace micropixel::runtime
