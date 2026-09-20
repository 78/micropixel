#ifndef MICROPIXEL_SDK_MATH_HPP
#define MICROPIXEL_SDK_MATH_HPP

#include <stdint.h>

// Freestanding float helpers. Guests link without libm, so everything here is
// either a Wasm instruction (sqrt, floor, abs) or a short polynomial. Accuracy
// is about 1e-6 for Sin/Cos and 1e-4 rad for Atan/Atan2, which is below what
// any supported panel can show. Games that need exact reproducibility across
// builds should still avoid float in their simulation state.
namespace micropixel::math {

inline constexpr float kPi = 3.14159265358979F;
inline constexpr float kTwoPi = 6.28318530717959F;
inline constexpr float kHalfPi = 1.57079632679490F;

[[nodiscard]] inline float Sqrt(float value) { return value > 0.0F ? __builtin_sqrtf(value) : 0.0F; }
[[nodiscard]] inline float Floor(float value) { return __builtin_floorf(value); }
[[nodiscard]] inline float Abs(float value) { return __builtin_fabsf(value); }
[[nodiscard]] constexpr int32_t Abs(int32_t value) { return value < 0 ? -value : value; }

// Rounds toward negative infinity.
[[nodiscard]] inline int32_t FloorToInt(float value) { return static_cast<int32_t>(__builtin_floorf(value)); }
// Rounds half away from zero.
[[nodiscard]] constexpr int32_t RoundToInt(float value) {
    return static_cast<int32_t>(value >= 0.0F ? value + 0.5F : value - 0.5F);
}

template <typename T>
[[nodiscard]] constexpr T Clamp(T value, T low, T high) {
    return value < low ? low : (value > high ? high : value);
}

template <typename T>
[[nodiscard]] constexpr T Min(T first, T second) {
    return second < first ? second : first;
}

template <typename T>
[[nodiscard]] constexpr T Max(T first, T second) {
    return first < second ? second : first;
}

// Linear interpolation; `t` is not clamped.
[[nodiscard]] constexpr float Lerp(float from, float to, float t) { return from + (to - from) * t; }

// Hermite ease-in/ease-out on [0, 1]; `t` is clamped first.
[[nodiscard]] constexpr float SmoothStep(float t) {
    t = Clamp(t, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Rescales `value` in [-1, 1] so magnitudes below `deadzone` read as zero and
// the remaining range still reaches +-1. Thumbsticks and tilt sensors both
// need this to hold still at rest.
[[nodiscard]] constexpr float ApplyDeadzone(float value, float deadzone) {
    const float magnitude = value < 0.0F ? -value : value;
    if (magnitude <= deadzone || deadzone >= 1.0F) {
        return 0.0F;
    }
    const float scaled = (magnitude - deadzone) / (1.0F - deadzone);
    return value < 0.0F ? -scaled : scaled;
}

// Wraps to [-pi, pi).
[[nodiscard]] inline float WrapAngle(float radians) { return radians - kTwoPi * Floor((radians + kPi) / kTwoPi); }

// Odd Taylor polynomial through x^11 on [-pi/2, pi/2] after range reduction.
[[nodiscard]] inline float Sin(float radians) {
    float x = WrapAngle(radians);
    if (x > kHalfPi) {
        x = kPi - x;
    } else if (x < -kHalfPi) {
        x = -kPi - x;
    }
    const float x2 = x * x;
    return x * (1.0F + x2 * (-1.0F / 6.0F +
                             x2 * (1.0F / 120.0F +
                                   x2 * (-1.0F / 5040.0F + x2 * (1.0F / 362880.0F + x2 * (-1.0F / 39916800.0F))))));
}

[[nodiscard]] inline float Cos(float radians) { return Sin(radians + kHalfPi); }

namespace detail {
// atan on [0, 1] via a degree-11 odd polynomial.
[[nodiscard]] inline float AtanUnit(float x) {
    const float x2 = x * x;
    return x * (0.99997726F +
                x2 * (-0.33262347F + x2 * (0.19354346F + x2 * (-0.11643287F + x2 * (0.05265332F - x2 * 0.01172120F)))));
}
}  // namespace detail

// Extended with atan(x) = pi/2 - atan(1/x) outside [-1, 1].
[[nodiscard]] inline float Atan(float value) {
    const bool negative = value < 0.0F;
    float x = negative ? -value : value;
    const bool inverted = x > 1.0F;
    if (inverted) {
        x = 1.0F / x;
    }
    float result = detail::AtanUnit(x);
    if (inverted) {
        result = kHalfPi - result;
    }
    return negative ? -result : result;
}

// Four-quadrant arctangent; (0, 0) yields 0.
[[nodiscard]] inline float Atan2(float y, float x) {
    const float ax = Abs(x);
    const float ay = Abs(y);
    const float larger = ax > ay ? ax : ay;
    if (larger == 0.0F) {
        return 0.0F;
    }
    float angle = detail::AtanUnit((ax < ay ? ax : ay) / larger);
    if (ay > ax) {
        angle = kHalfPi - angle;
    }
    if (x < 0.0F) {
        angle = kPi - angle;
    }
    return y < 0.0F ? -angle : angle;
}

// Moves `angle` towards `target` by at most `step` along the shorter arc.
[[nodiscard]] inline float ApproachAngle(float angle, float target, float step) {
    const float delta = WrapAngle(target - angle);
    if (Abs(delta) <= step) {
        return target;
    }
    return WrapAngle(angle + (delta > 0.0F ? step : -step));
}

}  // namespace micropixel::math

#endif
