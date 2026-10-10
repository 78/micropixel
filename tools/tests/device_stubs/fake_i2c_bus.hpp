// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::test {

// One device per 7-bit address, plus the access log the tests assert on.
//
// Byte-register chips (RX8130CE) use `registers`, where register N is byte N.
// Word-register chips (INA226) use `words`: their consecutive register addresses
// are 16-bit registers, so flattening them into bytes would make register 03h
// overwrite the low half of register 02h.
//
// fake_i2c_bus.cpp implements the ESP-IDF I2C-master calls against this model.
struct FakeI2cWrite final {
    uint8_t address{};
    std::vector<uint8_t> data{};
};

struct FakeI2cDevice final {
    std::array<uint8_t, 256U> registers{};
    std::array<uint16_t, 256U> words{};
    bool word_registers{false};
    // Reads of an overridden register return the override instead, which models
    // a chip that latches bits the driver cannot write back.
    std::array<uint8_t, 256U> read_override_mask{};
    std::array<uint8_t, 256U> read_override_value{};
    std::array<bool, 256U> word_read_override_set{};
    std::array<uint16_t, 256U> word_read_override{};
    bool present{true};
    int probe_failures{};
    int write_failures{};
    int read_failures{};
    int probe_count{};
    int read_count{};
    std::vector<FakeI2cWrite> writes{};

    [[nodiscard]] std::size_t writes_to(uint8_t address) const {
        std::size_t count = 0U;
        for (const FakeI2cWrite& write : writes) {
            if (write.address == address) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] bool wrote_value(uint8_t address, uint8_t value) const {
        for (const FakeI2cWrite& write : writes) {
            if (write.address == address && !write.data.empty() && write.data[0] == value) {
                return true;
            }
        }
        return false;
    }
};

inline std::map<uint16_t, FakeI2cDevice>& FakeI2cDevices() {
    static std::map<uint16_t, FakeI2cDevice> devices;
    return devices;
}

inline FakeI2cDevice& FakeDevice(uint16_t address) {
    return FakeI2cDevices()[address];
}

inline void FakeClearBus() {
    FakeI2cDevices().clear();
}

inline void FakeSetRegister8(uint16_t address, uint8_t reg, uint8_t value) {
    FakeDevice(address).registers[reg] = value;
}

inline uint8_t FakeRegister8(uint16_t address, uint8_t reg) {
    return FakeDevice(address).registers[reg];
}

inline void FakeSetRegister16(uint16_t address, uint8_t reg, uint16_t value) {
    FakeI2cDevice& device = FakeDevice(address);
    if (device.word_registers) {
        device.words[reg] = value;
        return;
    }
    device.registers[reg] = static_cast<uint8_t>(value >> 8U);
    device.registers[static_cast<uint8_t>(reg + 1U)] = static_cast<uint8_t>(value & 0xFFU);
}

inline uint16_t FakeRegister16(uint16_t address, uint8_t reg) {
    const FakeI2cDevice& device = FakeDevice(address);
    if (device.word_registers) {
        return device.words[reg];
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(device.registers[reg]) << 8U) |
                                 device.registers[static_cast<uint8_t>(reg + 1U)]);
}

}  // namespace micropixel::test
