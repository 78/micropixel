// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

// Register-level conversion for the RX8130CE real-time clock (calendar
// registers 0x10..0x16). This header stays free of ESP-IDF headers so the host
// tests can cover the BCD and calendar arithmetic; the I2C transport lives in
// rx8130.hpp and the system-clock policy in the board's RtcClock.
namespace micropixel::platform::drivers::rx8130 {

// Calendar registers 0x10..0x16 in wire order. The year holds two digits
// (2000..2099), the weekday is Sunday = 0 and hours are 24-hour values
// (register bit 6 = 0).
struct Fields final {
    uint8_t second{};
    uint8_t minute{};
    uint8_t hour{};
    uint8_t weekday{};
    uint8_t day{};
    uint8_t month{};
    uint8_t year{};
};

inline constexpr uint8_t kCalendarRegisterSize = 7U;

// Decoded calendar value. MicroPixel keeps the RTC in UTC, the same basis SNTP
// updates and the Host formats for display.
struct Time final {
    uint16_t year{};
    uint8_t month{};
    uint8_t day{};
    uint8_t hour{};
    uint8_t minute{};
    uint8_t second{};
};

inline constexpr int64_t kSecondsPerMinute = 60;
inline constexpr int64_t kSecondsPerHour = 3600;
inline constexpr int64_t kSecondsPerDay = 86400;

// The chip ships with its backup capacitor discharged, so a first boot reads
// 2000-01-01 or BCD noise. Anything outside this window means "no usable
// time", and the Host then keeps whatever the system clock already holds.
inline constexpr uint16_t kEarliestPlausibleYear = 2024U;
inline constexpr uint16_t kLatestPlausibleYear = 2099U;

// The two-digit year the register can hold.
inline constexpr uint16_t kFirstTwoDigitYear = 2000U;
inline constexpr uint16_t kLastTwoDigitYear = 2099U;

[[nodiscard]] constexpr bool IsBcdByte(uint8_t value) { return (value & 0x0FU) <= 9U && ((value >> 4U) & 0x0FU) <= 9U; }

[[nodiscard]] constexpr uint8_t ToBcd(uint8_t value) {
    return static_cast<uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

[[nodiscard]] constexpr uint8_t FromBcd(uint8_t value) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value >> 4U) & 0x0FU) * 10U + (value & 0x0FU));
}

