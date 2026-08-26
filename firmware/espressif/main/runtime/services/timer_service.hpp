#ifndef MICROPIXEL_RUNTIME_SERVICES_TIMER_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_TIMER_SERVICE_HPP

#include <atomic>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "runtime/event_queue.hpp"
#include "runtime/runtime_limits.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class TimerService final {
   public:
    TimerService(EventQueue& events, int64_t clock_origin_us);
    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;
    ~TimerService();

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return mutex_ != nullptr;
    }
    [[nodiscard]] micropixel_app_time_t Now() const;
    [[nodiscard]] micropixel_app_time_t FromGlobalTime(uint64_t timestamp_us) const;

    [[nodiscard]] ServiceResult<micropixel_timer_handle_t> Create();
    [[nodiscard]] ServiceResult<void> Start(micropixel_timer_handle_t handle, uint64_t initial_delay_us,
                                            uint64_t period_us);
    [[nodiscard]] ServiceResult<void> Cancel(micropixel_timer_handle_t handle);
    [[nodiscard]] ServiceResult<void> Release(micropixel_timer_handle_t handle);
    [[nodiscard]] bool Suspend();
    [[nodiscard]] bool Resume();

   private:
    struct Slot {
        TimerService* owner{};
        esp_timer_handle_t native{};
        uint32_t index{};
        uint32_t generation{};
        micropixel_timer_handle_t handle{};
        uint64_t last_event_us{};
        uint64_t period_us{};
        uint32_t sequence{};
        uint32_t coalesced{};
        uint32_t dropped{};
        uint64_t resume_delay_us{};
        bool active{};
        bool periodic{};
        bool resume_active{};
    };

    static void OnExpired(void* argument);
    [[nodiscard]] Slot* FindSlot(micropixel_timer_handle_t handle);
    [[nodiscard]] bool TakeLock();
    void GiveLock();
    void ReleaseSlot(Slot& slot);

    EventQueue& events_;
    int64_t clock_origin_us_{};
    std::atomic<int64_t> suspended_at_us_{};
    std::atomic<int64_t> suspended_total_us_{};
    std::atomic<bool> suspended_{};
    SemaphoreHandle_t mutex_{};
    portMUX_TYPE state_lock_ = portMUX_INITIALIZER_UNLOCKED;
    Slot slots_[limits::kMaxTimers]{};
    uint32_t created_{};
    uint32_t released_{};
    uint32_t live_{};
    uint32_t high_water_{};
    uint32_t coalesced_total_{};
    uint32_t dropped_total_{};
    uint64_t timing_samples_{};
    uint64_t jitter_total_us_{};
    uint64_t jitter_min_us_{UINT64_MAX};
    uint64_t jitter_max_us_{};
};

}  // namespace micropixel::runtime

#endif
