// SPDX-License-Identifier: Apache-2.0
// Host-side coverage of the header-only SDK game helpers: freestanding math,
// the deterministic XorShift32 generator, CyclicPool and ToneSequencer.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

#include "sdk/cyclic_pool.hpp"
#include "sdk/fixed_string.hpp"
#include "sdk/geometry.hpp"
#include "sdk/launch_arguments.hpp"
#include "sdk/math.hpp"
#include "sdk/random.hpp"
#include "sdk/storage.hpp"
#include "sdk/tilt_filter.hpp"
#include "sdk/tone_sequencer.hpp"

namespace micropixel {

// ToneSequencer talks to the Audio service view; the test replaces the Guest
// Runtime lowering with a recorder and mints the view the way Application does.
namespace {
std::vector<Tone> g_played;
uint32_t g_stop_all_calls = 0U;
bool g_reject_commands = false;
}  // namespace

namespace {
std::vector<const char*> g_arguments;
uint32_t g_stored_best = 0U;
bool g_store_has_best = false;
}  // namespace

class Application final {
   public:
    static constexpr Audio MakeAudio() { return Audio{Audio::CapabilityToken{}}; }
    static constexpr LaunchArguments MakeArguments() { return LaunchArguments{LaunchArguments::CapabilityToken{}}; }
    static constexpr KVStore MakeStore() { return KVStore{KVStore::CapabilityToken{}}; }
};

uint32_t LaunchArguments::count() const { return static_cast<uint32_t>(g_arguments.size()); }
const char* LaunchArguments::Get(uint32_t index) const {
    return index < g_arguments.size() ? g_arguments[index] : nullptr;
}
const char* LaunchArguments::FindValue(const char* name) const {
    // Mirrors the Runtime: "--name value" or "--name=value".
    const std::string_view wanted{name};
    for (uint32_t index = 0U; index < count(); ++index) {
        const std::string_view argument{g_arguments[index]};
        if (argument == wanted) {
            return index + 1U < count() ? g_arguments[index + 1U] : nullptr;
        }
        if (argument.size() > wanted.size() && argument.substr(0, wanted.size()) == wanted &&
            argument[wanted.size()] == '=') {
            return g_arguments[index] + wanted.size() + 1U;
        }
    }
    return nullptr;
}

Result<uint32_t> KVStore::GetU32(const char*) const {
    if (!g_store_has_best) {
        return unexpected(Error{ErrorCode::kNotFound});
    }
    return g_stored_best;
}
Result<bool> KVStore::GetBool(const char*) const { return unexpected(Error{ErrorCode::kNotFound}); }

Result<void> Audio::Play(const Tone& tone) const {
    if (g_reject_commands) {
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }
    g_played.push_back(tone);
    return {};
}

Result<void> Audio::StopAll() const {
    ++g_stop_all_calls;
    return g_reject_commands ? Result<void>{unexpected(Error{ErrorCode::kInvalidState})} : Result<void>{};
}

}  // namespace micropixel

