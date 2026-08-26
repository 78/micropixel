#ifndef MICROPIXEL_RUNTIME_SERVICES_HAPTICS_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_HAPTICS_SERVICE_HPP

#include "device/device_services.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "runtime/event_queue.hpp"
#include "runtime/runtime_limits.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class TimerService;

class HapticsService final {
   public:
    HapticsService(device::HapticsService& haptics, EventQueue& events, TimerService& clock);
    HapticsService(const HapticsService&) = delete;
    HapticsService& operator=(const HapticsService&) = delete;
    ~HapticsService();

    [[nodiscard]] bool valid() const { return mutex_ != nullptr; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] ServiceResult<micropixel_handle_response_t> Open(micropixel_device_id_t device);
    [[nodiscard]] ServiceResult<void> Play(const micropixel_haptics_play_request_t& request);
    [[nodiscard]] ServiceResult<void> Stop(micropixel_haptic_handle_t haptic);
    [[nodiscard]] ServiceResult<void> Release(micropixel_haptic_handle_t haptic);
    void Suspend();
    void Shutdown();

   private:
    struct Slot final {
        micropixel_device_id_t device{};
        micropixel_haptic_handle_t handle{};
        uint32_t generation{};
        uint32_t sequence{};
        bool playing{};
    };

    static void OnFinished(void* context, micropixel_device_id_t device, uint64_t timestamp_us);
    void HandleFinished(micropixel_device_id_t device);
    [[nodiscard]] Slot* Find(micropixel_haptic_handle_t handle);
    [[nodiscard]] bool TakeLock();
    void GiveLock();

    device::HapticsService& haptics_;
    EventQueue& events_;
    TimerService& clock_;
    SemaphoreHandle_t mutex_{};
    Slot slots_[limits::kMaxHapticHandles]{};
    bool shut_down_{};
};

}  // namespace micropixel::runtime

#endif
