#ifndef MICROPIXEL_SDK_APPLICATION_HPP
#define MICROPIXEL_SDK_APPLICATION_HPP

#include <type_traits>

#include "sdk/audio.hpp"
#include "sdk/clock.hpp"
#include "sdk/event.hpp"
#include "sdk/graphics.hpp"
#include "sdk/input.hpp"
#include "sdk/localization.hpp"
#include "sdk/log.hpp"
#include "sdk/random.hpp"
#include "sdk/resources.hpp"
#include "sdk/storage.hpp"
#include "sdk/timer.hpp"
#include "sdk/types.hpp"

namespace micropixel {

// The result of dispatching one Event through Application::Run(). Returning
// kExit lets the Guest finish successfully without waiting for a Host Stop.
enum class EventResult : uint8_t {
    kContinue,
    kExit,
};

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
    [[nodiscard]] constexpr Renderer renderer() const noexcept { return Renderer{Renderer::CapabilityToken{}}; }
    [[nodiscard]] constexpr Audio audio() const noexcept { return Audio{Audio::CapabilityToken{}}; }
    [[nodiscard]] constexpr Input input() const noexcept { return Input{Input::CapabilityToken{}}; }
    [[nodiscard]] constexpr Resources resources() const noexcept { return Resources{Resources::CapabilityToken{}}; }
    [[nodiscard]] constexpr KVStore storage() const noexcept { return KVStore{KVStore::CapabilityToken{}}; }
    [[nodiscard]] constexpr Localization localization() const noexcept {
        return Localization{Localization::CapabilityToken{}};
    }

    template <typename Handler>
    void Run(Handler&& handler) const {
        constexpr bool kValidHandler = requires(Handler& candidate, const Event& event) { candidate(event); };
        static_assert(kValidHandler, "Application::Run handler must accept const Event &");
        if constexpr (kValidHandler) {
            using HandlerResult = std::invoke_result_t<Handler&, const Event&>;
            constexpr bool kReturnsVoid = std::is_same_v<HandlerResult, void>;
            constexpr bool kReturnsEventResult = std::is_same_v<HandlerResult, EventResult>;
            static_assert(kReturnsVoid || kReturnsEventResult,
                          "Application::Run handler must return void or EventResult");
            BeginRun();
            for (;;) {
                Event event = WaitEvent();
                EventResult result = EventResult::kContinue;
                if constexpr (kReturnsEventResult) {
                    result = handler(event);
                } else {
                    handler(event);
                }
                if (result == EventResult::kExit || event.type() == EventType::kStop) {
                    EndRun();
                    return;
                }
            }
        }
    }

    // Advanced event access. Runtime/ABI failures Panic at the call site;
    // PollEvent and WaitEventFor return false only when no event arrived.
    [[nodiscard]] Event WaitEvent() const;
    [[nodiscard]] bool WaitEventFor(Event& event, Duration timeout) const;
    [[nodiscard]] bool PollEvent(Event& event) const;

   private:
    void BeginRun() const;
    void EndRun() const;
    [[nodiscard]] bool WaitEventInternal(Event& event, uint64_t timeout_us) const;

    mutable bool running_{};
};

}  // namespace micropixel

#endif
