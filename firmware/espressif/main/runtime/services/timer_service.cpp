#include "runtime/services/timer_service.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_timers";

}  // namespace

TimerService::TimerService(EventQueue& events, int64_t clock_origin_us)
    : events_(events), clock_origin_us_(clock_origin_us), mutex_(xSemaphoreCreateMutex()) {
    for (uint32_t index = 0; index < limits::kMaxTimers; ++index) {
        slots_[index].owner = this;
        slots_[index].index = index;
    }
}

TimerService::~TimerService() {
    if (TakeLock()) {
        for (auto& slot : slots_) {
            if (slot.native != nullptr) {
                ReleaseSlot(slot);
            }
        }
        GiveLock();
    }

    ESP_LOGI(kTag,
             "timer resources: created=%" PRIu32 " released=%" PRIu32 " live=%" PRIu32 " high-water=%" PRIu32
             " coalesced=%" PRIu32 " dropped=%" PRIu32,
             created_, released_, live_, high_water_, coalesced_total_, dropped_total_);
    if (timing_samples_ > 0U) {
        ESP_LOGI(kTag,
                 "periodic timer timing: samples=%" PRIu64 " avg-jitter=%" PRIu64 " us min-jitter=%" PRIu64
                 " us max-jitter=%" PRIu64 " us",
                 timing_samples_, jitter_total_us_ / timing_samples_, jitter_min_us_, jitter_max_us_);
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

micropixel_app_time_t TimerService::Now() const {
    const int64_t suspended_at = suspended_at_us_.load(std::memory_order_acquire);
    const int64_t now = suspended_.load(std::memory_order_acquire) ? suspended_at : esp_timer_get_time();
    int64_t elapsed = now - clock_origin_us_ - suspended_total_us_.load(std::memory_order_acquire);
    return elapsed > 0 ? static_cast<micropixel_app_time_t>(elapsed) : 0U;
}

bool TimerService::TakeLock() { return mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE; }

void TimerService::GiveLock() { (void)xSemaphoreGive(mutex_); }

TimerService::Slot* TimerService::FindSlot(micropixel_timer_handle_t handle) {
    uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > limits::kMaxTimers) {
        return nullptr;
    }
    Slot& slot = slots_[encoded_index - 1U];
    return slot.native != nullptr && slot.handle == handle ? &slot : nullptr;
}

ServiceResult<micropixel_timer_handle_t> TimerService::Create() {
    if (!TakeLock()) {
        return FailService<micropixel_timer_handle_t>(MICROPIXEL_STATUS_INTERNAL);
    }

    int32_t status = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    micropixel_timer_handle_t created_handle = 0U;
    for (auto& slot : slots_) {
        if (slot.native != nullptr) {
            continue;
        }

        esp_timer_create_args_t arguments{};
        arguments.callback = OnExpired;
        arguments.arg = &slot;
        arguments.dispatch_method = ESP_TIMER_TASK;
        arguments.name = "micropixel_guest";
        arguments.skip_unhandled_events = true;

        esp_timer_handle_t native = nullptr;
        if (esp_timer_create(&arguments, &native) != ESP_OK) {
            status = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
            break;
        }

        portENTER_CRITICAL(&state_lock_);
        ++slot.generation;
        if (slot.generation == 0U) {
            ++slot.generation;
        }
        slot.native = native;
        slot.handle = (slot.generation << 8U) | (slot.index + 1U);
        slot.last_event_us = 0U;
        slot.period_us = 0U;
        slot.sequence = 0U;
        slot.coalesced = 0U;
        slot.dropped = 0U;
        slot.resume_delay_us = 0U;
        slot.active = false;
        slot.periodic = false;
        slot.resume_active = false;
        created_handle = slot.handle;
        portEXIT_CRITICAL(&state_lock_);

        ++created_;
        ++live_;
        high_water_ = std::max(high_water_, live_);
        status = MICROPIXEL_STATUS_OK;
        break;
    }

    GiveLock();
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<micropixel_timer_handle_t>(status);
    }
    return created_handle;
}

