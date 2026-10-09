// SPDX-License-Identifier: Apache-2.0
//
// RX8130CE driver regression: writing a calendar must not touch the
// voltage-loss flag, and clearing that flag must be an explicit step that only
// writes when the chip actually reports a supply drop.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fake_i2c_bus.hpp"
#include "platform/drivers/rtc/rx8130.hpp"

namespace {

using micropixel::platform::drivers::Rx8130;
using micropixel::platform::drivers::Rx8130Config;
using micropixel::platform::drivers::rx8130::Fields;
using micropixel::platform::drivers::rx8130::FromEpochSeconds;
using micropixel::platform::drivers::rx8130::Time;
using micropixel::test::FakeDevice;
using micropixel::test::FakeI2cDevice;

constexpr uint16_t kAddress = 0x32U;
constexpr uint16_t kAbsentAddress = 0x33U;
constexpr uint8_t kRegisterSecond = 0x10U;
constexpr uint8_t kRegisterFlag = 0x1DU;
constexpr uint8_t kRegisterControl0 = 0x1EU;
constexpr uint8_t kRegisterControl1 = 0x1FU;
constexpr uint8_t kFlagVoltageLoss = 0x02U;
constexpr uint8_t kControl0Stop = 0x40U;
constexpr uint8_t kControl1Backup = 0x30U;
void* const kBus = reinterpret_cast<void*>(1);

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

FakeI2cDevice& InstallDevice(uint16_t address) {
    FakeI2cDevice& device = FakeDevice(address);
    device.registers.fill(0U);
    return device;
}

Fields FieldsAt(uint8_t second, uint8_t minute, uint8_t hour, uint8_t weekday, uint8_t day, uint8_t month,
                uint8_t year) {
    Fields fields{};
    fields.second = second;
    fields.minute = minute;
    fields.hour = hour;
    fields.weekday = weekday;
    fields.day = day;
    fields.month = month;
    fields.year = year;
    return fields;
}

// Load registers 10h..16h in the order the driver reads and writes them.
void StoreCalendar(uint16_t address, const Fields& fields) {
    const uint8_t raw[micropixel::platform::drivers::rx8130::kCalendarRegisterSize] = {
        fields.second, fields.minute, fields.hour, fields.weekday, fields.day, fields.month, fields.year};
    for (uint8_t offset = 0U; offset < micropixel::platform::drivers::rx8130::kCalendarRegisterSize; ++offset) {
        micropixel::test::FakeSetRegister8(address, static_cast<uint8_t>(kRegisterSecond + offset), raw[offset]);
    }
}

void ConfigureEnablesBackupCharging() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAddress);
    Rx8130 rtc;
    Check(rtc.Bind(kBus, Rx8130Config{}) == ESP_OK, "bind the RTC");
    Check(rtc.Configure() == ESP_OK, "configure the RTC");
    Check(device.writes_to(kRegisterControl0) == 1U, "control register 0 is cleared once");
    Check(device.wrote_value(kRegisterControl0, 0x00U), "TEST and STOP read zero after configuration");
    Check(device.writes_to(kRegisterControl1) == 1U, "control register 1 is written once");
    Check(micropixel::test::FakeRegister8(kAddress, kRegisterControl1) == kControl1Backup,
          "INIEN and CHGEN enable backup switchover and charging");
    Check(device.writes_to(kRegisterFlag) == 0U, "configuration never touches the flag register");
}

void WriteCalendarNeverTouchesTheVoltageLossFlag() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAddress);
    // The chip reports a supply drop; only an explicit clear may remove it.
    micropixel::test::FakeSetRegister8(kAddress, kRegisterFlag, kFlagVoltageLoss);
    Rx8130 rtc;
    Check(rtc.Bind(kBus, Rx8130Config{}) == ESP_OK, "bind the RTC");
    const Fields fields = FieldsAt(0x56U, 0x34U, 0x12U, 0x04U, 0x09U, 0x10U, 0x26U);
    Check(rtc.WriteCalendar(fields) == ESP_OK, "write the calendar");
    Check(device.writes_to(kRegisterFlag) == 0U, "a calendar write leaves the voltage-loss flag alone");
    Check(micropixel::test::FakeRegister8(kAddress, kRegisterFlag) == kFlagVoltageLoss,
          "the flag survives a calendar write");
    Check(device.writes_to(kRegisterSecond) == 1U, "the calendar is written in one burst");
    Check(device.writes.size() == 3U, "STOP, the burst and the restart");
    Check(device.writes[0].address == kRegisterControl0 && device.writes[0].data[0] == kControl0Stop,
          "the counter is stopped before the burst");
    Check(device.writes[1].address == kRegisterSecond && device.writes[1].data.size() == 7U,
          "seven calendar registers follow");
    const uint8_t expected[7] = {0x56U, 0x34U, 0x12U, 0x04U, 0x09U, 0x10U, 0x26U};
    Check(std::memcmp(device.writes[1].data.data(), expected, sizeof(expected)) == 0,
          "the burst carries the encoded calendar");
    Check(device.writes[2].address == kRegisterControl0 && device.writes[2].data[0] == 0x00U,
          "the counter restarts afterwards");
}

