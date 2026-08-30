#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace micropixel::platform::drivers {

enum class Bq27220DataWidth : uint8_t {
    kU8 = 1U,
    kU16 = 2U,
};

struct Bq27220DataMemoryValue final {
    uint16_t address{};
    uint16_t value{};
    Bq27220DataWidth width{Bq27220DataWidth::kU16};
};

struct Bq27220Profile final {
    uint16_t design_capacity_mah{};
    std::span<const Bq27220DataMemoryValue> parameters{};
    std::span<const Bq27220DataMemoryValue> verification_parameters{};
};

[[nodiscard]] constexpr uint8_t Bq27220Checksum(std::span<const uint8_t> bytes) {
    uint8_t sum = 0U;
    for (uint8_t byte : bytes) {
        sum = static_cast<uint8_t>(sum + byte);
    }
    return static_cast<uint8_t>(0xffU - sum);
}

[[nodiscard]] constexpr size_t Bq27220DataWidthBytes(Bq27220DataWidth width) { return static_cast<size_t>(width); }

}  // namespace micropixel::platform::drivers
