#ifndef MICROPIXEL_SDK_SCHEDULE_HPP
#define MICROPIXEL_SDK_SCHEDULE_HPP

#include "sdk/event.hpp"
#include "sdk/timer.hpp"

namespace micropixel {

class Application;

namespace detail {

enum class ScheduleKind {
    kOnce,
    kRepeating,
};

template <ScheduleKind Kind, typename Callback>
class TimerSchedule final {
   public:
    static_assert(
        requires(Callback& callback, const TimerEvent& tick) { callback(tick); } ||
            requires(Callback& callback) { callback(); },
        "Timer callback must accept no arguments or const TimerEvent &");

    TimerSchedule(const TimerSchedule&) = delete;
    TimerSchedule& operator=(const TimerSchedule&) = delete;
    TimerSchedule(TimerSchedule&&) = default;
    TimerSchedule& operator=(TimerSchedule&&) = delete;

   private:
    TimerSchedule(Timer timer, Callback callback)
        : timer_(static_cast<Timer&&>(timer)), callback_(static_cast<Callback&&>(callback)) {}

    void Dispatch(const Event& event) {
        if constexpr (Kind == ScheduleKind::kOnce) {
            if (!active_) {
                return;
            }
        }
        const TimerEvent* timer_event = event.TimerFrom(timer_);
        if (timer_event == nullptr) {
            return;
        }
        if constexpr (Kind == ScheduleKind::kOnce) {
            active_ = false;
            timer_.Release();
        }
        if constexpr (requires(Callback& callback, const TimerEvent& tick) { callback(tick); }) {
            callback_(*timer_event);
        } else {
            callback_();
        }
    }

    Timer timer_;
    Callback callback_;
    bool active_{true};

    friend class micropixel::Application;
};

}  // namespace detail
}  // namespace micropixel

#endif
