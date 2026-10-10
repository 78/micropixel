// SPDX-License-Identifier: Apache-2.0
//
// Tab5 RTC policy regression for the reviewed defect: the voltage-loss flag used
// to latch forever, so every boot skipped "system clock restored from RTC" even
// though the stored calendar was correct. The policy now clears the flag only
// when the calendar is trustworthy - written from the system clock, or found to
// match it - and never while the system clock itself is unusable.
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "esp_timer.h"
#include "fake_i2c_bus.hpp"
#include "platform/boards/m5stack-tab5/rtc_clock.hpp"
#include "platform/drivers/rtc/rx8130.hpp"
#include "time_shim.hpp"

namespace {

using micropixel::platform::buses::I2cExecutor;
using micropixel::platform::drivers::rx8130::Decode;
using micropixel::platform::drivers::rx8130::Encode;
using micropixel::platform::drivers::rx8130::Fields;
using micropixel::platform::drivers::rx8130::FromEpochSeconds;
using micropixel::platform::drivers::rx8130::kCalendarRegisterSize;
using micropixel::platform::drivers::rx8130::Time;
using micropixel::platform::drivers::rx8130::ToEpochSeconds;
using micropixel::platform::m5stack_tab5::RtcClock;
using micropixel::test::FakeDevice;
using micropixel::test::FakeRegister8;
using micropixel::test::FakeSetRegister8;

constexpr uint16_t kAddress = 0x32U;
constexpr uint8_t kRegisterSecond = 0x10U;
constexpr uint8_t kRegisterFlag = 0x1DU;
constexpr uint8_t kFlagVoltageLoss = 0x02U;
constexpr uint32_t kRefreshIntervalUs = 60U * 1000U * 1000U;
// 2026-10-09 06:00:00 UTC: inside the calendar range the Host trusts.
constexpr std::time_t kPlausibleNow = 1791525600;
void* const kBus = reinterpret_cast<void*>(1);

int set_time_of_day_calls{};
std::time_t last_set_time_of_day{};
std::time_t test_now_seconds{};

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void StoreCalendar(uint16_t address, std::time_t seconds) {
    Time time{};
    Check(FromEpochSeconds(static_cast<int64_t>(seconds), time), "encode the calendar");
    Fields fields{};
    Encode(time, fields);
    const uint8_t raw[kCalendarRegisterSize] = {fields.second, fields.minute, fields.hour, fields.weekday,
                                                fields.day,    fields.month,  fields.year};
    for (uint8_t offset = 0U; offset < kCalendarRegisterSize; ++offset) {
        FakeSetRegister8(address, static_cast<uint8_t>(kRegisterSecond + offset), raw[offset]);
    }
}

int64_t StoredEpoch(uint16_t address) {
    Fields fields{};
    fields.second = FakeRegister8(address, kRegisterSecond);
    fields.minute = FakeRegister8(address, static_cast<uint8_t>(kRegisterSecond + 1U));
    fields.hour = FakeRegister8(address, static_cast<uint8_t>(kRegisterSecond + 2U));
    fields.weekday = FakeRegister8(address, static_cast<uint8_t>(kRegisterSecond + 3U));
    fields.day = FakeRegister8(address, static_cast<uint8_t>(kRegisterSecond + 4U));
    fields.month = FakeRegister8(address, static_cast<uint8_t>(kRegisterSecond + 5U));
    fields.year = FakeRegister8(address, static_cast<uint8_t>(kRegisterSecond + 6U));
    Time time{};
    if (!Decode(fields, time)) {
        return -1;
    }
    return ToEpochSeconds(time);
}

// Fresh chip, fresh clock, fresh host clock.
void Prepare(std::time_t system_now, std::time_t calendar, bool voltage_loss) {
    micropixel::test::FakeClearBus();
    FakeResetEspTimers();
    MicropixelTestResetClock();
    set_time_of_day_calls = 0;
    last_set_time_of_day = 0;
    MicropixelTestSetNow(system_now);
    FakeDevice(kAddress).registers.fill(0U);
    if (calendar != 0) {
        StoreCalendar(kAddress, calendar);
    }
    FakeSetRegister8(kAddress, kRegisterFlag, voltage_loss ? kFlagVoltageLoss : 0x00U);
}

void IntactCalendarSeedsTheSystemClock() {
    Prepare(kPlausibleNow, kPlausibleNow, false);
    I2cExecutor executor;
    RtcClock clock;
    clock.Initialize(kBus, executor);
    Check(set_time_of_day_calls == 1, "an intact calendar seeds the system clock");
    Check(last_set_time_of_day == kPlausibleNow, "the restored time is the stored calendar");
    Check(FakeDevice(kAddress).writes_to(kRegisterFlag) == 0U, "an intact flag is left alone");
    Check(FakeDevice(kAddress).writes_to(kRegisterSecond) == 0U, "a matching calendar is not rewritten");
    Check(fake_esp_timer_last != nullptr && fake_esp_timer_last->period_us == kRefreshIntervalUs,
          "the refresh timer runs every minute");
}

void SupplyDropIsClearedOnceTheCalendarIsConfirmed() {
    Prepare(kPlausibleNow, kPlausibleNow, true);
    I2cExecutor executor;
    RtcClock clock;
    clock.Initialize(kBus, executor);
    Check(set_time_of_day_calls == 0, "a supply drop keeps the stored time untrusted at bring-up");
    Check(FakeDevice(kAddress).writes_to(kRegisterSecond) == 0U, "a matching calendar is not rewritten");
    Check(FakeDevice(kAddress).writes_to(kRegisterFlag) == 1U, "a confirmed calendar clears the flag once");
    Check(FakeRegister8(kAddress, kRegisterFlag) == 0x00U, "the flag reads back clear");
}

void StaleCalendarIsRewrittenAndCleared() {
    Prepare(kPlausibleNow, static_cast<std::time_t>(kPlausibleNow - 600), true);
    I2cExecutor executor;
    RtcClock clock;
    clock.Initialize(kBus, executor);
    Check(FakeDevice(kAddress).writes_to(kRegisterSecond) == 1U, "a stale calendar is rewritten once");
    Check(StoredEpoch(kAddress) == kPlausibleNow, "the rewrite stores the system clock");
    Check(FakeDevice(kAddress).writes_to(kRegisterFlag) == 1U, "the write-back clears the flag");
    Check(set_time_of_day_calls == 0, "the board never writes the untrusted stored time into the Host");
}

void UnusableCalendarIsRewrittenAndCleared() {
    // A calendar the Host cannot trust (year 2000) must be replaced, not adopted.
    Prepare(kPlausibleNow, 946684800, true);
    I2cExecutor executor;
    RtcClock clock;
    clock.Initialize(kBus, executor);
    Check(set_time_of_day_calls == 0, "an unusable calendar is not adopted");
    Check(FakeDevice(kAddress).writes_to(kRegisterSecond) == 1U, "an unusable calendar is rewritten");
    Check(StoredEpoch(kAddress) == kPlausibleNow, "the rewrite stores the system clock");
    Check(FakeDevice(kAddress).writes_to(kRegisterFlag) == 1U, "the write-back clears the flag");
}

void UntrustedClockNeverWritesAndClearsOnTheNextTick() {
    // No SNTP yet and no retained time: the system clock reads 1970.
    Prepare(0, kPlausibleNow, true);
    I2cExecutor executor;
    RtcClock clock;
    clock.Initialize(kBus, executor);
    Check(set_time_of_day_calls == 0, "a supply drop suppresses the restore");
    Check(FakeDevice(kAddress).writes_to(kRegisterSecond) == 0U,
          "an untrusted system clock never overwrites the calendar");
    Check(FakeDevice(kAddress).writes_to(kRegisterFlag) == 0U, "an untrusted system clock never clears the flag");

    FakeFireEspTimer(fake_esp_timer_last);
    executor.Drain();
    Check(executor.jobs.empty(), "the refresh tick posted one job");
    Check(FakeDevice(kAddress).writes_to(kRegisterFlag) == 0U, "the tick also refuses while the clock is untrusted");
    Check(FakeDevice(kAddress).writes_to(kRegisterSecond) == 0U, "the tick leaves the calendar alone");

    // SNTP (or the user) sets the clock; the next tick sizes up the calendar.
    MicropixelTestSetNow(kPlausibleNow);
    FakeFireEspTimer(fake_esp_timer_last);
    executor.Drain();
    Check(FakeDevice(kAddress).writes_to(kRegisterFlag) == 1U, "the tick clears the flag once the clock is trustworthy");
    Check(FakeDevice(kAddress).writes_to(kRegisterSecond) == 0U, "a now-matching calendar is still not rewritten");
}

}  // namespace

// The RTC policy reaches the Host clock through the standard calls; the Host
// side of that contract lives here because the test framework cannot change the
// real one without privileges.
std::time_t MicropixelTestNow() {
    return test_now_seconds;
}

int MicropixelTestSetTimeOfDay(const struct timeval* value) {
    if (value == nullptr) {
        return -1;
    }
    ++set_time_of_day_calls;
    last_set_time_of_day = value->tv_sec;
    test_now_seconds = value->tv_sec;
    return 0;
}

void MicropixelTestSetNow(std::time_t seconds) {
    test_now_seconds = seconds;
}

void MicropixelTestResetClock() {
    test_now_seconds = 0;
    set_time_of_day_calls = 0;
    last_set_time_of_day = 0;
}

int main() {
    IntactCalendarSeedsTheSystemClock();
    SupplyDropIsClearedOnceTheCalendarIsConfirmed();
    StaleCalendarIsRewrittenAndCleared();
    UnusableCalendarIsRewrittenAndCleared();
    UntrustedClockNeverWritesAndClearsOnTheNextTick();
    std::printf("rtc clock policy tests passed\n");
    return 0;
}
