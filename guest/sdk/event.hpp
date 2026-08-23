#ifndef MICROPIXEL_SDK_EVENT_HPP
#define MICROPIXEL_SDK_EVENT_HPP

#include <stdint.h>

#include "sdk/resources.hpp"
#include "sdk/types.hpp"

namespace micropixel {

class Application;
class Timer;

enum class EventType : uint16_t {
    kUnknown,
    kResume,
    kStop,
    kTimer,
    kTouch,
    kResourceReady,
};

enum class TouchPhase : uint8_t {
    kDown,
    kMove,
    kUp,
    kCancel,
};

class TouchEvent final {
   public:
    constexpr TouchEvent(const TouchEvent&) = default;
    constexpr TouchEvent& operator=(const TouchEvent&) = default;

    [[nodiscard]] constexpr TimePoint timestamp() const { return timestamp_; }
    [[nodiscard]] constexpr TouchPhase phase() const { return phase_; }
    [[nodiscard]] constexpr uint32_t id() const { return id_; }
    [[nodiscard]] constexpr uint16_t x() const { return x_; }
    [[nodiscard]] constexpr uint16_t y() const { return y_; }
    [[nodiscard]] constexpr uint16_t pressure() const { return pressure_; }

   private:
    constexpr TouchEvent(TimePoint timestamp, TouchPhase phase, uint32_t id, uint16_t x, uint16_t y, uint16_t pressure)
        : timestamp_(timestamp), phase_(phase), id_(id), x_(x), y_(y), pressure_(pressure) {}

    TimePoint timestamp_{};
    TouchPhase phase_{TouchPhase::kCancel};
    uint32_t id_{};
    uint16_t x_{};
    uint16_t y_{};
    uint16_t pressure_{};

    friend class Application;
    friend class Event;
};

class TimerEvent final {
   public:
    constexpr TimerEvent(const TimerEvent&) = default;
    constexpr TimerEvent& operator=(const TimerEvent&) = default;

    [[nodiscard]] constexpr TimePoint timestamp() const { return timestamp_; }
    [[nodiscard]] constexpr Duration delta() const { return delta_; }

   private:
    constexpr TimerEvent() = default;
    constexpr TimerEvent(TimePoint timestamp, Duration delta, uint32_t source)
        : timestamp_(timestamp), delta_(delta), source_(source) {}

    TimePoint timestamp_{};
    Duration delta_{};
    uint32_t source_{};

    friend class Application;
    friend class Event;
    friend class Timer;
};

class Event final {
   public:
    constexpr Event(const Event&) = default;
    constexpr Event& operator=(const Event&) = default;
    constexpr Event(Event&&) = default;
    constexpr Event& operator=(Event&&) = default;

    [[nodiscard]] constexpr EventType type() const { return type_; }
    [[nodiscard]] constexpr TimePoint timestamp() const { return timestamp_; }

    // Returns a view only when this is a TimerEvent emitted by source.
    // The pointer remains valid until this Event is destroyed or reassigned.
    [[nodiscard]] const TimerEvent* TimerFrom(const Timer& source) const;
    [[nodiscard]] constexpr const TouchEvent* touch() const { return type_ == EventType::kTouch ? &touch_ : nullptr; }
    [[nodiscard]] ResourceReadyEvent* ResourceFrom(LoadRequest& request);

   private:
    [[nodiscard]] constexpr const TimerEvent* timer() const { return type_ == EventType::kTimer ? &timer_ : nullptr; }

    explicit constexpr Event(TimePoint timestamp) : type_(EventType::kUnknown), timestamp_(timestamp) {}

    constexpr Event(EventType type, TimePoint timestamp) : type_(type), timestamp_(timestamp) {}

    explicit constexpr Event(TimerEvent timer)
        : type_(EventType::kTimer), timestamp_(timer.timestamp()), timer_(timer) {}

    explicit constexpr Event(TouchEvent touch)
        : type_(EventType::kTouch), timestamp_(touch.timestamp()), touch_(touch) {}

    explicit constexpr Event(ResourceReadyEvent resource) : type_(EventType::kResourceReady), resource_(resource) {}

    EventType type_{EventType::kUnknown};
    TimePoint timestamp_{};
    TimerEvent timer_{};
    TouchEvent touch_{TimePoint{}, TouchPhase::kCancel, 0U, 0U, 0U, 0U};
    ResourceReadyEvent resource_{0U, 0U, 0};

    friend class Application;
};

}  // namespace micropixel

#endif
