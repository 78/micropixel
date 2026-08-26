#ifndef MICROPIXEL_DEVICE_SENSORS_HPP
#define MICROPIXEL_DEVICE_SENSORS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

struct SensorValues final {
    // Platform-global monotonic timestamp captured when the cache was updated.
    uint64_t timestamp_us{};
    float values[4]{};
};

class SensorBackend {
   public:
    virtual ~SensorBackend() = default;
    SensorBackend(const SensorBackend&) = delete;
    SensorBackend& operator=(const SensorBackend&) = delete;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info_out) const = 0;
    // Start/Stop control Platform-owned asynchronous sampling. Read only copies
    // the latest cache and may return WOULD_BLOCK before the first sample.
    [[nodiscard]] virtual int32_t Start(micropixel_device_id_t device, uint32_t interval_us) = 0;
    [[nodiscard]] virtual int32_t Read(micropixel_device_id_t device, SensorValues& values_out) = 0;
    virtual void Stop(micropixel_device_id_t device) = 0;

   protected:
    SensorBackend() = default;
};

}  // namespace micropixel::device

#endif
