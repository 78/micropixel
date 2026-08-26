#include "runtime/services/haptics_service.hpp"

#include "runtime/services/timer_service.hpp"

namespace micropixel::runtime {

HapticsService::HapticsService(device::HapticsService& haptics, EventQueue& events, TimerService& clock)
    : haptics_(haptics), events_(events), clock_(clock), mutex_(xSemaphoreCreateMutex()) {
    haptics_.SetCompletionSink(OnFinished, this);
}

HapticsService::~HapticsService() {
    Shutdown();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

bool HapticsService::TakeLock() { return mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE; }

void HapticsService::GiveLock() { (void)xSemaphoreGive(mutex_); }

HapticsService::Slot* HapticsService::Find(micropixel_haptic_handle_t handle) {
    const uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > limits::kMaxHapticHandles) {
        return nullptr;
    }
    Slot& slot = slots_[encoded_index - 1U];
    return slot.handle == handle ? &slot : nullptr;
}

ServiceResult<micropixel_handle_response_t> HapticsService::Open(micropixel_device_id_t device) {
    auto info = haptics_.GetInfo(device);
    if (!info) {
        return FailService<micropixel_handle_response_t>(info.error().status);
    }
    if (!TakeLock()) {
        return FailService<micropixel_handle_response_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    for (const Slot& existing : slots_) {
        if (existing.handle != 0U && existing.device == device) {
            GiveLock();
            return FailService<micropixel_handle_response_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
    }
    micropixel_handle_response_t response{};
    for (uint32_t index = 0U; index < limits::kMaxHapticHandles; ++index) {
        Slot& slot = slots_[index];
        if (slot.handle != 0U) {
            continue;
        }
        ++slot.generation;
        if (slot.generation == 0U) {
            ++slot.generation;
        }
        slot.device = device;
        slot.handle = (slot.generation << 8U) | (index + 1U);
        response.size = sizeof(response);
        response.handle = slot.handle;
        break;
    }
    GiveLock();
    return response.handle != 0U ? ServiceResult<micropixel_handle_response_t>{response}
                                 : FailService<micropixel_handle_response_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
}

ServiceResult<void> HapticsService::Play(const micropixel_haptics_play_request_t& request) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(request.haptic);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    GiveLock();
    auto result = haptics_.Play(device, request.strength_per_mille, request.duration_ms);
    if (!result) {
        return FailService<void>(result.error().status);
    }
    if (TakeLock()) {
        slot = Find(request.haptic);
        if (slot != nullptr) {
            slot->playing = true;
        }
        GiveLock();
    }
    return {};
}

ServiceResult<void> HapticsService::Stop(micropixel_haptic_handle_t haptic) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(haptic);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    slot->playing = false;
    GiveLock();
    auto result = haptics_.Stop(device);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> HapticsService::Release(micropixel_haptic_handle_t haptic) {
    auto stopped = Stop(haptic);
    if (!stopped) {
        return stopped;
    }
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(haptic);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const uint32_t generation = slot->generation;
    *slot = {};
    slot->generation = generation;
    GiveLock();
    return {};
}

void HapticsService::Suspend() {
    if (!TakeLock()) {
        return;
    }
    for (Slot& slot : slots_) {
        if (slot.handle != 0U && slot.playing) {
            (void)haptics_.Stop(slot.device);
            slot.playing = false;
        }
    }
    GiveLock();
}

void HapticsService::Shutdown() {
    if (shut_down_) {
        return;
    }
    shut_down_ = true;
    haptics_.SetCompletionSink(nullptr, nullptr);
    if (!TakeLock()) {
        return;
    }
    for (Slot& slot : slots_) {
        if (slot.handle != 0U) {
            (void)haptics_.Stop(slot.device);
            const uint32_t generation = slot.generation;
            slot = {};
            slot.generation = generation;
        }
    }
    GiveLock();
}

void HapticsService::OnFinished(void* context, micropixel_device_id_t device, uint64_t timestamp_us) {
    (void)timestamp_us;
    if (context != nullptr) {
        static_cast<HapticsService*>(context)->HandleFinished(device);
    }
}

void HapticsService::HandleFinished(micropixel_device_id_t device) {
    if (!TakeLock()) {
        return;
    }
    micropixel_haptic_handle_t handle = 0U;
    uint32_t sequence = 0U;
    for (Slot& slot : slots_) {
        if (slot.handle != 0U && slot.device == device && slot.playing) {
            slot.playing = false;
            handle = slot.handle;
            sequence = ++slot.sequence;
            break;
        }
    }
    GiveLock();
    if (handle == 0U) {
        return;
    }
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_HAPTICS_EVENT_FINISHED;
    event.service_id = MICROPIXEL_SERVICE_HAPTICS;
    event.source = handle;
    event.timestamp_us = clock_.Now();
    event.sequence = sequence;
    event.status = MICROPIXEL_STATUS_OK;
    (void)events_.PushRequired(event);
}

}  // namespace micropixel::runtime
