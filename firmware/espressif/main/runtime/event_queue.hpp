#ifndef MICROPIXEL_RUNTIME_EVENT_QUEUE_HPP
#define MICROPIXEL_RUNTIME_EVENT_QUEUE_HPP

#include <atomic>

#include "abi/micropixel_abi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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
        return queue_ != nullptr;
    }
    [[nodiscard]] EventWaitResult Wait(micropixel_event_t& event, uint64_t timeout_us);

    /* Required events apply backpressure and are never silently dropped. */
    [[nodiscard]] bool PushRequired(const micropixel_event_t& event);

    /* At most one event for each periodic Timer may wait in the queue. */
    [[nodiscard]] PeriodicPushResult PushPeriodicCoalesced(const micropixel_event_t& event);

    /* Move keeps only the newest sample per Touch ID. Other phases use
     * PushRequired() and retain strict queue ordering. */
    [[nodiscard]] TouchPushResult PushTouchMove(const micropixel_event_t& event);

    void Close();

   private:
    QueueHandle_t queue_{};
    std::atomic<bool> accepting_{true};
    std::atomic<uint32_t> periodic_pending_[limits::kMaxTimers]{};
    portMUX_TYPE touch_lock_ = portMUX_INITIALIZER_UNLOCKED;
    micropixel_event_t touch_latest_[MICROPIXEL_MAX_TOUCH_POINTS]{};
    uint32_t touch_pending_id_[MICROPIXEL_MAX_TOUCH_POINTS]{};
};

}  // namespace micropixel::runtime

#endif
