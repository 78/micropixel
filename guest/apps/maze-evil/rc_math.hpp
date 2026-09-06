#ifndef MICROPIXEL_APPS_MAZE_BREAK_RC_MATH_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_RC_MATH_HPP

#include <stdint.h>

// Freestanding float helpers. Guests link without libm, so everything the
// raycaster needs is either a Wasm instruction (sqrt/floor/abs) or a short
// polynomial. Accuracy is ~1e-6 for sin/cos and ~1e-4 rad for atan, which is
// far below what a 480-column raycaster can show.
namespace maze_break::math {

inline constexpr float kPi = 3.14159265358979F;
inline constexpr float kTwoPi = 6.28318530717959F;
inline constexpr float kHalfPi = 1.57079632679490F;

[[nodiscard]] inline float Sqrt(float value) { return __builtin_sqrtf(value); }
[[nodiscard]] inline float Floor(float value) { return __builtin_floorf(value); }
[[nodiscard]] inline float Fabs(float value) { return __builtin_fabsf(value); }
[[nodiscard]] inline int FloorInt(float value) { return static_cast<int>(__builtin_floorf(value)); }

[[nodiscard]] inline float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

[[nodiscard]] inline int Clamp(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// Odd polynomial on [-pi/2, pi/2]; input reduced to that range first.
[[nodiscard]] inline float Sin(float radians) {
    // Reduce to [-pi, pi].
    float x = radians - kTwoPi * Floor((radians + kPi) / kTwoPi);
    // Fold to [-pi/2, pi/2].
    if (x > kHalfPi) {
        x = kPi - x;
    } else if (x < -kHalfPi) {
        x = -kPi - x;
    }
    // Taylor series through x^11; the truncation error on [-pi/2, pi/2] is
    // below 1e-6.
    const float x2 = x * x;
    return x * (1.0F + x2 * (-1.0F / 6.0F +
                             x2 * (1.0F / 120.0F +
                                   x2 * (-1.0F / 5040.0F + x2 * (1.0F / 362880.0F + x2 * (-1.0F / 39916800.0F))))));
}

[[nodiscard]] inline float Cos(float radians) { return Sin(radians + kHalfPi); }

// atan on [0, 1] via a degree-9 polynomial, extended with atan(x) = pi/2 - atan(1/x).
[[nodiscard]] inline float Atan(float value) {
    const bool negative = value < 0.0F;
    float x = negative ? -value : value;
    const bool inverted = x > 1.0F;
    if (inverted) {
        x = 1.0F / x;
    }
    const float x2 = x * x;
    float result =
        x * (0.99997726F +
             x2 * (-0.33262347F + x2 * (0.19354346F + x2 * (-0.11643287F + x2 * (0.05265332F + x2 * -0.01172120F)))));
    if (inverted) {
        result = kHalfPi - result;
    }
    return negative ? -result : result;
}

// Deterministic xorshift32, shared by AI wobble and (in benchmark mode) the
// autopilot so every run replays identically.
class Rng final {
   public:
    constexpr explicit Rng(uint32_t seed = 0x9E3779B9U) : state_(seed == 0U ? 0x9E3779B9U : seed) {}

    [[nodiscard]] uint32_t Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    // Uniform in [0, 1).
    [[nodiscard]] float Unit() { return static_cast<float>(Next() >> 8) * (1.0F / 16777216.0F); }

   private:
    uint32_t state_;
};

}  // namespace maze_break::math

#endif
