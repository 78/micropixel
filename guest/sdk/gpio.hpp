#ifndef MICROPIXEL_SDK_GPIO_HPP
#define MICROPIXEL_SDK_GPIO_HPP

#include <stdint.h>

#include "sdk/devices.hpp"
#include "sdk/event.hpp"
#include "sdk/result.hpp"

namespace micropixel {

class Application;

enum class GpioCapability : uint32_t {
    kInput = 1U << 0U,
    kOutput = 1U << 1U,
    kPullUp = 1U << 2U,
    kPullDown = 1U << 3U,
    kEdgeEvents = 1U << 4U,
    kPwm = 1U << 5U,
};

enum class GpioPull : uint16_t {
    kNone = 0,
    kUp = 1,
    kDown = 2,
};

enum class GpioEdgeTrigger : uint16_t {
    kNone = 0,
    kRising = 1,
    kFalling = 2,
    kBoth = 3,
};

struct GpioInfo final {
    DeviceId id{};
    uint16_t line_number{};
    uint32_t capabilities{};
    uint32_t maximum_pwm_frequency_hz{};

    [[nodiscard]] constexpr bool Supports(GpioCapability capability) const {
        return (capabilities & static_cast<uint32_t>(capability)) != 0U;
    }
};

struct GpioInputOptions final {
    GpioPull pull{GpioPull::kNone};
    GpioEdgeTrigger edge{GpioEdgeTrigger::kNone};
};

namespace detail {

struct GpioOpenResult final {
    uint32_t handle{};
    DeviceId device{};
};

[[nodiscard]] Result<GpioOpenResult> OpenGpio(DeviceId device, uint16_t mode, uint16_t pull, uint16_t edge,
                                              uint32_t initial_value, uint32_t pwm_frequency_hz);
[[nodiscard]] Result<bool> ReadGpio(uint32_t handle);
[[nodiscard]] Result<void> WriteGpio(uint32_t handle, bool value);
[[nodiscard]] Result<void> SetGpioPwmDuty(uint32_t handle, uint16_t duty_per_mille);
void ReleaseGpio(uint32_t handle);

}  // namespace detail

class GpioInput final {
   public:
    GpioInput() = default;
    GpioInput(const GpioInput&) = delete;
    GpioInput& operator=(const GpioInput&) = delete;
    GpioInput(GpioInput&& other) noexcept : handle_(other.handle_), device_(other.device_) { other.handle_ = 0U; }
    GpioInput& operator=(GpioInput&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            device_ = other.device_;
            other.handle_ = 0U;
        }
        return *this;
    }
    ~GpioInput() { Reset(); }

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr DeviceId id() const { return device_; }
    [[nodiscard]] Result<bool> Read() const { return detail::ReadGpio(handle_); }
    void Reset() {
        if (handle_ != 0U) {
            detail::ReleaseGpio(handle_);
            handle_ = 0U;
            device_ = DeviceId{};
        }
    }

   private:
    constexpr GpioInput(uint32_t handle, DeviceId device) : handle_(handle), device_(device) {}
    [[nodiscard]] constexpr bool Matches(const GpioEdgeEvent& event) const {
        return handle_ != 0U && event.source_ == handle_;
    }

    uint32_t handle_{};
    DeviceId device_{};
    friend class Event;
    friend class Gpio;
};

class GpioOutput final {
   public:
    GpioOutput() = default;
    GpioOutput(const GpioOutput&) = delete;
    GpioOutput& operator=(const GpioOutput&) = delete;
    GpioOutput(GpioOutput&& other) noexcept : handle_(other.handle_), device_(other.device_) { other.handle_ = 0U; }
    GpioOutput& operator=(GpioOutput&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            device_ = other.device_;
            other.handle_ = 0U;
        }
        return *this;
    }
    ~GpioOutput() { Reset(); }

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr DeviceId id() const { return device_; }
    [[nodiscard]] Result<bool> Read() const { return detail::ReadGpio(handle_); }
    [[nodiscard]] Result<void> Write(bool value) const { return detail::WriteGpio(handle_, value); }
    void Reset() {
        if (handle_ != 0U) {
            detail::ReleaseGpio(handle_);
            handle_ = 0U;
            device_ = DeviceId{};
        }
    }

   private:
    constexpr GpioOutput(uint32_t handle, DeviceId device) : handle_(handle), device_(device) {}
    uint32_t handle_{};
    DeviceId device_{};
    friend class Gpio;
};

class GpioPwm final {
   public:
    GpioPwm() = default;
    GpioPwm(const GpioPwm&) = delete;
    GpioPwm& operator=(const GpioPwm&) = delete;
    GpioPwm(GpioPwm&& other) noexcept : handle_(other.handle_), device_(other.device_) { other.handle_ = 0U; }
    GpioPwm& operator=(GpioPwm&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            device_ = other.device_;
            other.handle_ = 0U;
        }
        return *this;
    }
    ~GpioPwm() { Reset(); }

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr DeviceId id() const { return device_; }
    [[nodiscard]] Result<void> SetDuty(uint16_t duty_per_mille) const {
        return detail::SetGpioPwmDuty(handle_, duty_per_mille);
    }
    void Reset() {
        if (handle_ != 0U) {
            detail::ReleaseGpio(handle_);
            handle_ = 0U;
            device_ = DeviceId{};
        }
    }

   private:
    constexpr GpioPwm(uint32_t handle, DeviceId device) : handle_(handle), device_(device) {}
    uint32_t handle_{};
    DeviceId device_{};
    friend class Gpio;
};

class Gpio final {
   public:
    [[nodiscard]] Result<GpioInfo> GetInfo(DeviceId device) const;
    [[nodiscard]] Result<GpioInput> OpenInput(DeviceId device, GpioInputOptions options = {}) const;
    [[nodiscard]] Result<GpioOutput> OpenOutput(DeviceId device, bool initial_value = false) const;
    [[nodiscard]] Result<GpioPwm> OpenPwm(DeviceId device, uint32_t frequency_hz,
                                          uint16_t initial_duty_per_mille = 0U) const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };
    explicit constexpr Gpio(CapabilityToken) noexcept {}
    friend class Application;
};

inline const GpioEdgeEvent* Event::EdgeFrom(const GpioInput& source) const {
    const GpioEdgeEvent* candidate = gpio_edge();
    return candidate != nullptr && source.Matches(*candidate) ? candidate : nullptr;
}

}  // namespace micropixel

#endif
