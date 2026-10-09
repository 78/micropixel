// SPDX-License-Identifier: Apache-2.0
//
// ESP-IDF I2C-master calls backed by the fake devices in fake_i2c_bus.hpp. This
// lives in a translation unit of its own: the drivers reference these symbols
// from other objects, and an inline header definition would only be emitted when
// the test itself odr-uses it.
#include "fake_i2c_bus.hpp"

namespace {

// Device handles carry their 7-bit address; the drivers only pass them back.
uint16_t DeviceAddress(i2c_master_dev_handle_t device) {
    return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(device));
}

}  // namespace

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms) {
    (void)bus;
    (void)timeout_ms;
    micropixel::test::FakeI2cDevice& device = micropixel::test::FakeDevice(address);
    ++device.probe_count;
    if (device.probe_failures > 0) {
        --device.probe_failures;
        return ESP_ERR_NOT_FOUND;
    }
    return device.present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t* config,
                                    i2c_master_dev_handle_t* device_out) {
    (void)bus;
    if (config == nullptr || device_out == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *device_out = reinterpret_cast<i2c_master_dev_handle_t>(static_cast<uintptr_t>(config->device_address));
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
    (void)device;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t* payload, size_t size, int timeout_ms) {
    (void)timeout_ms;
    if (device == nullptr || payload == nullptr || size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    micropixel::test::FakeI2cDevice& target = micropixel::test::FakeDevice(DeviceAddress(device));
    if (!target.present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (target.write_failures > 0) {
        --target.write_failures;
        return ESP_FAIL;
    }
    const uint8_t address = payload[0];
    std::vector<uint8_t> data(payload + 1U, payload + size);
    if (target.word_registers) {
        if (data.size() != 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        target.words[address] = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) | data[1]);
    } else {
        for (std::size_t index = 0U; index < data.size(); ++index) {
            const std::size_t reg = static_cast<std::size_t>(address) + index;
            if (reg >= target.registers.size()) {
                return ESP_ERR_INVALID_SIZE;
            }
            target.registers[reg] = data[index];
        }
    }
    target.writes.push_back({address, std::move(data)});
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t* write, size_t write_size,
                                      uint8_t* read, size_t read_size, int timeout_ms) {
    (void)timeout_ms;
    if (device == nullptr || write == nullptr || write_size != 1U || read == nullptr || read_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    micropixel::test::FakeI2cDevice& target = micropixel::test::FakeDevice(DeviceAddress(device));
    if (!target.present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (target.read_failures > 0) {
        --target.read_failures;
        return ESP_FAIL;
    }
    const uint8_t address = write[0];
    if (target.word_registers) {
        if (read_size != 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint16_t value =
            target.word_read_override_set[address] ? target.word_read_override[address] : target.words[address];
        read[0] = static_cast<uint8_t>(value >> 8U);
        read[1] = static_cast<uint8_t>(value & 0xFFU);
    } else {
        for (std::size_t index = 0U; index < read_size; ++index) {
            const std::size_t reg = static_cast<std::size_t>(address) + index;
            if (reg >= target.registers.size()) {
                return ESP_ERR_INVALID_SIZE;
            }
            read[index] =
                (target.read_override_mask[reg] != 0U) ? target.read_override_value[reg] : target.registers[reg];
        }
    }
    ++target.read_count;
    return ESP_OK;
}
