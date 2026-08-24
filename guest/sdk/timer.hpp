#ifndef MICROPIXEL_SDK_TIMER_HPP
#define MICROPIXEL_SDK_TIMER_HPP

#include <stdint.h>

#include "sdk/event.hpp"
#include "sdk/types.hpp"

namespace micropixel {

class Application;

// Move-only resource proxy. Each live Timer owns one Host Timer handle.
class Timer final {
   public:
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    constexpr Timer(Timer&& other) noexcept : handle_(other.handle_) { other.handle_ = 0U; }

    Timer& operator=(Timer&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = 0U;
        }
        return *this;
    }

    ~Timer();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    void Cancel();
    /* Best-effort cancel + release. Safe to call repeatedly and from the destructor. */
    void Reset();

   private:
    explicit constexpr Timer(uint32_t handle) : handle_(handle) {}

    [[nodiscard]] constexpr bool Matches(const TimerEvent& event) const {
        return handle_ != 0U && event.source_ == handle_;
    }

    uint32_t handle_{};
    friend class Event;
    friend class Timers;
};

// Lightweight view of the Timer service. Copies are equivalent; Timer objects
// created through this view carry the resource identity and ownership.
class Timers final {
   public:
    constexpr Timers(const Timers&) noexcept = default;
    constexpr Timers& operator=(const Timers&) noexcept = default;

    [[nodiscard]] Timer After(Duration delay) const;
    [[nodiscard]] Timer Every(Duration period) const;

   private:
    struct CapabilityToken {};
    explicit constexpr Timers(CapabilityToken) noexcept {}
    friend class Application;
};

inline const TimerEvent* Event::TimerFrom(const Timer& source) const {
    const TimerEvent* candidate = timer();
    return candidate != nullptr && source.Matches(*candidate) ? candidate : nullptr;
}

}  // namespace micropixel

#endif