// Days since 1970-01-01 for a proleptic Gregorian date (days_from_civil).
[[nodiscard]] constexpr int64_t DaysFromCivil(uint16_t year, uint8_t month, uint8_t day) {
    const int32_t adjusted_year = static_cast<int32_t>(year) - (month <= 2U ? 1 : 0);
    const int32_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const uint32_t year_of_era = static_cast<uint32_t>(adjusted_year - era * 400);
    const int32_t month_offset = month > 2U ? -3 : 9;
    const uint32_t day_of_year =
        static_cast<uint32_t>((153 * (static_cast<int32_t>(month) + month_offset) + 2) / 5) + day - 1U;
    const uint32_t day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

// Inverse of DaysFromCivil.
constexpr void CivilFromDays(int64_t days, uint16_t& year, uint8_t& month, uint8_t& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const uint32_t day_of_era = static_cast<uint32_t>(days - era * 146097);
    const uint32_t year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    const int64_t computed_year = static_cast<int64_t>(year_of_era) + era * 400;
    const uint32_t day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const uint32_t month_prime = (5U * day_of_year + 2U) / 153U;
    const int32_t month_adjust = month_prime < 10U ? 3 : -9;
    day = static_cast<uint8_t>(day_of_year - (153U * month_prime + 2U) / 5U + 1U);
    month = static_cast<uint8_t>(static_cast<int32_t>(month_prime) + month_adjust);
    year = static_cast<uint16_t>(computed_year + (month <= 2U ? 1 : 0));
}

// Weekday with Sunday = 0 (the value C's tm_wday and the chip's WEEK register
// use); 1970-01-01 was a Thursday.
[[nodiscard]] constexpr uint8_t WeekdayFromEpochSeconds(int64_t seconds) {
    int64_t days = seconds / kSecondsPerDay;
    if (seconds % kSecondsPerDay < 0) {
        --days;
    }
    int64_t weekday = (days + 4) % 7;
    if (weekday < 0) {
        weekday += 7;
    }
    return static_cast<uint8_t>(weekday);
}

// Calendar ranges, independent of the two-digit year the chip stores.
[[nodiscard]] constexpr bool IsValidCalendar(const Time& time) {
    return time.month >= 1U && time.month <= 12U && time.day >= 1U && time.day <= 31U && time.hour <= 23U &&
           time.minute <= 59U && time.second <= 59U;
}

// Valid calendar inside the window the firmware accepts as "a clock that was
// actually set".
[[nodiscard]] constexpr bool IsPlausible(const Time& time) {
    return time.year >= kEarliestPlausibleYear && time.year <= kLatestPlausibleYear && IsValidCalendar(time);
}

[[nodiscard]] constexpr int64_t ToEpochSeconds(const Time& time) {
    return DaysFromCivil(time.year, time.month, time.day) * kSecondsPerDay +
           static_cast<int64_t>(time.hour) * kSecondsPerHour + static_cast<int64_t>(time.minute) * kSecondsPerMinute +
           time.second;
}

// Splits epoch seconds into a calendar value, rejecting years the chip cannot
// store (2000..2099) so a caller never writes a value it cannot read back.
[[nodiscard]] constexpr bool FromEpochSeconds(int64_t seconds, Time& out) {
    int64_t days = seconds / kSecondsPerDay;
    int64_t seconds_of_day = seconds % kSecondsPerDay;
    if (seconds_of_day < 0) {
        seconds_of_day += kSecondsPerDay;
        --days;
    }
    uint16_t year = 0U;
    uint8_t month = 0U;
    uint8_t day = 0U;
    CivilFromDays(days, year, month, day);
    out = {year,
           month,
           day,
           static_cast<uint8_t>(seconds_of_day / kSecondsPerHour),
           static_cast<uint8_t>((seconds_of_day / kSecondsPerMinute) % kSecondsPerMinute),
           static_cast<uint8_t>(seconds_of_day % kSecondsPerMinute)};
    return year >= kFirstTwoDigitYear && year <= kLastTwoDigitYear;
}

// Masks the reserved bits the registers carry (SEC bit 7, HOUR bits 6-7,
// WEEK bit 7, DAY bits 6-7, MONTH bits 5-7) and rejects BCD noise.
[[nodiscard]] constexpr bool Decode(const Fields& fields, Time& out) {
    const uint8_t second = static_cast<uint8_t>(fields.second & 0x7FU);
    const uint8_t minute = static_cast<uint8_t>(fields.minute & 0x7FU);
    const uint8_t hour = static_cast<uint8_t>(fields.hour & 0x3FU);
    const uint8_t weekday = static_cast<uint8_t>(fields.weekday & 0x07U);
    const uint8_t day = static_cast<uint8_t>(fields.day & 0x3FU);
    const uint8_t month = static_cast<uint8_t>(fields.month & 0x1FU);
    if (!IsBcdByte(second) || !IsBcdByte(minute) || !IsBcdByte(hour) || !IsBcdByte(day) || !IsBcdByte(month) ||
        !IsBcdByte(fields.year) || weekday > 6U) {
        return false;
    }
    const Time decoded{static_cast<uint16_t>(kFirstTwoDigitYear + FromBcd(fields.year)),
                       FromBcd(month),
                       FromBcd(day),
                       FromBcd(hour),
                       FromBcd(minute),
                       FromBcd(second)};
    out = decoded;
    return IsValidCalendar(decoded);
}

// Encodes a calendar value, filling the weekday so the chip's WEEK register
// stays consistent with the date (the firmware itself only uses the date).
constexpr void Encode(const Time& time, Fields& out) {
    out.second = ToBcd(time.second);
    out.minute = ToBcd(time.minute);
    out.hour = ToBcd(time.hour);  // bit 6 = 0 keeps the chip in 24-hour mode
    out.weekday = ToBcd(WeekdayFromEpochSeconds(ToEpochSeconds(time)));
    out.day = ToBcd(time.day);
    out.month = ToBcd(time.month);
    out.year = ToBcd(static_cast<uint8_t>(time.year % 100U));
}

}  // namespace micropixel::platform::drivers::rx8130
