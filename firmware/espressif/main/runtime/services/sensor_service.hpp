#ifndef MICROPIXEL_RUNTIME_SERVICES_SENSOR_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_SENSOR_SERVICE_HPP

#include "device/device_services.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "runtime/runtime_limits.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class TimerService;

// Session-owned Sensor handles. Platform owns asynchronous sampling and its
// shared bus executor; this layer validates handles and exposes cached values.
class SensorService final {
   public:
    SensorService(device::SensorsService& sensors, TimerService& clock);
    SensorService(const SensorService&) = delete;
    SensorService& operator=(const SensorService&) = delete;
    ~SensorService();

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return mutex_ != nullptr;
    }
    [[nodiscard]] ServiceResult<micropixel_sensor_open_response_t> Open(micropixel_device_id_t device,
                                                                        uint16_t expected_kind);
    [[nodiscard]] ServiceResult<micropixel_sensor_reading_t> Read(micropixel_sensor_handle_t sensor);
    [[nodiscard]] ServiceResult<void> SetSampleInterval(micropixel_sensor_handle_t sensor, uint64_t interval_us);
    [[nodiscard]] ServiceResult<void> Release(micropixel_sensor_handle_t sensor);
    void Suspend();
    [[nodiscard]] bool Resume();
    void Shutdown();

   private:
    struct Slot final {
        micropixel_device_id_t device{};
        micropixel_sensor_handle_t handle{};
        uint32_t generation{};
        uint32_t interval_us{};
        uint32_t minimum_interval_us{};
        uint32_t maximum_interval_us{};
        uint16_t kind{};
    };

    static constexpr uint32_t kDefaultSampleIntervalUs = 10000U;
    [[nodiscard]] Slot* Find(micropixel_sensor_handle_t handle);
    [[nodiscard]] bool TakeLock();
    void GiveLock();

    device::SensorsService& sensors_;
    TimerService& clock_;
    SemaphoreHandle_t mutex_{};
    Slot slots_[limits::kMaxSensorHandles]{};
    bool suspended_{};
    bool shut_down_{};
};

}  // namespace micropixel::runtime

#endif
