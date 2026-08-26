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

bool IsGpioEdge(const micropixel_event_t& event) {
    return event.service_id == MICROPIXEL_SERVICE_GPIO && event.event_id == MICROPIXEL_GPIO_EVENT_EDGE;
}

bool DecodeGpioEdge(const micropixel_event_t& event, micropixel_gpio_event_payload_t& payload_out) {
    if (!IsGpioEdge(event) || event.size != sizeof(event) || event.flags != 0U ||
        event.status != MICROPIXEL_STATUS_OK) {
        return false;
    }
    std::memcpy(&payload_out, event.payload, sizeof(payload_out));
    return payload_out.value <= 1U &&
           (payload_out.edge == MICROPIXEL_GPIO_EDGE_RISING || payload_out.edge == MICROPIXEL_GPIO_EDGE_FALLING) &&
           payload_out.reserved0 == 0U;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) { return right > UINT64_MAX - left ? UINT64_MAX : left + right; }

uint32_t SaturatingAdd(uint32_t left, uint32_t right) { return right > UINT32_MAX - left ? UINT32_MAX : left + right; }

void MergeTimerEvent(micropixel_event_t& aggregate, const micropixel_event_t& incoming, uint32_t additionally_missed) {
    micropixel_timer_event_payload_t accumulated_payload{};
    micropixel_timer_event_payload_t incoming_payload{};
    std::memcpy(&accumulated_payload, aggregate.payload, sizeof(accumulated_payload));
    std::memcpy(&incoming_payload, incoming.payload, sizeof(incoming_payload));
    incoming_payload.elapsed_us = SaturatingAdd(accumulated_payload.elapsed_us, incoming_payload.elapsed_us);
    incoming_payload.missed_count = SaturatingAdd(
        SaturatingAdd(accumulated_payload.missed_count, incoming_payload.missed_count), additionally_missed);
    aggregate = incoming;
    std::memcpy(aggregate.payload, &incoming_payload, sizeof(incoming_payload));
}

void CountTimerEventAsMissed(micropixel_event_t& event) {
    micropixel_timer_event_payload_t payload{};
    std::memcpy(&payload, event.payload, sizeof(payload));
    payload.missed_count = SaturatingAdd(payload.missed_count, 1U);
    std::memcpy(event.payload, &payload, sizeof(payload));
}

}  // namespace

EventQueue::EventQueue()
    : queue_(xQueueCreate(limits::kEventQueueCapacity, sizeof(micropixel_event_t))),
      pause_acknowledged_(xSemaphoreCreateBinary()),
      resume_signal_(xSemaphoreCreateBinary()) {}

EventQueue::~EventQueue() {
    Close();
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
    }
    if (pause_acknowledged_ != nullptr) {
        vSemaphoreDelete(pause_acknowledged_);
    }
    if (resume_signal_ != nullptr) {
        vSemaphoreDelete(resume_signal_);
    }
}

