#ifndef MICROPIXEL_SDK_SENSORS_HPP
#define MICROPIXEL_SDK_SENSORS_HPP

#include <stdint.h>

#include <utility>

#include "sdk/result.hpp"
#include "sdk/sensor_types.hpp"

namespace micropixel {

class Application;

namespace detail {

struct SensorOpenResult final {
    uint32_t handle{};
    DeviceId device{};
    SensorKind kind{SensorKind::kAcceleration};
};

struct SensorReadResult final {
    uint64_t timestamp_us{};
    float values[4]{};
};

[[nodiscard]] Result<SensorOpenResult> OpenSensor(DeviceId device, SensorKind expected_kind);
[[nodiscard]] Result<SensorReadResult> ReadSensor(uint32_t handle, SensorKind expected_kind);
[[nodiscard]] Result<Duration> SetSensorSampleInterval(uint32_t handle, Duration interval);
void ReleaseSensor(uint32_t handle);

}  // namespace detail

template <typename Reading>
class Sensor final {
   public:
    Sensor() = default;
    Sensor(const Sensor&) = delete;
    Sensor& operator=(const Sensor&) = delete;
    Sensor(Sensor&& other) noexcept : handle_(other.handle_), device_(other.device_) { other.handle_ = 0U; }
    Sensor& operator=(Sensor&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            device_ = other.device_;
            other.handle_ = 0U;
        }
        return *this;
    }
    ~Sensor() { Reset(); }

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr DeviceId id() const { return device_; }
    [[nodiscard]] Result<SensorSample<Reading>> Read() const {
        auto raw = detail::ReadSensor(handle_, SensorTraits<Reading>::kKind);
        if (!raw) {
            return unexpected(raw.error());
        }
        return SensorSample<Reading>{TimePoint{raw->timestamp_us}, SensorTraits<Reading>::FromValues(raw->values)};
    }
    [[nodiscard]] Result<Duration> SetSampleInterval(Duration interval) const {
        return detail::SetSensorSampleInterval(handle_, interval);
    }
    void Reset() {
        if (handle_ != 0U) {
            detail::ReleaseSensor(handle_);
            handle_ = 0U;
            device_ = DeviceId{};
        }
    }

   private:
    constexpr Sensor(uint32_t handle, DeviceId device) : handle_(handle), device_(device) {}
    uint32_t handle_{};
    DeviceId device_{};

    friend class Sensors;
};

using Accelerometer = Sensor<Acceleration>;
using Magnetometer = Sensor<MagneticField>;

class Sensors final {
   public:
    [[nodiscard]] Result<SensorInfo> GetInfo(DeviceId device) const;

    template <typename Reading>
    [[nodiscard]] Result<Sensor<Reading>> Open(DeviceId device) const {
        auto opened = detail::OpenSensor(device, SensorTraits<Reading>::kKind);
        if (!opened) {
            return unexpected(opened.error());
        }
        return Sensor<Reading>{opened->handle, opened->device};
    }

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Sensors(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
