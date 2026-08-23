#ifndef MICROPIXEL_RUNTIME_TOUCH_EVENT_BRIDGE_HPP
#define MICROPIXEL_RUNTIME_TOUCH_EVENT_BRIDGE_HPP

#include <atomic>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/input.hpp"

namespace micropixel::device {
class InputService;
}

namespace micropixel::runtime {

class EventQueue;

// Converts platform touch samples into Guest events and owns the timing
// instrumentation that spans input delivery and the next graphics submit.
class TouchEventBridge final {
   public:
    TouchEventBridge(EventQueue& events, device::InputService& input, int64_t clock_origin_us);
    TouchEventBridge(const TouchEventBridge&) = delete;
    TouchEventBridge& operator=(const TouchEventBridge&) = delete;
    ~TouchEventBridge();

    [[nodiscard]] bool Push(const device::TouchSample& sample);
    void NoteDelivered(const micropixel_event_t& event);
    [[nodiscard]] uint64_t TakeSampleForGraphics();
    void NoteGraphicsSubmitComplete(uint64_t touch_sample_global_us);
    void Shutdown();

   private:
    static bool Deliver(void* context, const device::TouchSample& sample);

    EventQueue& events_;
    device::InputService& input_;
    int64_t clock_origin_us_{};
    std::atomic<uint32_t> sequence_{};
    std::atomic<uint32_t> delivered_{};
    std::atomic<uint32_t> coalesced_{};
    std::atomic<uint32_t> dropped_{};
    uint64_t last_sample_global_us_{};
    uint64_t to_guest_total_us_{};
    uint64_t to_screen_total_us_{};
    uint32_t received_{};
    uint32_t screen_updates_{};
    bool bound_{true};
};

}  // namespace micropixel::runtime

#endif
