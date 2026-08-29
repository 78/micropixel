#ifndef MICROPIXEL_DEVICE_HAPTICS_HPP
#define MICROPIXEL_DEVICE_HAPTICS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/contracts/peripheral_channel.hpp"

namespace micropixel::device {

using HapticCompletionSink = void (*)(void* context, micropixel_device_id_t device, uint64_t timestamp_us);

class HapticsPeripheral;
using HapticsPeripheralCompletionSink = void (*)(void* context, HapticsPeripheral& peripheral,
                                                 PeripheralChannelId channel, uint64_t timestamp_us);

class Haptics {
   public:
    virtual ~Haptics() = default;
    Haptics(const Haptics&) = delete;
    Haptics& operator=(const Haptics&) = delete;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_device_id_t device, micropixel_haptics_info_t& info_out) const = 0;
    [[nodiscard]] virtual int32_t Play(micropixel_device_id_t device, uint16_t strength_per_mille,
                                       uint32_t duration_ms) = 0;
    [[nodiscard]] virtual int32_t Stop(micropixel_device_id_t device) = 0;
    virtual void SetCompletionSink(HapticCompletionSink sink, void* context) = 0;

   protected:
    Haptics() = default;
};

// Hardware-facing haptics boundary. Public DeviceId assignment and completion
// event translation are performed by DeviceRegistry.
class HapticsPeripheral {
   public:
    virtual ~HapticsPeripheral() = default;
    HapticsPeripheral(const HapticsPeripheral&) = delete;
    HapticsPeripheral& operator=(const HapticsPeripheral&) = delete;

    [[nodiscard]] virtual int32_t GetInfo(PeripheralChannelId channel, micropixel_haptics_info_t& info_out) const = 0;
    [[nodiscard]] virtual int32_t Play(PeripheralChannelId channel, uint16_t strength_per_mille,
                                       uint32_t duration_ms) = 0;
    [[nodiscard]] virtual int32_t Stop(PeripheralChannelId channel) = 0;
    virtual void SetCompletionSink(HapticsPeripheralCompletionSink sink, void* context) = 0;

   protected:
    HapticsPeripheral() = default;
};

}  // namespace micropixel::device

#endif
