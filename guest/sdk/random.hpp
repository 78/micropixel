#ifndef MICROPIXEL_SDK_RANDOM_HPP
#define MICROPIXEL_SDK_RANDOM_HPP

#include <stdint.h>

namespace micropixel {

class Application;

// Lightweight view of the Host random service. Values come from the selected
// platform's hardware RNG rather than a Guest-local deterministic generator.
class Random final {
   public:
    [[nodiscard]] uint32_t U32() const;
    // Returns a uniform value in [0, upper_bound) without modulo bias.
    // upper_bound must be greater than zero.
    [[nodiscard]] uint32_t Below(uint32_t upper_bound) const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Random(CapabilityToken) noexcept {}
    friend class Application;
};

// Deterministic Guest-local xorshift32 generator. Use it wherever a run must
// replay identically: level generation from a seed, autopilot benchmarks,
// unit tests. Seed it from Random::U32() when reproducibility is not needed
// but the Host service is too slow for per-particle use.
class XorShift32 final {
   public:
    static constexpr uint32_t kDefaultSeed = 0x9E3779B9U;

    constexpr explicit XorShift32(uint32_t seed = kDefaultSeed) : state_(seed == 0U ? kDefaultSeed : seed) {}

    constexpr void Seed(uint32_t seed) { state_ = seed == 0U ? kDefaultSeed : seed; }

    [[nodiscard]] constexpr uint32_t Next() {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return state_;
    }

    // Uniform in [0, upper_bound); upper_bound of zero yields zero. The slight
    // modulo bias is irrelevant for game use.
    [[nodiscard]] constexpr uint32_t Below(uint32_t upper_bound) {
        return upper_bound == 0U ? 0U : Next() % upper_bound;
    }

    // Uniform in [0, 1).
    [[nodiscard]] constexpr float Unit() { return static_cast<float>(Next() >> 8U) * (1.0F / 16777216.0F); }

    // Uniform in [low, high).
    [[nodiscard]] constexpr float Range(float low, float high) { return low + (high - low) * Unit(); }

    [[nodiscard]] constexpr uint32_t state() const { return state_; }

   private:
    uint32_t state_;
};

}  // namespace micropixel

#endif
