#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_BATTERY_BACKEND_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_BATTERY_BACKEND_HPP

#include <array>
#include <cstdint>

#include "device/battery.hpp"
#include "driver/i2c_master.h"

namespace micropixel::platform::metalio_claw4 {

class BatteryBackend final : public device::BatteryBackend {
   public:
    BatteryBackend() = default;

    void Initialize(i2c_master_bus_handle_t bus);
    [[nodiscard]] device::BatterySnapshot Snapshot() override;

   private:
    [[nodiscard]] bool Attach();
    [[nodiscard]] bool ReadRegister(uint8_t address, uint16_t& value);
    [[nodiscard]] uint8_t Filter(uint8_t sample);

    static constexpr uint32_t kFilterCapacity = 60U;

    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t device_{};
    std::array<uint8_t, kFilterCapacity> filter_{};
    uint32_t filter_sum_{};
    uint32_t filter_index_{};
    int64_t next_probe_us_{};
    int64_t read_cooldown_until_us_{};
    uint32_t consecutive_read_failures_{};
    uint8_t last_percent_{};
    bool filter_primed_{};
    bool sample_available_{};
    bool probe_warning_logged_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
