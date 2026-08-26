#ifndef MICROPIXEL_DEVICE_GPIO_HPP
#define MICROPIXEL_DEVICE_GPIO_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

using GpioEdgeSink = void (*)(void* context, micropixel_device_id_t device, bool value, uint64_t timestamp_us);

class GpioBackend {
   public:
    virtual ~GpioBackend() = default;
    GpioBackend(const GpioBackend&) = delete;
    GpioBackend& operator=(const GpioBackend&) = delete;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_device_id_t device, micropixel_gpio_info_t& info_out) const = 0;
    [[nodiscard]] virtual int32_t Open(micropixel_device_id_t device, uint16_t mode, uint16_t pull, uint16_t edge,
                                       uint32_t initial_value, uint32_t pwm_frequency_hz, GpioEdgeSink edge_sink,
                                       void* edge_context) = 0;
    [[nodiscard]] virtual int32_t Read(micropixel_device_id_t device, bool& value_out) const = 0;
    [[nodiscard]] virtual int32_t Write(micropixel_device_id_t device, bool value) = 0;
    [[nodiscard]] virtual int32_t SetPwmDuty(micropixel_device_id_t device, uint16_t duty_per_mille) = 0;
    virtual void SuspendEvents() = 0;
    [[nodiscard]] virtual int32_t ResumeEvents() = 0;
    virtual void Close(micropixel_device_id_t device) = 0;

   protected:
    GpioBackend() = default;
};

}  // namespace micropixel::device

#endif
