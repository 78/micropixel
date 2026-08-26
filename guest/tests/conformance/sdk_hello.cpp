#include "sdk/micropixel.hpp"
#include "sdk/result.hpp"

using micropixel::literals::operator""_ms;
using micropixel::literals::operator""_s;

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
    static_assert(1_s == 1000_ms);
    static_assert(250_ms * 4U == 1_s);
    static_assert(1_s / 4U == 250_ms);
    static_assert(1_s - 250_ms == 750_ms);
    static_assert(micropixel::KVStore::kMaximumKeyBytes == 15U);
    static_assert(micropixel::KVStore::kMaximumValueBytes == 4096U);
    static_assert(micropixel::Log::kMaximumMessageBytes == 1023U);
    static_assert(micropixel::FixedString<5U>::capacity() == 4U);

    micropixel::Application app;
    if (constructor_probe != 0x4d455441U) {
        return 10;
    }
    micropixel::Result<ResultProbe> failed =
        micropixel::unexpected(micropixel::Error{micropixel::ErrorCode::kInvalidArgument});
    if (failed || failed.error().code() != micropixel::ErrorCode::kInvalidArgument) {
        return 11;
    }
    if (failed.error().name()[0] != 'i' ||
        micropixel::ErrorCodeName(micropixel::ErrorCode::kNotFound)[0] != 'n') {
        return 12;
    }

    micropixel::FixedString<5U> text;
    if (!text.Append("1234") || text.Append("5") || !text.truncated() || text.size() != text.capacity()) {
        return 21;
    }
    text.Clear();
    if (text.truncated() || !text.AppendUint(42U) || text.size() != 2U) {
        return 22;
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
    if (first + 1_s <= first || (first + 1_s) - 1_s != first) {
        return 18;
    }

    micropixel::Random random = app.random();
    if (random.Below(1U) != 0U) {
        return 19;
    }
    for (uint32_t count = 0U; count < 16U; ++count) {
        if (random.Below(7U) >= 7U) {
            return 20;
        }
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
