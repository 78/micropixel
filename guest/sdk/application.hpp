#ifndef MICROPIXEL_SDK_APPLICATION_HPP
#define MICROPIXEL_SDK_APPLICATION_HPP

#include "sdk/audio.hpp"
#include "sdk/clock.hpp"
#include "sdk/event.hpp"
#include "sdk/graphics.hpp"
#include "sdk/input.hpp"
#include "sdk/log.hpp"
#include "sdk/random.hpp"
#include "sdk/resources.hpp"
#include "sdk/schedule.hpp"
#include "sdk/storage.hpp"
#include "sdk/timer.hpp"
#include "sdk/types.hpp"

namespace micropixel {

// Discoverable capability facade for one Guest application. Service accessors
// return lightweight views; Application does not own their Host implementations
// or the resources created through them.
class Application final {
   public:
    constexpr Application() noexcept = default;

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] constexpr Log log() const noexcept { return Log{Log::CapabilityToken{}}; }
    [[nodiscard]] constexpr Clock clock() const noexcept { return Clock{Clock::CapabilityToken{}}; }
    [[nodiscard]] constexpr Random random() const noexcept { return Random{Random::CapabilityToken{}}; }
    [[nodiscard]] constexpr Timers timers() const noexcept { return Timers{Timers::CapabilityToken{}}; }
    [[nodiscard]] constexpr Graphics graphics() const noexcept { return Graphics{Graphics::CapabilityToken{}}; }
    [[nodiscard]] constexpr Audio audio() const noexcept { return Audio{Audio::CapabilityToken{}}; }
    [[nodiscard]] constexpr Input input() const noexcept { return Input{Input::CapabilityToken{}}; }
    [[nodiscard]] constexpr Resources resources() const noexcept { return Resources{Resources::CapabilityToken{}}; }
    [[nodiscard]] constexpr KVStore storage() const noexcept { return KVStore{KVStore::CapabilityToken{}}; }

    template <typename Callback>
    [[nodiscard]] auto After(Duration delay, Callback callback) const {
        using Schedule = detail::TimerSchedule<detail::ScheduleKind::kOnce, Callback>;
        return Schedule{timers().After(delay), static_cast<Callback&&>(callback)};
    }

    template <typename Callback>
    [[nodiscard]] auto Every(Duration period, Callback callback) const {
        using Schedule = detail::TimerSchedule<detail::ScheduleKind::kRepeating, Callback>;
        return Schedule{timers().Every(period), static_cast<Callback&&>(callback)};
    }

    template <typename... Schedules>
    [[noreturn]] void Run(Schedules&&... schedules) const {
        static_assert(sizeof...(Schedules) > 0U, "Application::Run requires at least one schedule");
        constexpr bool kValidSchedules =
            (requires(Schedules& schedule, const Event& event) { schedule.Dispatch(event); } && ...);
        static_assert(kValidSchedules, "Application::Run accepts only tasks or subscriptions returned by the SDK");
        if constexpr (kValidSchedules) {
            for (;;) {
                Event event = WaitEvent();
                (schedules.Dispatch(event), ...);
            }
        } else {
            __builtin_unreachable();
        }
    }

    // Advanced event access. Runtime/ABI failures Panic at the call site.
    [[nodiscard]] Event WaitEvent() const;
};

}  // namespace micropixel

#endif