EventWaitResult EventQueue::Wait(micropixel_event_t& event, uint64_t timeout_us) {
    if (queue_ == nullptr || !accepting_.load(std::memory_order_acquire)) {
        return EventWaitResult::kClosed;
    }

    for (;;) {
        if (TakeStop(event)) {
            break;
        }
        ESP_LOGD(kTag, "Guest blocked: event queue empty or awaiting next event");
        if (xQueueReceive(queue_, &event, TimeoutTicks(timeout_us)) != pdTRUE) {
            return accepting_.load(std::memory_order_acquire) ? EventWaitResult::kTimeout : EventWaitResult::kClosed;
        }
        if (!IsControl(event)) {
            break;
        }
        if (!accepting_.load(std::memory_order_acquire)) {
            return EventWaitResult::kClosed;
        }
        if (!pause_requested_.load(std::memory_order_acquire)) {
            continue;
        }
        (void)xSemaphoreGive(pause_acknowledged_);
        (void)xSemaphoreTake(resume_signal_, portMAX_DELAY);
        if (!accepting_.load(std::memory_order_acquire)) {
            return EventWaitResult::kClosed;
        }
    }
    if (event.service_id == MICROPIXEL_SERVICE_TIMER && event.event_id == MICROPIXEL_TIMER_EVENT_EXPIRED) {
        uint32_t encoded_index = event.source & 0xffU;
        if (encoded_index > 0U && encoded_index <= limits::kMaxTimers) {
            const uint32_t slot = encoded_index - 1U;
            portENTER_CRITICAL(&periodic_lock_);
            if (periodic_pending_[slot] == event.source) {
                event = periodic_latest_[slot];
                periodic_pending_[slot] = 0U;
                periodic_latest_[slot] = {};
            }
            portEXIT_CRITICAL(&periodic_lock_);
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
    } else if (IsGpioEdge(event)) {
        const uint32_t encoded_index = event.source & 0xffU;
        if (encoded_index > 0U && encoded_index <= limits::kMaxGpioHandles) {
            const uint32_t slot = encoded_index - 1U;
            portENTER_CRITICAL(&gpio_lock_);
            const GpioEventSnapshot snapshot = gpio_latest_[slot];
            if (snapshot.source == event.source) {
                micropixel_gpio_event_payload_t payload{};
                payload.device = snapshot.device;
                payload.value = snapshot.value;
                payload.edge = snapshot.edge;
                event.timestamp_us = snapshot.timestamp_us;
                event.sequence = snapshot.sequence;
                std::memcpy(event.payload, &payload, sizeof(payload));
                gpio_latest_[slot] = {};
            }
            portEXIT_CRITICAL(&gpio_lock_);
        }
    }
    ESP_LOGD(kTag, "Guest woke: service=%" PRIu32 " event=%u sequence=%" PRIu32, event.service_id,
             static_cast<unsigned>(event.event_id), event.sequence);
    return EventWaitResult::kReceived;
}

bool EventQueue::Suspend(TickType_t timeout) {
    if (!valid() || !accepting_.load(std::memory_order_acquire)) {
        return false;
    }
    while (xSemaphoreTake(pause_acknowledged_, 0U) == pdTRUE) {
    }
    while (xSemaphoreTake(resume_signal_, 0U) == pdTRUE) {
    }
    pause_requested_.store(true, std::memory_order_release);
    if (!PushControl(timeout) || xSemaphoreTake(pause_acknowledged_, timeout) != pdTRUE) {
        Resume();
        return false;
    }
    return true;
}

bool EventQueue::PrepareResume(const micropixel_event_t& event) {
    if (queue_ == nullptr || !accepting_.load(std::memory_order_acquire) ||
        !pause_requested_.load(std::memory_order_acquire)) {
        return false;
    }
    // Suspend's control record has already been consumed at the acknowledged
    // WaitEvent safe point, leaving room for this required event. Put it at
    // the front so Guest code observes Resume before any queued Timer/Input
    // work that survived the transition.
    return xQueueSendToFront(queue_, &event, 0U) == pdTRUE;
}

bool EventQueue::RequestStop(const micropixel_event_t& event) {
    if (queue_ == nullptr || !accepting_.load(std::memory_order_acquire)) {
        return false;
    }
    stop_event_ = event;
    stop_requested_.store(true, std::memory_order_release);
    pause_requested_.store(false, std::memory_order_release);
    // Wake either xQueueReceive or the suspended WaitEvent safe point. A full
    // queue does not lose Stop: TakeStop observes the atomic request before
    // the Guest receives its next queued event.
    (void)PushControl(0U);
    if (resume_signal_ != nullptr) {
        (void)xSemaphoreGive(resume_signal_);
    }
    return true;
}

void EventQueue::Resume() {
    pause_requested_.store(false, std::memory_order_release);
    if (resume_signal_ != nullptr) {
        (void)xSemaphoreGive(resume_signal_);
    }
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

    const uint32_t slot = encoded_index - 1U;
    portENTER_CRITICAL(&periodic_lock_);
    if (periodic_pending_[slot] == event.source) {
        MergeTimerEvent(periodic_latest_[slot], event, 1U);
        portEXIT_CRITICAL(&periodic_lock_);
        return PeriodicPushResult::kCoalesced;
    }
    if (periodic_pending_[slot] != 0U) {
        portEXIT_CRITICAL(&periodic_lock_);
        return xQueueSend(queue_, &event, 0U) == pdTRUE ? PeriodicPushResult::kEnqueued : PeriodicPushResult::kFailed;
    }

    periodic_latest_[slot] = event;
    if (periodic_carry_valid_[slot]) {
        if (periodic_carry_[slot].source == event.source) {
            micropixel_event_t current = periodic_latest_[slot];
            periodic_latest_[slot] = periodic_carry_[slot];
            MergeTimerEvent(periodic_latest_[slot], current, 0U);
        }
        periodic_carry_[slot] = {};
        periodic_carry_valid_[slot] = false;
    }
    periodic_pending_[slot] = event.source;
    portEXIT_CRITICAL(&periodic_lock_);

    if (xQueueSend(queue_, &event, 0) != pdTRUE) {
        portENTER_CRITICAL(&periodic_lock_);
        if (periodic_pending_[slot] == event.source) {
            periodic_carry_[slot] = periodic_latest_[slot];
            CountTimerEventAsMissed(periodic_carry_[slot]);
            periodic_carry_valid_[slot] = true;
            periodic_pending_[slot] = 0U;
            periodic_latest_[slot] = {};
        }
        portEXIT_CRITICAL(&periodic_lock_);
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

GpioPushResult EventQueue::PushGpioCoalesced(const micropixel_event_t& event) {
    const uint32_t encoded_index = event.source & 0xffU;
    micropixel_gpio_event_payload_t payload{};
    if (queue_ == nullptr || !accepting_.load(std::memory_order_acquire) || !DecodeGpioEdge(event, payload) ||
        encoded_index == 0U || encoded_index > limits::kMaxGpioHandles) {
        return GpioPushResult::kFailed;
    }
    const uint32_t slot = encoded_index - 1U;
    portENTER_CRITICAL(&gpio_lock_);
    if (gpio_latest_[slot].source == event.source) {
        gpio_latest_[slot] = GpioEventSnapshot{event.timestamp_us,
                                               event.source,
                                               event.sequence,
                                               payload.device,
                                               static_cast<uint8_t>(payload.value),
                                               static_cast<uint8_t>(payload.edge)};
        portEXIT_CRITICAL(&gpio_lock_);
        return GpioPushResult::kCoalesced;
    }
    if (gpio_latest_[slot].source != 0U) {
        portEXIT_CRITICAL(&gpio_lock_);
        return GpioPushResult::kFailed;
    }
    gpio_latest_[slot] = GpioEventSnapshot{event.timestamp_us,
                                           event.source,
                                           event.sequence,
                                           payload.device,
                                           static_cast<uint8_t>(payload.value),
                                           static_cast<uint8_t>(payload.edge)};
    portEXIT_CRITICAL(&gpio_lock_);

    if (xQueueSend(queue_, &event, 0U) == pdTRUE) {
        return GpioPushResult::kEnqueued;
    }
    portENTER_CRITICAL(&gpio_lock_);
    if (gpio_latest_[slot].source == event.source) {
        gpio_latest_[slot] = {};
    }
    portEXIT_CRITICAL(&gpio_lock_);
    return GpioPushResult::kFailed;
}

bool EventQueue::PushControl(TickType_t timeout) {
    if (queue_ == nullptr) {
        return false;
    }
    micropixel_event_t control{};
    return xQueueSendToFront(queue_, &control, timeout) == pdTRUE;
}

bool EventQueue::TakeStop(micropixel_event_t& event) {
    if (!stop_requested_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    event = stop_event_;
    return true;
}

bool EventQueue::IsControl(const micropixel_event_t& event) {
    return event.size == 0U && event.service_id == 0U && event.event_id == 0U;
}

void EventQueue::Close() {
    accepting_.store(false, std::memory_order_release);
    pause_requested_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    (void)PushControl(0U);
    if (resume_signal_ != nullptr) {
        (void)xSemaphoreGive(resume_signal_);
    }
}

}  // namespace micropixel::runtime