namespace {

namespace math = micropixel::math;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool Near(float value, float expected, float tolerance) { return std::fabs(value - expected) <= tolerance; }

void TrigMatchesLibm() {
    for (int step = -720; step <= 720; ++step) {
        const float radians = static_cast<float>(step) * 0.05F;
        Check(Near(math::Sin(radians), std::sin(radians), 2e-5F), "Sin tracks libm within 2e-5");
        Check(Near(math::Cos(radians), std::cos(radians), 2e-5F), "Cos tracks libm within 2e-5");
        const float wrapped = math::WrapAngle(radians);
        Check(wrapped >= -math::kPi && wrapped < math::kPi, "WrapAngle lands in [-pi, pi)");
    }
    for (int step = -200; step <= 200; ++step) {
        const float value = static_cast<float>(step) * 0.1F;
        Check(Near(math::Atan(value), std::atan(value), 2e-4F), "Atan tracks libm within 2e-4 rad");
        for (int other = -20; other <= 20; ++other) {
            const float x = static_cast<float>(other) * 0.5F;
            if (value == 0.0F && x == 0.0F) {
                continue;
            }
            Check(Near(math::Atan2(value, x), std::atan2(value, x), 3e-4F), "Atan2 tracks libm in all quadrants");
        }
    }
    Check(math::Atan2(0.0F, 0.0F) == 0.0F, "Atan2 at the origin is zero");
    Check(math::Sqrt(-1.0F) == 0.0F && math::Sqrt(16.0F) == 4.0F, "Sqrt clamps negatives to zero");
}

void ScalarHelpers() {
    Check(math::Clamp(5, 0, 3) == 3 && math::Clamp(-1.0F, 0.0F, 1.0F) == 0.0F, "Clamp both ends");
    Check(math::Min(2, 7) == 2 && math::Max(2, 7) == 7, "Min and Max");
    Check(math::Abs(-3) == 3 && math::Abs(-2.5F) == 2.5F, "Abs for int and float");
    Check(math::FloorToInt(-0.5F) == -1 && math::FloorToInt(2.9F) == 2, "FloorToInt rounds down");
    Check(math::RoundToInt(2.5F) == 3 && math::RoundToInt(-2.5F) == -3, "RoundToInt rounds half away from zero");
    Check(math::Lerp(10.0F, 20.0F, 0.25F) == 12.5F, "Lerp");
    Check(math::SmoothStep(-1.0F) == 0.0F && math::SmoothStep(2.0F) == 1.0F && math::SmoothStep(0.5F) == 0.5F,
          "SmoothStep clamps and is symmetric");
    Check(math::ApplyDeadzone(0.05F, 0.1F) == 0.0F, "inside the deadzone reads zero");
    Check(Near(math::ApplyDeadzone(1.0F, 0.1F), 1.0F, 1e-6F), "full deflection survives the deadzone");
    Check(Near(math::ApplyDeadzone(-0.55F, 0.1F), -0.5F, 1e-6F), "the remaining range is rescaled");
    Check(math::ApplyDeadzone(0.5F, 1.0F) == 0.0F, "a full deadzone always reads zero");
    Check(Near(math::ApproachAngle(0.0F, 3.0F, 0.5F), 0.5F, 1e-6F), "ApproachAngle steps toward the target");
    Check(Near(math::ApproachAngle(2.5F, -3.0F, 0.5F), 3.0F, 1e-5F),
          "ApproachAngle takes the shorter arc across the wrap");
    Check(Near(math::ApproachAngle(3.0F, -3.0F, 0.5F), -3.0F, 1e-6F),
          "ApproachAngle snaps across the wrap when the arc is short");
    Check(math::ApproachAngle(1.0F, 1.2F, 0.5F) == 1.2F, "ApproachAngle snaps when within one step");
}

void XorShiftIsDeterministic() {
    micropixel::XorShift32 first{1234U};
    micropixel::XorShift32 second{1234U};
    for (int i = 0; i < 1000; ++i) {
        Check(first.Next() == second.Next(), "equal seeds replay identically");
    }
    micropixel::XorShift32 zero_seed{0U};
    Check(zero_seed.state() == micropixel::XorShift32::kDefaultSeed, "seed zero falls back so the state never sticks");
    micropixel::XorShift32 rng{99U};
    for (int i = 0; i < 1000; ++i) {
        Check(rng.Below(10U) < 10U, "Below stays in range");
        const float unit = rng.Unit();
        Check(unit >= 0.0F && unit < 1.0F, "Unit stays in [0, 1)");
        const float ranged = rng.Range(-2.0F, 2.0F);
        Check(ranged >= -2.0F && ranged < 2.0F, "Range stays in [low, high)");
    }
    Check(rng.Below(0U) == 0U, "Below(0) is zero rather than a division");
    rng.Seed(1234U);
    micropixel::XorShift32 fresh{1234U};
    Check(rng.Next() == fresh.Next(), "Seed restarts the sequence");
}

void CyclicPoolWrapsToTheOldestSlot() {
    struct Particle final {
        int id{};
        bool active{};
    };
    micropixel::CyclicPool<Particle, 3U> pool;
    Check(micropixel::CyclicPool<Particle, 3U>::capacity() == 3U, "capacity is exposed");
    for (int id = 1; id <= 4; ++id) {
        Particle& slot = pool.Acquire();
        slot.id = id;
        slot.active = true;
    }
    Check(pool[0].id == 4 && pool[1].id == 2 && pool[2].id == 3, "the fourth acquire reuses the oldest slot");
    int active = 0;
    for (const Particle& particle : pool) {
        active += particle.active ? 1 : 0;
    }
    Check(active == 3, "range-for visits every slot");
    pool.ResetCursor();
    pool.Acquire().id = 9;
    Check(pool[0].id == 9, "ResetCursor restarts at slot zero");
    pool.Clear();
    Check(pool[0].id == 0 && !pool[1].active, "Clear value-initializes the slots");
}

void ToneSequencerSchedulesProfiles() {
    using micropixel::Duration;
    using micropixel::ToneSpec;
    using micropixel::Waveform;
    micropixel::g_played.clear();
    micropixel::g_stop_all_calls = 0U;
    micropixel::g_reject_commands = false;

    constexpr ToneSpec kProfile[] = {
        {Waveform::kSquare, 440U, 80U, 300U, 4U, 30U, 0U},
        {Waveform::kTriangle, 660U, 80U, 300U, 4U, 30U, 50U},
        {Waveform::kSine, 880U, 80U, 300U, 4U, 30U, 120U},
    };
    const micropixel::Tone loud = kProfile[1].ToTone();
    Check(loud.frequency_hz == 660U && loud.volume_per_mille == 300U && loud.duration.count_microseconds() == 80'000U &&
              loud.attack.count_microseconds() == 4'000U,
          "ToTone copies the authored note");
    Check(kProfile[1].ToTone(128U).volume_per_mille == 151U, "gain scales the volume with rounding");
    Check(kProfile[1].ToTone(0U).volume_per_mille == 0U, "gain zero silences");

    micropixel::ToneSequencer<2U> tones{micropixel::Application::MakeAudio(), true};
    Check(tones.Play(kProfile), "a three-note profile fits two delay slots");
    Check(micropixel::g_played.size() == 1U && micropixel::g_played[0].frequency_hz == 440U,
          "the undelayed note starts immediately");
    Check(tones.pending() == 2U, "two notes wait");
    tones.Advance(Duration::Milliseconds(30U));
    Check(micropixel::g_played.size() == 1U, "nothing is due after 30 ms");
    tones.Advance(Duration::Milliseconds(30U));
    Check(micropixel::g_played.size() == 2U && micropixel::g_played[1].frequency_hz == 660U,
          "the 50 ms note fires once its delay elapsed");
    tones.Advance(Duration::Milliseconds(100U));
    Check(micropixel::g_played.size() == 3U && micropixel::g_played[2].frequency_hz == 880U,
          "the 120 ms note fires on a late frame");
    Check(tones.pending() == 0U && tones.dropped() == 0U, "the pool drains without drops");

    // A full pool drops the late notes and reports it, keeping the rest.
    micropixel::g_played.clear();
    micropixel::ToneSequencer<1U> small{micropixel::Application::MakeAudio(), true};
    Check(!small.Play(kProfile), "one delay slot cannot hold two delayed notes");
    Check(small.dropped() == 1U && small.pending() == 1U && micropixel::g_played.size() == 1U,
          "the overflow note is dropped, the others survive");
    small.Clear();
    Check(small.pending() == 0U && micropixel::g_stop_all_calls == 0U, "Clear forgets pending notes silently");
    Check(small.StopAll() && micropixel::g_stop_all_calls == 1U, "StopAll reaches the Host");

    // Host rejections count as drops without stopping the game.
    micropixel::g_reject_commands = true;
    micropixel::ToneSequencer<4U> rejected{micropixel::Application::MakeAudio(), true};
    Check(!rejected.PlayNow(kProfile[0].ToTone()) && rejected.dropped() == 1U, "a rejected Play is a drop");
    Check(!rejected.StopAll() && rejected.dropped() == 2U, "a rejected StopAll is a drop");
    micropixel::g_reject_commands = false;

    // Disabled sequencers never touch the Host and never report drops.
    micropixel::g_played.clear();
    micropixel::g_stop_all_calls = 0U;
    micropixel::ToneSequencer<2U> disabled{micropixel::Application::MakeAudio(), false};
    Check(disabled.Play(kProfile) && disabled.StopAll(), "disabled calls succeed as no-ops");
    disabled.Advance(Duration::Seconds(1U));
    Check(micropixel::g_played.empty() && micropixel::g_stop_all_calls == 0U && disabled.dropped() == 0U,
          "a disabled sequencer is silent");
    disabled.set_enabled(true);
    Check(disabled.enabled() && disabled.PlayNow(kProfile[0].ToTone()) && micropixel::g_played.size() == 1U,
          "set_enabled turns the sequencer on");
}

void FixedStringAppendsDecimals() {
    micropixel::FixedString<32> line;
    Check(line.AppendFixed(-1.2345F, 2) && std::string_view{line.c_str()} == "-1.23", "two decimals, negative");
    line.Clear();
    Check(line.AppendFixed(9.8F) && std::string_view{line.c_str()} == "9.80", "default two decimals pad zeros");
    line.Clear();
    Check(line.AppendFixed(0.004F, 2) && std::string_view{line.c_str()} == "0.00", "tiny values do not print -0");
    line.Clear();
    Check(line.AppendFixed(-0.004F, 2) && std::string_view{line.c_str()} == "0.00", "negative tiny values neither");
    line.Clear();
    Check(line.AppendFixed(3.14159F, 4) && std::string_view{line.c_str()} == "3.1416", "four decimals round");
    line.Clear();
    Check(line.AppendFixed(2.5F, 0) && std::string_view{line.c_str()} == "2", "zero decimals is an integer");
    line.Clear();
    Check(line.AppendFixed(0.05F, 1) && std::string_view{line.c_str()} == "0.1", "0.05 rounds up to one decimal");
    micropixel::FixedString<4> tight;
    Check(!tight.AppendFixed(12.5F, 1) && tight.truncated(), "overflow reports truncation");
}

void LaunchArgumentFlagsAndNumbers() {
    micropixel::g_arguments = {"--benchmark", "--upscale=2",      "--frames",   "240",
                               "--seed=99x",  "--big=5000000000", "--buffers=3"};
    const micropixel::LaunchArguments args = micropixel::Application::MakeArguments();
    Check(args.HasFlag("--benchmark") && args.HasFlag("--upscale") && args.HasFlag("--frames"), "flags present");
    Check(args.HasFlag("--buffers=3") && !args.HasFlag("--buffers=2"), "exact match with value works");
    Check(!args.HasFlag("--bench") && !args.HasFlag("--perf"), "prefixes and absent flags are false");
    Check(args.GetUnsigned("--upscale", 1U) == 2U, "--name=N");
    Check(args.GetUnsigned("--frames", 1U) == 240U, "--name N");
    Check(args.GetUnsigned("--seed", 7U) == 7U, "non-digits fall back");
    Check(args.GetUnsigned("--big", 7U) == 7U, "overflow falls back");
    Check(args.GetUnsigned("--missing", 7U) == 7U, "absent falls back");
    micropixel::g_arguments = {"--frames"};
    Check(args.GetUnsigned("--frames", 3U) == 3U, "a dangling option falls back");
    micropixel::g_arguments.clear();
}

void StorageFallbacks() {
    const micropixel::KVStore store = micropixel::Application::MakeStore();
    micropixel::g_store_has_best = false;
    Check(store.GetU32Or("best", 42U) == 42U, "missing key yields the fallback");
    micropixel::g_store_has_best = true;
    micropixel::g_stored_best = 1234U;
    Check(store.GetU32Or("best", 42U) == 1234U, "present key yields the value");
    Check(store.GetBoolOr("flag", true), "missing bool yields the fallback");
}

void RectUnionAndIntersection() {
    using micropixel::Rect;
    const Rect a{0, 0, 10, 10};
    const Rect b{5, 5, 10, 10};
    const Rect apart{20, 20, 3, 3};
    Check(a.intersects(b) && b.intersects(a), "overlapping rects intersect");
    Check(!a.intersects(apart) && !a.intersects(Rect{10, 0, 5, 5}), "touching edges do not intersect");
    Check(!a.intersects(Rect{2, 2, 0, 5}), "empty rects never intersect");
    const Rect u = a.united(b);
    Check(u.x == 0 && u.y == 0 && u.width == 15 && u.height == 15, "union spans both");
    const Rect e = a.united(Rect{});
    Check(e.x == a.x && e.width == a.width && e.height == a.height, "union with empty is identity");
    const Rect f = Rect{}.united(apart);
    Check(f.x == 20 && f.width == 3, "empty united with a rect is that rect");
}

void TiltFilterCalibratesAndFilters() {
    using micropixel::Acceleration;
    using micropixel::Duration;
    using micropixel::TimePoint;
    micropixel::TiltFilterConfig config{};
    config.calibration_samples = 4U;
    config.filter_alpha = 1.0F;  // no lag: the tilt follows each sample exactly
    config.full_scale = 2.0F;
    config.deadzone = 0.1F;
    micropixel::TiltFilter filter{config};
    Check(!filter.calibrated() && filter.x() == 0.0F, "starts uncalibrated and centred");
    uint64_t at_us = 1000U;
    const auto feed = [&](float x, float y, float z) {
        return filter.Sample(Acceleration{{x, y, z}}, TimePoint{} + Duration::Microseconds(at_us += 10'000U));
    };
    // Resting slightly off-level: the neutral pose is the average of the calibration samples.
    Check(feed(0.2F, -0.1F, 9.8F), "first sample accepted");
    Check(Near(filter.calibration_progress(), 0.25F, 1e-6F), "progress advances");
    feed(0.2F, -0.1F, 9.8F);
    feed(0.2F, -0.1F, 9.8F);
    Check(!filter.calibrated(), "three of four samples");
    feed(0.2F, -0.1F, 9.8F);
    Check(filter.calibrated() && filter.calibration_progress() == 1.0F, "four samples calibrate");
    Check(Near(filter.neutral().x, 0.2F, 1e-6F) && Near(filter.neutral().y, -0.1F, 1e-6F), "neutral is the mean");
    Check(filter.x() == 0.0F && filter.y() == 0.0F, "at rest reads zero");
    // Repeated timestamps are ignored.
    Check(!filter.Sample(Acceleration{{5.0F, 5.0F, 5.0F}}, TimePoint{} + Duration::Microseconds(at_us)),
          "stale sample rejected");
    Check(filter.x() == 0.0F, "stale sample changes nothing");
    // Tilt right on the board: sensor X drops (inverted axis), full scale 2 m/s^2.
    feed(0.2F - 1.0F, -0.1F, 9.8F);
    Check(Near(filter.x(), (0.5F - 0.1F) / 0.9F, 1e-5F) && filter.y() == 0.0F, "half travel minus deadzone");
    feed(0.2F - 3.0F, -0.1F + 3.0F, 9.8F);
    Check(filter.x() == 1.0F && filter.y() == 1.0F, "clamped to full deflection; +Y is screen down");
    feed(0.2F + 0.15F, -0.1F, 9.8F);
    Check(filter.x() == 0.0F, "inside the deadzone reads zero");
    filter.Recalibrate();
    Check(!filter.calibrated() && filter.x() == 0.0F, "Recalibrate forgets the pose");
    // Low-pass: with alpha 0.5 a step reaches half way on the first sample.
    micropixel::TiltFilterConfig slow = config;
    slow.filter_alpha = 0.5F;
    slow.deadzone = 0.0F;
    slow.invert_x = false;
    micropixel::TiltFilter smooth{slow};
    for (int i = 0; i < 4; ++i)
        feed(0.0F, 0.0F, 9.8F),
            smooth.Sample(Acceleration{{0.0F, 0.0F, 9.8F}}, TimePoint{} + Duration::Microseconds(at_us));
    Check(smooth.calibrated(), "second filter calibrated");
    smooth.Sample(Acceleration{{2.0F, 0.0F, 9.8F}}, TimePoint{} + Duration::Microseconds(at_us += 10'000U));
    Check(Near(smooth.x(), 0.5F, 1e-5F), "alpha 0.5 reaches half of the step");
    smooth.Sample(Acceleration{{2.0F, 0.0F, 9.8F}}, TimePoint{} + Duration::Microseconds(at_us += 10'000U));
    Check(Near(smooth.x(), 0.75F, 1e-5F), "and three quarters on the next");
}

}  // namespace

int main() {
    TrigMatchesLibm();
    ScalarHelpers();
    XorShiftIsDeterministic();
    CyclicPoolWrapsToTheOldestSlot();
    ToneSequencerSchedulesProfiles();
    FixedStringAppendsDecimals();
    LaunchArgumentFlagsAndNumbers();
    StorageFallbacks();
    RectUnionAndIntersection();
    TiltFilterCalibratesAndFilters();
    std::cout << "sdk helper tests passed\n";
    return 0;
}