void ClearVoltageLossOnlyWritesWhenTheFlagIsSet() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAddress);
    Rx8130 rtc;
    Check(rtc.Bind(kBus, Rx8130Config{}) == ESP_OK, "bind the RTC");
    Check(!rtc.VoltageLossDetected(), "a clean chip reports no supply drop");
    Check(rtc.ClearVoltageLoss() == ESP_OK, "clearing a clean flag succeeds");
    Check(device.writes.empty(), "clearing a clean flag writes nothing");

    micropixel::test::FakeSetRegister8(kAddress, kRegisterFlag, kFlagVoltageLoss | 0x01U);
    Check(rtc.VoltageLossDetected(), "the flag reports a supply drop");
    Check(rtc.ClearVoltageLoss() == ESP_OK, "clear the reported supply drop");
    Check(device.writes_to(kRegisterFlag) == 1U, "exactly one flag write");
    Check(device.wrote_value(kRegisterFlag, 0x01U), "the flag write clears VLF and keeps the neighbouring bit");
    Check(micropixel::test::FakeRegister8(kAddress, kRegisterFlag) == 0x01U,
          "the neighbouring flag bit is preserved");
    Check(rtc.ClearVoltageLoss() == ESP_OK, "clearing twice succeeds");
    Check(device.writes_to(kRegisterFlag) == 1U, "a cleared flag is not written again");
}

void ReadCalendarRoundTripsAndReportsFailure() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAddress);
    micropixel::test::FakeSetRegister8(kAddress, kRegisterFlag, 0U);
    const Fields fields = FieldsAt(0x07U, 0x25U, 0x08U, 0x05U, 0x09U, 0x10U, 0x26U);
    StoreCalendar(kAddress, fields);
    Rx8130 rtc;
    Check(rtc.Bind(kBus, Rx8130Config{}) == ESP_OK, "bind the RTC");
    Fields read{};
    Check(rtc.ReadCalendar(read) == ESP_OK, "read the calendar");
    Check(std::memcmp(&read, &fields, sizeof(fields)) == 0, "the calendar reads back unchanged");
    Time decoded{};
    Check(micropixel::platform::drivers::rx8130::Decode(read, decoded), "the stored calendar decodes");
    Check(micropixel::platform::drivers::rx8130::ToEpochSeconds(decoded) > 0,
          "the decoded calendar yields an epoch");

    // A busy bus must surface as an error and drop the bound device.
    device.read_failures = 1;
    Check(rtc.ReadCalendar(read) != ESP_OK, "a failed read reports an error");
    Check(!rtc.available(), "a failed read drops the device");
}

void AbsentChipReportsFailure() {
    micropixel::test::FakeClearBus();
    FakeI2cDevice& device = InstallDevice(kAbsentAddress);
    device.present = false;
    Rx8130 rtc;
    Check(rtc.Bind(kBus, Rx8130Config{kAbsentAddress}) == ESP_OK, "bind an absent chip");
    Check(!rtc.VoltageLossDetected(), "an absent chip reports no supply drop");
    Check(rtc.ClearVoltageLoss() != ESP_OK, "clearing on an absent chip reports an error");
    Check(!rtc.available(), "an absent chip stays unbound");
    Check(device.probe_count > 0, "the absent chip was probed");
}

}  // namespace

int main() {
    ConfigureEnablesBackupCharging();
    WriteCalendarNeverTouchesTheVoltageLossFlag();
    ClearVoltageLossOnlyWritesWhenTheFlagIsSet();
    ReadCalendarRoundTripsAndReportsFailure();
    AbsentChipReportsFailure();
    std::printf("rx8130 driver tests passed\n");
    return 0;
}
