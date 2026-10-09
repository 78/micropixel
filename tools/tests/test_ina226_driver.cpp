// SPDX-License-Identifier: Apache-2.0
//
// INA226 driver regression: the configuration register must select continuous
// shunt+bus conversion (the reviewed revision powered the chip down with
// 0x2938), the identification registers are advisory, and the backoff/cooldown
// state machine keeps working when the chip never answers or answers late.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "esp_timer.h"
#include "fake_i2c_bus.hpp"
#include "platform/drivers/power/ina226.hpp"

namespace {

using micropixel::platform::drivers::Ina226;
using micropixel::platform::drivers::Ina226Config;
using micropixel::platform::drivers::Ina226Sample;
using micropixel::test::FakeDevice;
using micropixel::test::FakeI2cDevice;

constexpr uint16_t kAddress = 0x41U;
constexpr uint16_t kSpareAddress = 0x42U;
constexpr uint16_t kAbsentAddress = 0x43U;
constexpr uint8_t kRegisterConfiguration = 0x00U;
constexpr uint8_t kRegisterBusVoltage = 0x02U;
constexpr uint8_t kRegisterPower = 0x03U;
constexpr uint8_t kRegisterCurrent = 0x04U;
constexpr uint8_t kRegisterCalibration = 0x05U;
constexpr uint8_t kRegisterManufacturer = 0xFEU;
constexpr uint8_t kRegisterDie = 0xFFU;
// 0.00512 / (250 uA * 5 mOhm), the vendor BSP's calibration constant.
constexpr uint16_t kExpectedCalibration = 4096U;
void* const kBus = reinterpret_cast<void*>(1);

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void CheckClose(float value, float expected, const char* message) {
    if (std::fabs(value - expected) > 1e-4F) {
        std::fprintf(stderr, "FAIL: %s (got %.6f, expected %.6f)\n", message, static_cast<double>(value),
                     static_cast<double>(expected));
        std::exit(1);
    }
}

FakeI2cDevice& InstallDevice(uint16_t address, uint16_t manufacturer, uint16_t die) {
    FakeI2cDevice& device = FakeDevice(address);
    device.word_registers = true;
    micropixel::test::FakeSetRegister16(address, kRegisterManufacturer, manufacturer);
    micropixel::test::FakeSetRegister16(address, kRegisterDie, die);
    return device;
}

void ConfigurationSelectsContinuousConversion() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAddress, 0x5449U, 0x2260U);
    Ina226 monitor;
    Check(monitor.Bind(kBus, Ina226Config{}) == ESP_OK, "bind the monitor");
    Ina226Sample sample{};
    Check(monitor.Read(sample, 0), "first read configures the chip");
    Check(device.writes_to(kRegisterConfiguration) == 1U, "exactly one configuration write");
    // AVG[11:9] = 16 averages, VBUSCT[8:6] = VSHCT[5:3] = 1.1 ms, MODE[2:0] = 111.
    Check(micropixel::test::FakeRegister16(kAddress, kRegisterConfiguration) == 0x4527U,
          "configuration selects 16 averages, 1.1 ms conversions and shunt+bus continuous mode");
    Check((micropixel::test::FakeRegister16(kAddress, kRegisterConfiguration) & 0x0007U) == 0x0007U,
          "MODE[2:0] is not power-down");
    Check(micropixel::test::FakeRegister16(kAddress, kRegisterCalibration) == kExpectedCalibration,
          "calibration register matches the shunt and current LSB");
    Check(device.read_count >= 3, "identification and read-back use the bus");
}

void ReadingsUseTheConfiguredScales() {
    micropixel::test::FakeClearBus();
    InstallDevice(kAddress, 0x5449U, 0x2261U);  // The die ID ends in a revision bit.
    micropixel::test::FakeSetRegister16(kAddress, kRegisterBusVoltage, 8000U);
    micropixel::test::FakeSetRegister16(kAddress, kRegisterCurrent, 4000U);
    micropixel::test::FakeSetRegister16(kAddress, kRegisterPower, 1600U);
    Ina226 monitor;
    Check(monitor.Bind(kBus, Ina226Config{}) == ESP_OK, "bind the monitor");
    Ina226Sample sample{};
    Check(monitor.Read(sample, 0), "read the installed device");
    CheckClose(sample.bus_voltage_volts, 10.0F, "bus voltage uses the 1.25 mV LSB");
    CheckClose(sample.shunt_current_amps, 1.0F, "current uses the 250 uA LSB");
    CheckClose(sample.bus_power_watts, 10.0F, "power uses the 25x current LSB");

    // Discharging: the current register is a signed value.
    micropixel::test::FakeSetRegister16(kAddress, kRegisterCurrent, 0xFF38U);
    Check(monitor.Read(sample, 2'000'000), "read a negative current");
    CheckClose(sample.shunt_current_amps, -0.05F, "discharge current stays signed");
}

