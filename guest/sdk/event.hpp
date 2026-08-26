#ifndef MICROPIXEL_SDK_EVENT_HPP
#define MICROPIXEL_SDK_EVENT_HPP

#include <stdint.h>

#include "sdk/geometry.hpp"
#include "sdk/types.hpp"

namespace micropixel {

class Application;
class Playback;
class Timer;

enum class EventType : uint16_t {
    kUnknown,
    kResume,
    kStop,
    kTimer,
    kTouch,
    kKey,
    kAudioPlayback,
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
    [[nodiscard]] constexpr int32_t x() const { return x_; }
    [[nodiscard]] constexpr int32_t y() const { return y_; }
    [[nodiscard]] constexpr Point position() const { return Point{x_, y_}; }
    [[nodiscard]] constexpr bool has_pressure() const { return has_pressure_; }
    [[nodiscard]] constexpr uint16_t pressure_per_mille() const { return pressure_per_mille_; }

   private:
    constexpr TouchEvent(TimePoint timestamp, TouchPhase phase, uint32_t id, int32_t x, int32_t y, bool has_pressure,
                         uint16_t pressure_per_mille)
        : timestamp_(timestamp),
          phase_(phase),
          id_(id),
          x_(x),
          y_(y),
          has_pressure_(has_pressure),
          pressure_per_mille_(pressure_per_mille) {}

    TimePoint timestamp_{};
    TouchPhase phase_{TouchPhase::kCancel};
    uint32_t id_{};
    int32_t x_{};
    int32_t y_{};
    bool has_pressure_{};
    uint16_t pressure_per_mille_{};

    friend class Application;
    friend class Event;
};

enum class KeyCode : uint16_t {
    // Directional controls and logical actions are mapped by the Host.
    kUp = 1,
    kDown = 2,
    kLeft = 3,
    kRight = 4,
    kConfirm = 5,
    kBack = 6,
    kMenu = 7,

    // Face buttons are named by physical position so their meaning does not
    // depend on Xbox/Nintendo/PlayStation label conventions.
    kSouth = 8,
    kEast = 9,
    kWest = 10,
    kNorth = 11,
};

enum class KeyPhase : uint8_t {
    kDown,
    kUp,
    kRepeat,
    kCancel,
};

class KeyEvent final {
   public:
    constexpr KeyEvent(const KeyEvent&) = default;
    constexpr KeyEvent& operator=(const KeyEvent&) = default;

    [[nodiscard]] constexpr TimePoint timestamp() const { return timestamp_; }
    [[nodiscard]] constexpr KeyCode code() const { return code_; }
    [[nodiscard]] constexpr KeyPhase phase() const { return phase_; }
    [[nodiscard]] constexpr uint32_t repeat_count() const { return repeat_count_; }

   private:
    constexpr KeyEvent(TimePoint timestamp, KeyCode code, KeyPhase phase, uint32_t repeat_count)
        : timestamp_(timestamp), code_(code), phase_(phase), repeat_count_(repeat_count) {}

    TimePoint timestamp_{};
    KeyCode code_{KeyCode::kConfirm};
    KeyPhase phase_{KeyPhase::kCancel};
    uint32_t repeat_count_{};

    friend class Application;
    friend class Event;
};

class TimerEvent final {
   public:
    constexpr TimerEvent(const TimerEvent&) = default;
    constexpr TimerEvent& operator=(const TimerEvent&) = default;

    [[nodiscard]] constexpr TimePoint timestamp() const { return timestamp_; }
    [[nodiscard]] constexpr Duration delta() const { return delta_; }
    [[nodiscard]] constexpr uint32_t missed_count() const { return missed_count_; }

   private:
    constexpr TimerEvent() = default;
    constexpr TimerEvent(TimePoint timestamp, Duration delta, uint32_t missed_count, uint32_t source)
        : timestamp_(timestamp), delta_(delta), missed_count_(missed_count), source_(source) {}

    TimePoint timestamp_{};
    Duration delta_{};
    uint32_t missed_count_{};
    uint32_t source_{};

    friend class Application;
    friend class Event;
    friend class Timer;
};

class AudioPlaybackEvent final {
   public:
    constexpr AudioPlaybackEvent(const AudioPlaybackEvent&) = default;
    constexpr AudioPlaybackEvent& operator=(const AudioPlaybackEvent&) = default;

    [[nodiscard]] constexpr TimePoint timestamp() const { return timestamp_; }
    [[nodiscard]] constexpr bool succeeded() const { return succeeded_; }

   private:
    constexpr AudioPlaybackEvent() = default;
    constexpr AudioPlaybackEvent(TimePoint timestamp, bool succeeded, uint32_t source)
        : timestamp_(timestamp), succeeded_(succeeded), source_(source) {}

    TimePoint timestamp_{};
    bool succeeded_{};
    uint32_t source_{};

    friend class Application;
    friend class Event;
    friend class Playback;
};

class Event final {
   public:
    constexpr Event() = default;
    constexpr Event(const Event&) = default;
    constexpr Event& operator=(const Event&) = default;
    constexpr Event(Event&&) = default;
    constexpr Event& operator=(Event&&) = default;

    [[nodiscard]] constexpr EventType type() const { return type_; }
    [[nodiscard]] constexpr TimePoint timestamp() const { return timestamp_; }

    // Returns a view only when this is a TimerEvent emitted by source.
    // The pointer remains valid until this Event is destroyed or reassigned.
    [[nodiscard]] const TimerEvent* TimerFrom(const Timer& source) const;
    [[nodiscard]] const AudioPlaybackEvent* PlaybackFrom(const Playback& source) const;
    [[nodiscard]] constexpr const TouchEvent* touch() const { return type_ == EventType::kTouch ? &touch_ : nullptr; }
    [[nodiscard]] constexpr const KeyEvent* key() const { return type_ == EventType::kKey ? &key_ : nullptr; }

   private:
    [[nodiscard]] constexpr const TimerEvent* timer() const { return type_ == EventType::kTimer ? &timer_ : nullptr; }
    [[nodiscard]] constexpr const AudioPlaybackEvent* audio_playback() const {
        return type_ == EventType::kAudioPlayback ? &audio_playback_ : nullptr;
    }

    explicit constexpr Event(TimePoint timestamp) : type_(EventType::kUnknown), timestamp_(timestamp) {}

    constexpr Event(EventType type, TimePoint timestamp) : type_(type), timestamp_(timestamp) {}

    explicit constexpr Event(TimerEvent timer)
        : type_(EventType::kTimer), timestamp_(timer.timestamp()), timer_(timer) {}

    explicit constexpr Event(TouchEvent touch)
        : type_(EventType::kTouch), timestamp_(touch.timestamp()), touch_(touch) {}

    explicit constexpr Event(KeyEvent key) : type_(EventType::kKey), timestamp_(key.timestamp()), key_(key) {}

    explicit constexpr Event(AudioPlaybackEvent playback)
        : type_(EventType::kAudioPlayback), timestamp_(playback.timestamp()), audio_playback_(playback) {}

    EventType type_{EventType::kUnknown};
    TimePoint timestamp_{};
    TimerEvent timer_{};
    TouchEvent touch_{TimePoint{}, TouchPhase::kCancel, 0U, 0, 0, false, 0U};
    KeyEvent key_{TimePoint{}, KeyCode::kConfirm, KeyPhase::kCancel, 0U};
    AudioPlaybackEvent audio_playback_{};
    friend class Application;
};

}  // namespace micropixel

#endif
