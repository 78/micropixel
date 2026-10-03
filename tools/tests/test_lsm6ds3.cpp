// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "platform/drivers/sensors/lsm6ds3.hpp"

namespace {
std::array<uint8_t, 256> registers{};
uint16_t present_address = 0x6bU;
bool fail_transfer{};
unsigned attached{}, removed{};
}  // namespace

esp_err_t i2c_master_probe(void*, uint16_t address, int timeout) {
    assert(timeout == 10);
    return address == present_address ? ESP_OK : ESP_ERR_NOT_FOUND;
}
esp_err_t i2c_master_bus_add_device(void*, const i2c_device_config_t* config, void** out) {
    assert(config->device_address == present_address && config->scl_speed_hz == 400000U);
    ++attached;
    *out = registers.data();
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(void*) {
    ++removed;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(void*, const uint8_t* bytes, size_t length, int timeout) {
    assert(length == 2U && timeout == 10);
    if (fail_transfer) return ESP_FAIL;
    registers[bytes[0]] = bytes[1];
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(void*, const uint8_t* address, size_t length, uint8_t* bytes, size_t count,
                                      int timeout) {
    assert(length == 1U && timeout == 10);
    if (fail_transfer) return ESP_FAIL;
    std::memcpy(bytes, registers.data() + *address, count);
    return ESP_OK;
}

int main() {
    using micropixel::platform::drivers::Lsm6ds3;
    using Kind = Lsm6ds3::Kind;
    for (const uint8_t identity : {0x69U, 0x6aU}) {
        registers.fill(0U);
        registers[0x0f] = identity;
        {
            Lsm6ds3 sensor;
            assert(sensor.Initialize(nullptr) == ESP_ERR_INVALID_ARG);
            assert(sensor.Initialize(registers.data()) == ESP_OK);
            assert(registers[0x12] == 0x44U);
            assert(registers[0x10] == 0U && registers[0x11] == 0U);
            assert(sensor.Configure(Kind::kAcceleration, 2404U) == ESP_OK);
            assert(registers[0x10] == 0x60U);
            assert(sensor.Configure(Kind::kAngularVelocity, 10000U) == ESP_OK);
            assert(registers[0x11] == 0x40U);
            assert(sensor.Suspend(Kind::kAcceleration) == ESP_OK);
            assert(registers[0x10] == 0U && registers[0x11] == 0x40U);
            assert(sensor.Configure(Kind::kAcceleration, 2000U) == ESP_ERR_INVALID_ARG);
            registers[0x28] = 0x08U;
            registers[0x29] = 0x40U;  // 16392 counts, approximately 1 g.
            registers[0x2b] = 0x80U;  // -32768 counts.
            float values[3]{};
            assert(sensor.Read(Kind::kAcceleration, values) == ESP_OK);
            assert(std::abs(values[0] - 9.80665F) < 0.01F && values[1] < -19.0F);
            registers[0x22] = 0xe8U;
            registers[0x23] = 0x03U;  // 1000 counts, 8.75 dps.
            assert(sensor.Read(Kind::kAngularVelocity, values) == ESP_OK);
            assert(std::abs(values[0] - 0.1527163F) < 0.000001F);
            fail_transfer = true;
            values[0] = 42.0F;
            assert(sensor.Read(Kind::kAcceleration, values) == ESP_FAIL && values[0] == 42.0F);
            fail_transfer = false;
        }
        assert(attached == removed);
    }
    registers[0x0f] = 0xffU;
    Lsm6ds3 invalid;
    assert(invalid.Initialize(registers.data()) == ESP_ERR_NOT_FOUND && !invalid.available());
    assert(attached == removed);
    registers[0x0f] = 0x6aU;
    fail_transfer = true;
    assert(invalid.Initialize(registers.data()) != ESP_OK && !invalid.available());
    assert(attached == removed);
    std::puts("LSM6DS3 tests passed");
}
