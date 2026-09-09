#ifndef MICROPIXEL_APPS_TOMB_EXPLORER_GAME_MATH_HPP
#define MICROPIXEL_APPS_TOMB_EXPLORER_GAME_MATH_HPP

// Freestanding float helpers: the Guest links no libm.

namespace tomb::math {

constexpr float kPi = 3.14159265358979F;
constexpr float kTwoPi = 6.28318530717959F;
constexpr float kHalfPi = 1.57079632679490F;

[[nodiscard]] inline float Sqrt(float value) { return value > 0.0F ? __builtin_sqrtf(value) : 0.0F; }
[[nodiscard]] inline float Fabs(float value) { return __builtin_fabsf(value); }
[[nodiscard]] inline float Floor(float value) { return __builtin_floorf(value); }

[[nodiscard]] inline float Clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

// Wraps to [-pi, pi).
[[nodiscard]] inline float WrapAngle(float radians) { return radians - kTwoPi * Floor((radians + kPi) / kTwoPi); }

// Odd polynomial on [-pi/2, pi/2] after range reduction; error below 1e-6.
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

// atan2 accurate to about 1e-4 rad; (0, 0) yields 0.
[[nodiscard]] inline float Atan2(float y, float x) {
    const float ax = Fabs(x);
    const float ay = Fabs(y);
    const float larger = ax > ay ? ax : ay;
    if (larger == 0.0F) return 0.0F;
    const float ratio = (ax < ay ? ax : ay) / larger;
    const float r2 = ratio * ratio;
    float angle =
        ratio * (0.99997726F + r2 * (-0.33262347F +
                                     r2 * (0.19354346F + r2 * (-0.11643287F + r2 * (0.05265332F - r2 * 0.01172120F)))));
    if (ay > ax) angle = kHalfPi - angle;
    if (x < 0.0F) angle = kPi - angle;
    return y < 0.0F ? -angle : angle;
}

// Moves `angle` towards `target` by at most `step` along the shorter arc.
[[nodiscard]] inline float ApproachAngle(float angle, float target, float step) {
    const float delta = WrapAngle(target - angle);
    if (Fabs(delta) <= step) return target;
    return WrapAngle(angle + (delta > 0.0F ? step : -step));
}

}  // namespace tomb::math

#endif
