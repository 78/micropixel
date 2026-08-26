#ifndef MICROPIXEL_RUNTIME_EVENT_QUEUE_HPP
#define MICROPIXEL_RUNTIME_EVENT_QUEUE_HPP

#include <atomic>

#include "abi/micropixel_abi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "runtime/runtime_limits.hpp"

namespace micropixel::runtime {

enum class PeriodicPushResult {
    kEnqueued,
    kCoalesced,
    kFailed,
};

enum class TouchPushResult {
    kEnqueued,
    kCoalesced,
    kFailed,
};

enum class GpioPushResult {
    kEnqueued,
    kCoalesced,
    kFailed,
};

enum class EventWaitResult {
    kReceived,
    kTimeout,
    kClosed,
};

class EventQueue final {
   public:
    EventQueue();
    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;
    ~EventQueue();

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return queue_ != nullptr && pause_acknowledged_ != nullptr && resume_signal_ != nullptr;
    }
    [[nodiscard]] EventWaitResult Wait(micropixel_event_t& event, uint64_t timeout_us);
    [[nodiscard]] bool Suspend(TickType_t timeout);
    [[nodiscard]] bool PrepareResume(const micropixel_event_t& event);
    [[nodiscard]] bool RequestStop(const micropixel_event_t& event);
    void Resume();

    /* Required events apply backpressure and are never silently dropped. */
    [[nodiscard]] bool PushRequired(const micropixel_event_t& event);

    /* At most one event for each periodic Timer may wait in the queue. */
    [[nodiscard]] PeriodicPushResult PushPeriodicCoalesced(const micropixel_event_t& event);

    /* Move keeps only the newest sample per Touch ID. Other phases use
     * PushRequired() and retain strict queue ordering. */
    [[nodiscard]] TouchPushResult PushTouchMove(const micropixel_event_t& event);

    /* GPIO streams keep only the newest edge for each open GPIO handle. */
    [[nodiscard]] GpioPushResult PushGpioCoalesced(const micropixel_event_t& event);

    void Close();

   private:
    struct GpioEventSnapshot final {
        uint64_t timestamp_us{};
        uint32_t source{};
        uint32_t sequence{};
        micropixel_device_id_t device{};
        uint8_t value{};
        uint8_t edge{};
    };

    [[nodiscard]] bool PushControl(TickType_t timeout);
    [[nodiscard]] bool TakeStop(micropixel_event_t& event);
    [[nodiscard]] static bool IsControl(const micropixel_event_t& event);

    QueueHandle_t queue_{};
    SemaphoreHandle_t pause_acknowledged_{};
    SemaphoreHandle_t resume_signal_{};
    std::atomic<bool> accepting_{true};
    std::atomic<bool> pause_requested_{};
    std::atomic<bool> stop_requested_{};
    portMUX_TYPE periodic_lock_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t periodic_pending_[limits::kMaxTimers]{};
    micropixel_event_t periodic_latest_[limits::kMaxTimers]{};
    micropixel_event_t periodic_carry_[limits::kMaxTimers]{};
    bool periodic_carry_valid_[limits::kMaxTimers]{};
    portMUX_TYPE touch_lock_ = portMUX_INITIALIZER_UNLOCKED;
    micropixel_event_t touch_latest_[MICROPIXEL_MAX_TOUCH_POINTS]{};
    micropixel_event_t stop_event_{};
    uint32_t touch_pending_id_[MICROPIXEL_MAX_TOUCH_POINTS]{};
    portMUX_TYPE gpio_lock_ = portMUX_INITIALIZER_UNLOCKED;
    GpioEventSnapshot gpio_latest_[limits::kMaxGpioHandles]{};
};

}  // namespace micropixel::runtime

#endif