ServiceResult<void> TimerService::Start(micropixel_timer_handle_t handle, uint64_t initial_delay_us,
                                        uint64_t period_us) {
    if (handle == 0U || initial_delay_us == 0U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (period_us != 0U && initial_delay_us != period_us) {
        return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }

    Slot* slot = FindSlot(handle);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    portENTER_CRITICAL(&state_lock_);
    slot->active = false;
    esp_timer_handle_t native = slot->native;
    portEXIT_CRITICAL(&state_lock_);
    esp_err_t stop_status = esp_timer_stop(native);
    if (stop_status != ESP_OK && stop_status != ESP_ERR_INVALID_STATE) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }

    portENTER_CRITICAL(&state_lock_);
    slot->periodic = period_us != 0U;
    slot->period_us = period_us;
    slot->last_event_us = Now();
    slot->sequence = 0U;
    slot->active = true;
    slot->resume_active = suspended_.load(std::memory_order_acquire);
    slot->resume_delay_us = initial_delay_us;
    portEXIT_CRITICAL(&state_lock_);

    if (suspended_.load(std::memory_order_acquire)) {
        GiveLock();
        return {};
    }
    esp_err_t start_status =
        period_us == 0U ? esp_timer_start_once(native, initial_delay_us) : esp_timer_start_periodic(native, period_us);
    if (start_status != ESP_OK) {
        portENTER_CRITICAL(&state_lock_);
        slot->active = false;
        portEXIT_CRITICAL(&state_lock_);
        GiveLock();
        return FailService<void>(start_status == ESP_ERR_INVALID_ARG ? MICROPIXEL_STATUS_INVALID_ARGUMENT
                                                                     : MICROPIXEL_STATUS_INTERNAL);
    }

    GiveLock();
    return {};
}

ServiceResult<void> TimerService::Cancel(micropixel_timer_handle_t handle) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = FindSlot(handle);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    portENTER_CRITICAL(&state_lock_);
    slot->active = false;
    slot->resume_active = false;
    esp_timer_handle_t native = slot->native;
    portEXIT_CRITICAL(&state_lock_);
    esp_err_t status = esp_timer_stop(native);
    GiveLock();
    return status == ESP_OK || status == ESP_ERR_INVALID_STATE ? ServiceResult<void>{}
                                                               : FailService<void>(MICROPIXEL_STATUS_INTERNAL);
}

void TimerService::ReleaseSlot(Slot& slot) {
    portENTER_CRITICAL(&state_lock_);
    slot.active = false;
    slot.resume_active = false;
    esp_timer_handle_t native = slot.native;
    portEXIT_CRITICAL(&state_lock_);

    esp_err_t stop_status = esp_timer_stop(native);
    if (stop_status != ESP_OK && stop_status != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "timer stop during release failed: %s", esp_err_to_name(stop_status));
    }
    esp_err_t delete_status = esp_timer_delete(native);
    if (delete_status != ESP_OK) {
        ESP_LOGW(kTag, "timer delete failed: %s", esp_err_to_name(delete_status));
    }

    portENTER_CRITICAL(&state_lock_);
    slot.native = nullptr;
    slot.handle = 0U;
    slot.active = false;
    slot.periodic = false;
    slot.period_us = 0U;
    slot.resume_delay_us = 0U;
    slot.resume_active = false;
    portEXIT_CRITICAL(&state_lock_);
    ++released_;
    --live_;
}

ServiceResult<void> TimerService::Release(micropixel_timer_handle_t handle) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = FindSlot(handle);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    ReleaseSlot(*slot);
    GiveLock();
    return {};
}

