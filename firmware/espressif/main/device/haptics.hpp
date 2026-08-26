#ifndef MICROPIXEL_DEVICE_HAPTICS_HPP
#define MICROPIXEL_DEVICE_HAPTICS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

using HapticCompletionSink = void (*)(void* context, micropixel_device_id_t device, uint64_t timestamp_us);

class HapticsBackend {
   public:
    virtual ~HapticsBackend() = default;
    HapticsBackend(const HapticsBackend&) = delete;
    HapticsBackend& operator=(const HapticsBackend&) = delete;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_device_id_t device, micropixel_haptics_info_t& info_out) const = 0;
    [[nodiscard]] virtual int32_t Play(micropixel_device_id_t device, uint16_t strength_per_mille,
                                       uint32_t duration_ms) = 0;
    [[nodiscard]] virtual int32_t Stop(micropixel_device_id_t device) = 0;
    virtual void SetCompletionSink(HapticCompletionSink sink, void* context) = 0;

   protected:
    HapticsBackend() = default;
};

}  // namespace micropixel::device

#endif
