#ifndef MICROPIXEL_DEVICE_SENSORS_HPP
#define MICROPIXEL_DEVICE_SENSORS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/contracts/peripheral_channel.hpp"

namespace micropixel::device {

struct SensorValues final {
    // Platform-global monotonic timestamp captured when the cache was updated.
    uint64_t timestamp_us{};
    float values[4]{};
};

class Sensors {
   public:
    virtual ~Sensors() = default;
    Sensors(const Sensors&) = delete;
    Sensors& operator=(const Sensors&) = delete;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info_out) const = 0;
    // Start/Stop control Platform-owned asynchronous sampling. Read only copies
    // the latest cache and may return WOULD_BLOCK before the first sample.
    [[nodiscard]] virtual int32_t Start(micropixel_device_id_t device, uint32_t interval_us) = 0;
    [[nodiscard]] virtual int32_t Read(micropixel_device_id_t device, SensorValues& values_out) = 0;
    virtual void StopSampling(micropixel_device_id_t device) = 0;

   protected:
    Sensors() = default;
};

// Hardware-facing sensor boundary. Boards implement this interface using
// local channels; DeviceRegistry owns public identity and ABI routing.
class SensorPeripheral {
   public:
    virtual ~SensorPeripheral() = default;
    SensorPeripheral(const SensorPeripheral&) = delete;
    SensorPeripheral& operator=(const SensorPeripheral&) = delete;

    [[nodiscard]] virtual int32_t GetInfo(PeripheralChannelId channel, micropixel_sensor_info_t& info_out) const = 0;
    [[nodiscard]] virtual int32_t Start(PeripheralChannelId channel, uint32_t interval_us) = 0;
    [[nodiscard]] virtual int32_t Read(PeripheralChannelId channel, SensorValues& values_out) = 0;
    virtual void Stop(PeripheralChannelId channel) = 0;

   protected:
    SensorPeripheral() = default;
};

}  // namespace micropixel::device

#endif