void TimerService::OnExpired(void* argument) {
    auto& slot = *static_cast<Slot*>(argument);
    TimerService& service = *slot.owner;
    uint64_t timestamp = service.Now();

    portENTER_CRITICAL(&service.state_lock_);
    if (!slot.active || slot.native == nullptr || service.suspended_.load(std::memory_order_acquire)) {
        portEXIT_CRITICAL(&service.state_lock_);
        return;
    }
    bool periodic = slot.periodic;
    micropixel_timer_handle_t handle = slot.handle;
    uint64_t delta = timestamp - slot.last_event_us;
    uint32_t sequence = slot.sequence + 1U;
    slot.last_event_us = timestamp;
    slot.sequence = sequence;
    if (!periodic) {
        slot.active = false;
    }
    portEXIT_CRITICAL(&service.state_lock_);

    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_TIMER_EVENT_EXPIRED;
    event.service_id = MICROPIXEL_SERVICE_TIMER;
    event.timestamp_us = timestamp;
    event.source = handle;
    event.sequence = sequence;
    event.status = MICROPIXEL_STATUS_OK;
    micropixel_timer_event_payload_t payload{};
    payload.elapsed_us = delta;
    std::memcpy(event.payload, &payload, sizeof(payload));

    PeriodicPushResult result = PeriodicPushResult::kEnqueued;
    bool delivered = false;
    if (periodic) {
        result = service.events_.PushPeriodicCoalesced(event);
        delivered = result == PeriodicPushResult::kEnqueued;
    } else {
        delivered = service.events_.PushRequired(event);
    }

    portENTER_CRITICAL(&service.state_lock_);
    if (slot.handle == handle) {
        if (periodic && slot.period_us > 0U) {
            uint64_t jitter = delta >= slot.period_us ? delta - slot.period_us : slot.period_us - delta;
            ++service.timing_samples_;
            service.jitter_total_us_ += jitter;
            service.jitter_min_us_ = std::min(service.jitter_min_us_, jitter);
            service.jitter_max_us_ = std::max(service.jitter_max_us_, jitter);
        }
        if (!delivered && result == PeriodicPushResult::kCoalesced) {
            ++slot.coalesced;
            ++service.coalesced_total_;
        } else if (!delivered) {
            ++slot.dropped;
            ++service.dropped_total_;
        }
    }
    portEXIT_CRITICAL(&service.state_lock_);
}

bool TimerService::Suspend() {
    if (!TakeLock()) {
        return false;
    }
    if (suspended_.load(std::memory_order_acquire)) {
        GiveLock();
        return true;
    }

    const int64_t now_global_us = esp_timer_get_time();
    suspended_at_us_.store(now_global_us, std::memory_order_release);
    suspended_.store(true, std::memory_order_release);
    const uint64_t now_app_us = Now();
    bool succeeded = true;
    for (auto& slot : slots_) {
        if (slot.native == nullptr || !slot.active) {
            continue;
        }
        uint64_t remaining_us = 1U;
        if (slot.periodic && slot.period_us > 0U) {
            const uint64_t elapsed_us = now_app_us >= slot.last_event_us ? now_app_us - slot.last_event_us : 0U;
            remaining_us = elapsed_us < slot.period_us ? slot.period_us - elapsed_us : 1U;
        } else {
            uint64_t expiry_us = 0U;
            if (esp_timer_get_expiry_time(slot.native, &expiry_us) == ESP_OK && expiry_us > (uint64_t)now_global_us) {
                remaining_us = expiry_us - (uint64_t)now_global_us;
            }
        }
        portENTER_CRITICAL(&state_lock_);
        slot.resume_active = true;
        slot.resume_delay_us = remaining_us;
        portEXIT_CRITICAL(&state_lock_);
        const esp_err_t status = esp_timer_stop(slot.native);
        if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
            succeeded = false;
            ESP_LOGW(kTag, "timer stop during suspend failed: %s", esp_err_to_name(status));
        }
    }
    GiveLock();
    return succeeded;
}

bool TimerService::Resume() {
    if (!TakeLock()) {
        return false;
    }
    if (!suspended_.load(std::memory_order_acquire)) {
        GiveLock();
        return true;
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t suspended_at = suspended_at_us_.load(std::memory_order_acquire);
    if (now_us > suspended_at) {
        suspended_total_us_.fetch_add(now_us - suspended_at, std::memory_order_acq_rel);
    }
    suspended_.store(false, std::memory_order_release);
    suspended_at_us_.store(0, std::memory_order_release);

    bool succeeded = true;
    for (auto& slot : slots_) {
        if (slot.native == nullptr || !slot.active || !slot.resume_active) {
            continue;
        }
        const uint64_t delay_us = slot.resume_delay_us > 0U ? slot.resume_delay_us : 1U;
        portENTER_CRITICAL(&state_lock_);
        slot.resume_active = false;
        slot.resume_delay_us = 0U;
        portEXIT_CRITICAL(&state_lock_);
        const esp_err_t status = slot.periodic ? esp_timer_start_periodic_at(slot.native, slot.period_us,
                                                                             static_cast<uint64_t>(now_us) + delay_us)
                                               : esp_timer_start_once(slot.native, delay_us);
        if (status != ESP_OK) {
            portENTER_CRITICAL(&state_lock_);
            slot.active = false;
            portEXIT_CRITICAL(&state_lock_);
            succeeded = false;
            ESP_LOGW(kTag, "timer restart after resume failed: %s", esp_err_to_name(status));
        }
    }
    GiveLock();
    return succeeded;
}

}  // namespace micropixel::runtime
