#include "sdk/micropixel.hpp"
#include "sdk/result.hpp"

namespace {

uint32_t constructor_probe;

struct StartupProbe final {
    StartupProbe() { constructor_probe = 0x4d455441U; }
};

StartupProbe startup_probe;

class ResultProbe final {
   public:
    ResultProbe() = delete;
    explicit ResultProbe(uint32_t value) : value_(value) {}

    ResultProbe(const ResultProbe&) = delete;
    ResultProbe& operator=(const ResultProbe&) = delete;

    ResultProbe(ResultProbe&& other) noexcept : value_(other.value_) { other.value_ = 0U; }

    [[nodiscard]] uint32_t value() const { return value_; }

   private:
    uint32_t value_;
};

}  // namespace

int main() {
    micropixel::Application app;
    if (constructor_probe != 0x4d455441U) {
        return 10;
    }
    micropixel::Result<ResultProbe> failed =
        micropixel::unexpected(micropixel::Error{micropixel::ErrorCode::kInvalidArgument});
    if (failed || failed.error().code() != micropixel::ErrorCode::kInvalidArgument) {
        return 11;
    }

    micropixel::Result<ResultProbe> original{ResultProbe{42U}};
    auto moved = static_cast<micropixel::Result<ResultProbe>&&>(original);
    if (!original || !moved || original->value() != 0U || moved->value() != 42U) {
        return 13;
    }
    ResultProbe probe = *static_cast<micropixel::Result<ResultProbe>&&>(moved);
    if (!moved || probe.value() != 42U || moved->value() != 0U) {
        return 14;
    }

    micropixel::Result<uint32_t> fallback = micropixel::unexpected(micropixel::Error{micropixel::ErrorCode::kNotFound});
    if (static_cast<micropixel::Result<uint32_t>&&>(fallback).value_or(7U) != 7U) {
        return 16;
    }

    micropixel::Result<void> completed;
    completed.value();

    /* Application accessors return copyable views of the same Guest services.
       Copies do not create new clocks, logs, or Timer resources. */
    micropixel::Clock clock = app.clock();
    micropixel::Clock same_clock = clock;
    micropixel::TimePoint first = clock.Now();
    micropixel::TimePoint second = same_clock.Now();
    if (second < first) {
        return 15;
    }

    micropixel::Timers timers = app.timers();
    micropixel::Timers same_timers = timers;
    (void)same_timers;

    micropixel::Log log = app.log();
    micropixel::Log same_log = log;
    const micropixel::Locale locale = app.localization().CurrentLocale();
    if (locale.tag()[0] != 'e' || locale.tag()[1] != 'n' || locale.tag()[2] != '\0') {
        return 17;
    }
    same_log.Debug("sdk_hello: debug log available");
    same_log.Info("sdk_hello: startup ABI check, service views and SDK are ready");
    same_log.Warning("sdk_hello: warning log available");
    same_log.Error("sdk_hello: error log available");
    return 0;
}
