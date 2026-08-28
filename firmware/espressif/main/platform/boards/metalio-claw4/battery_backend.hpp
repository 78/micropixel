#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_BATTERY_BACKEND_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_BATTERY_BACKEND_HPP

#include <array>
#include <atomic>
#include <cstdint>

#include "device/battery.hpp"
#include "driver/i2c_master.h"
#include "platform/common/i2c_executor.hpp"
#include "platform/drivers/power/bq27220.hpp"

namespace micropixel::platform::metalio_claw4 {

class BatteryBackend final : public device::BatteryBackend {
   public:
    BatteryBackend() = default;

    void Initialize(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t io_expander,
                    common::I2cExecutor& i2c_executor);
    [[nodiscard]] device::BatterySnapshot Snapshot() override;
    void SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) override;
    void NotifyExternalPowerChanged();

   private:
    void InitializeOnWorker();
    [[nodiscard]] device::BatterySnapshot RefreshOnWorker();
    [[nodiscard]] bool ConfigurePowerDetectionInputs();
    [[nodiscard]] bool ReadExternalPower(bool& connected);
    [[nodiscard]] uint8_t Filter(uint8_t sample, int64_t now_us);

    static constexpr uint32_t kFilterCapacity = 60U;
    static constexpr int64_t kFilterStepUs = 1000000;

    i2c_master_dev_handle_t io_expander_{};
    common::I2cExecutor* i2c_executor_{};
    drivers::Bq27220 fuel_gauge_{};
    std::array<uint8_t, kFilterCapacity> filter_{};
    uint32_t filter_sum_{};
    uint32_t filter_index_{};
    int64_t filter_last_update_us_{};
    uint32_t consecutive_power_read_failures_{};
    uint8_t last_percent_{};
    bool charging_{};
    bool discharging_{};
    bool charging_available_{};
    bool filter_primed_{};
    bool sample_available_{};
    bool external_power_connected_{};
    bool external_power_available_{};
    std::atomic<device::BatteryStateChangeSink> state_change_sink_{};
    std::atomic<void*> state_change_context_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
