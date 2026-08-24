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
    [[nodiscard]] constexpr Renderer renderer() const noexcept { return Renderer{Renderer::CapabilityToken{}}; }
    [[nodiscard]] constexpr Audio audio() const noexcept { return Audio{Audio::CapabilityToken{}}; }
    [[nodiscard]] constexpr Input input() const noexcept { return Input{Input::CapabilityToken{}}; }
    [[nodiscard]] constexpr Resources resources() const noexcept { return Resources{Resources::CapabilityToken{}}; }
    [[nodiscard]] constexpr KVStore storage() const noexcept { return KVStore{KVStore::CapabilityToken{}}; }

    template <typename Handler>
    void Run(Handler&& handler) const {
        constexpr bool kValidHandler = requires(Handler& candidate, const Event& event) { candidate(event); };
        static_assert(kValidHandler, "Application::Run handler must accept const Event &");
        if constexpr (kValidHandler) {
            BeginRun();
            for (;;) {
                Event event = WaitEvent();
                handler(event);
                if (event.type() == EventType::kStop) {
                    EndRun();
                    return;
                }
            }
        }
    }

    // Advanced event access. Runtime/ABI failures Panic at the call site.
    [[nodiscard]] Event WaitEvent() const;

   private:
    void BeginRun() const;
    void EndRun() const;

    mutable bool running_{};
};

}  // namespace micropixel

#endif