void ConfigurationReadBackMismatchOnlyWarns() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAddress, 0x5449U, 0x2260U);
    // The chip forces reserved bit 14 and clears reserved bit 13, so the value
    // read back differs from the value written. That must not drop the device.
    device.word_read_override_set[0x00U] = true;
    device.word_read_override[0x00U] = 0x4938U;
    Ina226 monitor;
    Check(monitor.Bind(kBus, Ina226Config{}) == ESP_OK, "bind the monitor");
    Ina226Sample sample{};
    Check(monitor.Read(sample, 0), "read-back mismatch keeps the device");
    Check(monitor.available(), "device stays bound after a read-back mismatch");
    Check(device.writes_to(kRegisterConfiguration) == 1U, "configuration is not rewritten");
}

void UnexpectedIdentificationOnlyWarns() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kSpareAddress, 0x0000U, 0x1234U);
    Ina226Config config{};
    config.address = kSpareAddress;
    Ina226 monitor;
    Check(monitor.Bind(kBus, config) == ESP_OK, "bind an unidentified device");
    Ina226Sample sample{};
    Check(monitor.Read(sample, 0), "unidentified device still gets configured");
    Check(device.writes_to(kRegisterConfiguration) == 1U, "configuration written anyway");
}

void AbsentDeviceBacksOffAndRecovers() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = FakeDevice(kAbsentAddress);
    device.present = false;
    device.word_registers = true;
    Ina226Config config{};
    config.address = kAbsentAddress;
    Ina226 monitor;
    Check(monitor.Bind(kBus, config) == ESP_OK, "bind an absent device");
    Ina226Sample sample{};
    fake_esp_timer_now_us = 0;
    Check(!monitor.Read(sample, 0), "absent device reports no reading");
    Check(device.probe_count == 1, "absent device is probed once");
    fake_esp_timer_now_us = 500'000;  // Inside the 1 s probe backoff.
    Check(!monitor.Read(sample, 500'000), "backoff keeps reporting no reading");
    Check(device.probe_count == 1, "no probe inside the backoff window");
    fake_esp_timer_now_us = 1'000'000;
    Check(!monitor.Read(sample, 1'000'000), "absent device is still absent");
    Check(device.probe_count == 2, "probing resumes once the backoff elapses");
    device.present = true;
    micropixel::test::FakeSetRegister16(kAbsentAddress, kRegisterManufacturer, 0x5449U);
    micropixel::test::FakeSetRegister16(kAbsentAddress, kRegisterDie, 0x2260U);
    micropixel::test::FakeSetRegister16(kAbsentAddress, kRegisterBusVoltage, 8000U);
    fake_esp_timer_now_us = 2'000'000;
    Check(monitor.Read(sample, 2'000'000), "device that appears later is picked up");
    CheckClose(sample.bus_voltage_volts, 10.0F, "late device reports real readings");
}

void FailedRegisterReadDropsAndRecovers() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAddress, 0x5449U, 0x2260U);
    micropixel::test::FakeSetRegister16(kAddress, kRegisterBusVoltage, 3200U);
    device.read_failures = 1;
    Ina226 monitor;
    Check(monitor.Bind(kBus, Ina226Config{}) == ESP_OK, "bind the monitor");
    Ina226Sample sample{};
    fake_esp_timer_now_us = 0;
    Check(monitor.Read(sample, 0), "the first sample configures the chip");
    Check(device.writes_to(kRegisterConfiguration) == 1U, "the chip was configured before the failing read");
    device.read_failures = 1;  // The next register read fails.
    Check(!monitor.Read(sample, 0), "a dropped register read fails the sample");
    const int reads_after_failure = device.read_count;
    fake_esp_timer_now_us = 500'000;  // Inside the retry cooldown.
    Check(!monitor.Read(sample, 500'000), "cooldown refuses the retry");
    Check(device.read_count == reads_after_failure, "cooldown produces no bus traffic");
    fake_esp_timer_now_us = 2'000'000;
    Check(monitor.Read(sample, 2'000'000), "the monitor recovers after the cooldown");
    CheckClose(sample.bus_voltage_volts, 4.0F, "recovered reading uses the bus LSB");
}

}  // namespace

int main() {
    ConfigurationSelectsContinuousConversion();
    ReadingsUseTheConfiguredScales();
    ConfigurationReadBackMismatchOnlyWarns();
    UnexpectedIdentificationOnlyWarns();
    AbsentDeviceBacksOffAndRecovers();
    FailedRegisterReadDropsAndRecovers();
    std::printf("ina226 driver tests passed\n");
    return 0;
}
