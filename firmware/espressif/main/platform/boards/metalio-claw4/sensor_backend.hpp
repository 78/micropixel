#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_SENSOR_BACKEND_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_SENSOR_BACKEND_HPP

#include <atomic>

#include "device/sensors.hpp"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "platform/common/i2c_executor.hpp"

namespace micropixel::platform::metalio_claw4 {

class SensorBackend final : public device::SensorBackend {
   public:
    ~SensorBackend() override;

    void Initialize(i2c_master_bus_handle_t bus, common::I2cExecutor& i2c_executor);

    [[nodiscard]] bool acceleration_available() const { return acceleration_ != nullptr; }
    [[nodiscard]] bool magnetic_field_available() const { return magnetic_field_ != nullptr; }
    [[nodiscard]] int32_t GetInfo(micropixel_device_id_t device, micropixel_sensor_info_t& info_out) const override;
    [[nodiscard]] int32_t Start(micropixel_device_id_t device, uint32_t interval_us) override;
    [[nodiscard]] int32_t Read(micropixel_device_id_t device, device::SensorValues& values_out) override;
    void Stop(micropixel_device_id_t device) override;

   private:
    struct Sampler final {
        SensorBackend* owner{};
        micropixel_device_id_t device{};
        esp_timer_handle_t timer{};
        device::SensorValues latest{};
        std::atomic<bool> active{};
        std::atomic<bool> pending{};
        int32_t status{MICROPIXEL_STATUS_WOULD_BLOCK};
    };

    void InitializeOnWorker();
    [[nodiscard]] bool AddDevice(uint8_t address, i2c_master_dev_handle_t& device_out);
    [[nodiscard]] bool ReadRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t& value_out) const;
    [[nodiscard]] bool WriteRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t value) const;
    [[nodiscard]] bool ReadRegisters(i2c_master_dev_handle_t device, uint8_t address, uint8_t* bytes,
                                     size_t length) const;
    [[nodiscard]] int32_t ReadAcceleration(device::SensorValues& values_out) const;
    [[nodiscard]] int32_t ReadMagneticField(device::SensorValues& values_out) const;
    [[nodiscard]] int32_t ConfigureOnWorker(micropixel_device_id_t device, uint32_t interval_us);
    void StopOnWorker(micropixel_device_id_t device);
    [[nodiscard]] Sampler* FindSampler(micropixel_device_id_t device);
    [[nodiscard]] const Sampler* FindSampler(micropixel_device_id_t device) const;
    static void TimerExpired(void* context);
    static esp_err_t SampleOnWorker(void* context);

    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t acceleration_{};
    i2c_master_dev_handle_t magnetic_field_{};
    common::I2cExecutor* i2c_executor_{};
    Sampler acceleration_sampler_{};
    Sampler magnetic_field_sampler_{};
    portMUX_TYPE cache_lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace micropixel::platform::metalio_claw4

#endif
