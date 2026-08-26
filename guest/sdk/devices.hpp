#ifndef MICROPIXEL_SDK_DEVICES_HPP
#define MICROPIXEL_SDK_DEVICES_HPP

#include <stdint.h>

#include <array>

#include "sdk/fixed_string.hpp"
#include "sdk/result.hpp"

namespace micropixel {

class Application;

class DeviceId final {
   public:
    constexpr DeviceId() = default;
    explicit constexpr DeviceId(uint32_t value) : value_(value) {}

    [[nodiscard]] constexpr uint32_t value() const { return value_; }
    [[nodiscard]] constexpr bool valid() const { return value_ != 0U; }
    friend constexpr bool operator==(DeviceId, DeviceId) = default;

   private:
    uint32_t value_{};
};

enum class DeviceKind : uint16_t {
    kAny = 0,
    kDisplay = 1,
    kTouch = 2,
    kAudioInput = 3,
    kAudioOutput = 4,
    kSensor = 5,
    kGpioLine = 6,
    kHaptics = 7,
    kPower = 8,
    kGamepad = 9,
    kCamera = 10,
    kLocation = 11,
    kStorage = 12,
    kNetwork = 13,
};

enum class DeviceCapability : uint64_t {
    kRead = 1ULL << 0U,
    kWrite = 1ULL << 1U,
    kEvents = 1ULL << 2U,
    kHotpluggable = 1ULL << 3U,
};

struct DeviceInfo final {
    DeviceId id{};
    DeviceId parent{};
    DeviceKind kind{DeviceKind::kAny};
    uint64_t capabilities{};
    FixedString<40U> name{};

    [[nodiscard]] constexpr bool Supports(DeviceCapability capability) const {
        return (capabilities & static_cast<uint64_t>(capability)) != 0U;
    }
};

class DeviceList final {
   public:
    static constexpr uint32_t kCapacity = 64U;

    [[nodiscard]] constexpr uint32_t size() const { return count_; }
    [[nodiscard]] constexpr bool empty() const { return count_ == 0U; }
    [[nodiscard]] constexpr uint32_t generation() const { return generation_; }
    [[nodiscard]] constexpr DeviceId operator[](uint32_t index) const {
        return index < count_ ? devices_[index] : DeviceId{};
    }
    [[nodiscard]] constexpr const DeviceId* begin() const { return devices_.data(); }
    [[nodiscard]] constexpr const DeviceId* end() const { return devices_.data() + count_; }

   private:
    std::array<DeviceId, kCapacity> devices_{};
    uint32_t count_{};
    uint32_t generation_{};

    friend class Devices;
};

class Devices final {
   public:
    [[nodiscard]] Result<DeviceList> List(DeviceKind kind = DeviceKind::kAny) const;
    [[nodiscard]] Result<DeviceInfo> GetInfo(DeviceId device) const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Devices(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
