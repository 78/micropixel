#ifndef MICROPIXEL_RUNTIME_KEY_EVENT_BRIDGE_HPP
#define MICROPIXEL_RUNTIME_KEY_EVENT_BRIDGE_HPP

#include <atomic>
#include <cstdint>

#include "device/contracts/input.hpp"

namespace micropixel::device {
class InputService;
}

namespace micropixel::runtime {

class EventQueue;

// Converts logical key samples from the Host Input Router into required Guest
// events. Unlike Touch Move, key transitions are never coalesced.
class KeyEventBridge final {
   public:
    KeyEventBridge(EventQueue& events, device::InputService& input, int64_t clock_origin_us);
    KeyEventBridge(const KeyEventBridge&) = delete;
    KeyEventBridge& operator=(const KeyEventBridge&) = delete;
    ~KeyEventBridge();

    [[nodiscard]] bool Push(const device::KeySample& sample);
    void Suspend();
    void Resume();
    void Shutdown();

   private:
    static bool Deliver(void* context, const device::KeySample& sample);

    EventQueue& events_;
    device::InputService& input_;
    int64_t clock_origin_us_{};
    std::atomic<uint32_t> sequence_{};
    std::atomic<uint32_t> delivered_{};
    std::atomic<uint32_t> dropped_{};
    bool bound_{true};
};

}  // namespace micropixel::runtime

#endif
