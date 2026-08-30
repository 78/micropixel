#pragma once

#include <cstdint>
#include <span>

#include "driver/i2c_master.h"
#include "platform/drivers/power/bq27220_profile.hpp"

namespace micropixel::platform::drivers {

struct Bq27220Sample final {
    uint16_t voltage_mv{};
    uint16_t battery_status{};
    int16_t current_ma{};
    bool full_charged{};
};

// Bounded fuel-gauge driver shared by board battery policies. It owns probing,
// the I2C device handle, optional board-supplied profile configuration and
// failure backoff; percentage/filter and external-power semantics stay with
// the Board.
class Bq27220 final {
   public:
    void Bind(i2c_master_bus_handle_t bus, const Bq27220Profile* profile = nullptr) {
        bus_ = bus;
        profile_ = profile;
    }
    [[nodiscard]] bool Read(Bq27220Sample& sample, int64_t now_us);
    [[nodiscard]] bool ReadStateOfCharge(uint8_t& percent, int64_t now_us);
    [[nodiscard]] bool available() const { return device_ != nullptr; }

   private:
    [[nodiscard]] bool Prepare(int64_t now_us);
    [[nodiscard]] bool Attach(int64_t now_us);
    [[nodiscard]] bool ConfigureProfile(int64_t now_us);
    [[nodiscard]] bool ProfileMatches(int64_t now_us);
    [[nodiscard]] bool Unseal(int64_t now_us);
    [[nodiscard]] bool Seal(int64_t now_us);
    [[nodiscard]] bool WaitForConfigurationMode(bool enabled, int64_t now_us);
    [[nodiscard]] bool Control(uint16_t command, int64_t now_us);
    [[nodiscard]] bool ReadOperationStatus(uint16_t& status, int64_t now_us);
    [[nodiscard]] bool ReadDataMemory(const Bq27220DataMemoryValue& parameter, uint16_t& value, int64_t now_us);
    [[nodiscard]] bool WriteDataMemory(const Bq27220DataMemoryValue& parameter, int64_t now_us);
    [[nodiscard]] bool ReadRegister(uint8_t address, uint16_t& value, int64_t now_us);
    [[nodiscard]] bool ReadBytes(uint8_t address, std::span<uint8_t> bytes, int64_t now_us);
    [[nodiscard]] bool WriteBytes(uint8_t address, std::span<const uint8_t> bytes, int64_t now_us);
    void RecordFailure(const char* operation, uint8_t address, esp_err_t status, int64_t now_us);
    void RecordSuccess();

    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t device_{};
    const Bq27220Profile* profile_{};
    int64_t next_probe_us_{};
    int64_t next_profile_retry_us_{};
    int64_t read_cooldown_until_us_{};
    uint32_t consecutive_read_failures_{};
    bool profile_configured_{};
    bool probe_warning_logged_{};
};

}  // namespace micropixel::platform::drivers
