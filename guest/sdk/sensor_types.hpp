#ifndef MICROPIXEL_SDK_SENSOR_TYPES_HPP
#define MICROPIXEL_SDK_SENSOR_TYPES_HPP

#include <stdint.h>

#include "sdk/devices.hpp"
#include "sdk/types.hpp"

namespace micropixel {

enum class SensorKind : uint16_t {
    kAcceleration = 1,
    kAngularVelocity = 2,
    kMagneticField = 3,
    kTemperature = 4,
    kIlluminance = 5,
    kPressure = 6,
    kRelativeHumidity = 7,
    kProximity = 8,
    kOrientation = 9,
};

enum class SensorPlacement : uint16_t {
    kUnknown = 0,
    kBuiltIn = 1,
    kExternal = 2,
    kLeft = 3,
    kRight = 4,
};

struct Vector3 final {
    float x{};
    float y{};
    float z{};
};

struct Acceleration final {
    Vector3 meters_per_second_squared{};
};

struct MagneticField final {
    Vector3 microtesla{};
};

template <typename Reading>
struct SensorTraits;

template <>
struct SensorTraits<Acceleration> final {
    static constexpr SensorKind kKind = SensorKind::kAcceleration;
    [[nodiscard]] static constexpr Acceleration FromValues(const float* values) {
        return Acceleration{{values[0], values[1], values[2]}};
    }
};

template <>
struct SensorTraits<MagneticField> final {
    static constexpr SensorKind kKind = SensorKind::kMagneticField;
    [[nodiscard]] static constexpr MagneticField FromValues(const float* values) {
        return MagneticField{{values[0], values[1], values[2]}};
    }
};

template <typename Reading>
struct SensorSample final {
    TimePoint timestamp{};
    Reading value{};
};

struct SensorInfo final {
    DeviceId id{};
    DeviceId parent{};
    SensorKind kind{SensorKind::kAcceleration};
    SensorPlacement placement{SensorPlacement::kUnknown};
    uint16_t value_count{};
    Duration minimum_interval{};
    Duration maximum_interval{};
};

}  // namespace micropixel

#endif
