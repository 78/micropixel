#ifndef MICROPIXEL_SDK_HAPTICS_HPP
#define MICROPIXEL_SDK_HAPTICS_HPP

#include <stdint.h>

#include "sdk/devices.hpp"
#include "sdk/event.hpp"
#include "sdk/result.hpp"
#include "sdk/types.hpp"

namespace micropixel {

class Application;

struct HapticsInfo final {
    DeviceId id{};
    Duration maximum_duration{};
    bool variable_strength{};
};

namespace detail {
[[nodiscard]] Result<uint32_t> OpenHaptic(DeviceId device);
[[nodiscard]] Result<void> PlayHaptic(uint32_t handle, Duration duration, uint16_t strength_per_mille);
[[nodiscard]] Result<void> StopHaptic(uint32_t handle);
void ReleaseHaptic(uint32_t handle);
}  // namespace detail

class Haptic final {
   public:
    Haptic() = default;
    Haptic(const Haptic&) = delete;
    Haptic& operator=(const Haptic&) = delete;
    Haptic(Haptic&& other) noexcept : handle_(other.handle_), device_(other.device_) { other.handle_ = 0U; }
    Haptic& operator=(Haptic&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            device_ = other.device_;
            other.handle_ = 0U;
        }
        return *this;
    }
    ~Haptic() { Reset(); }

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr DeviceId id() const { return device_; }
    [[nodiscard]] Result<void> Play(Duration duration, uint16_t strength_per_mille = 1000U) const {
        return detail::PlayHaptic(handle_, duration, strength_per_mille);
    }
    [[nodiscard]] Result<void> Stop() const { return detail::StopHaptic(handle_); }
    void Reset() {
        if (handle_ != 0U) {
            detail::ReleaseHaptic(handle_);
            handle_ = 0U;
            device_ = DeviceId{};
        }
    }

   private:
    constexpr Haptic(uint32_t handle, DeviceId device) : handle_(handle), device_(device) {}
    [[nodiscard]] constexpr bool Matches(const HapticEvent& event) const {
        return handle_ != 0U && event.source_ == handle_;
    }
    uint32_t handle_{};
    DeviceId device_{};
    friend class Event;
    friend class Haptics;
};

class Haptics final {
   public:
    [[nodiscard]] Result<HapticsInfo> GetInfo(DeviceId device) const;
    [[nodiscard]] Result<Haptic> Open(DeviceId device) const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };
    explicit constexpr Haptics(CapabilityToken) noexcept {}
    friend class Application;
};

inline const HapticEvent* Event::HapticFrom(const Haptic& source) const {
    const HapticEvent* candidate = haptic();
    return candidate != nullptr && source.Matches(*candidate) ? candidate : nullptr;
}

}  // namespace micropixel

#endif
