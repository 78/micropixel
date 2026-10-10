// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "platform/drivers/rtc/rx8130_codec.hpp"

namespace {

using micropixel::platform::drivers::rx8130::Decode;
using micropixel::platform::drivers::rx8130::Encode;
using micropixel::platform::drivers::rx8130::Fields;
using micropixel::platform::drivers::rx8130::FromBcd;
using micropixel::platform::drivers::rx8130::FromEpochSeconds;
using micropixel::platform::drivers::rx8130::IsBcdByte;
using micropixel::platform::drivers::rx8130::IsPlausible;
using micropixel::platform::drivers::rx8130::Time;
using micropixel::platform::drivers::rx8130::ToBcd;
using micropixel::platform::drivers::rx8130::ToEpochSeconds;
using micropixel::platform::drivers::rx8130::WeekdayFromEpochSeconds;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void BcdConversion() {
    static_assert(ToBcd(0U) == 0x00U);
    static_assert(ToBcd(9U) == 0x09U);
    static_assert(ToBcd(26U) == 0x26U);
    static_assert(ToBcd(59U) == 0x59U);
    static_assert(FromBcd(0x00U) == 0U);
    static_assert(FromBcd(0x26U) == 26U);
    static_assert(FromBcd(0x59U) == 59U);
    static_assert(IsBcdByte(0x00U));
    static_assert(IsBcdByte(0x59U));
    static_assert(!IsBcdByte(0x1AU));
    static_assert(!IsBcdByte(0xA0U));
}

void EpochAnchors() {
    // Values cross-checked against the system calendar: the epoch, a leap-day
    // boundary, the millisecond-free firmware era and a date inside it.
    Check(ToEpochSeconds(Time{1970U, 1U, 1U, 0U, 0U, 0U}) == 0, "1970-01-01 is the epoch");
    Check(ToEpochSeconds(Time{2000U, 3U, 1U, 0U, 0U, 0U}) == 951868800, "2000-03-01 anchor");
    Check(ToEpochSeconds(Time{2024U, 1U, 1U, 0U, 0U, 0U}) == 1704067200, "2024-01-01 anchor");
    Check(ToEpochSeconds(Time{2026U, 10U, 4U, 13U, 0U, 0U}) == 1791118800, "2026-10-04 anchor");
    Check(ToEpochSeconds(Time{2026U, 12U, 31U, 23U, 59U, 59U}) == 1798761599, "end-of-year anchor");
    Check(ToEpochSeconds(Time{2028U, 2U, 29U, 12U, 34U, 56U}) == 1835440496, "leap-day anchor");
    Check(WeekdayFromEpochSeconds(0) == 4U, "1970-01-01 was a Thursday");
    Check(WeekdayFromEpochSeconds(1791118800) == 0U, "2026-10-04 is a Sunday");
}

void RoundTripsEveryMinute() {
    // Walk the plausible decade; a civil-from-days slip would show up as an
    // off-by-one day for most of these.
    for (int64_t seconds = 1704067200; seconds < 1798761600; seconds += 60) {
        Time decoded{};
        Check(FromEpochSeconds(seconds, decoded), "in-range epoch must decode");
        Check(ToEpochSeconds(decoded) == seconds, "decode/encode round trip");
    }
}

void DecodeMasksReservedBits() {
    // Reserved bits (SEC bit 7, HOUR bits 6-7, WEEK bit 7, DAY bits 6-7,
    // MONTH bits 5-7) must be ignored, not read as data.
    const Fields fields{0x59U | 0x80U, 0x59U, 0x13U | 0x40U, 0x00U, 0x04U | 0x40U, 0x10U | 0x20U, 0x26U};
    Time decoded{};
    Check(Decode(fields, decoded), "reserved bits must not invalidate a reading");
    Check(decoded.year == 2026U && decoded.month == 10U && decoded.day == 4U && decoded.hour == 13U &&
              decoded.minute == 59U && decoded.second == 59U,
          "masked fields must keep their value");
}

void DecodeRejectsNoise() {
    Time decoded{};
    // 0x4A is not BCD (low nibble > 9).
    Check(!Decode(Fields{0x4AU, 0x00U, 0x00U, 0x00U, 0x01U, 0x01U, 0x24U}, decoded), "non-BCD second is rejected");
    // Day 0 is valid BCD but not a calendar date.
    Check(!Decode(Fields{0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x24U}, decoded), "day 0 is rejected");
    // Month 13.
    Check(!Decode(Fields{0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U, 0x24U}, decoded), "month 13 is rejected");
    // Weekday 7 is outside Sunday..Saturday; the register's reserved bits above
    // bit 2 are dropped by the mask (see DecodeMasksReservedBits).
    Check(!Decode(Fields{0x00U, 0x00U, 0x00U, 0x07U, 0x01U, 0x01U, 0x24U}, decoded), "weekday 7 is rejected");
}

void EncodeWritesBcdWithWeekday() {
    Fields fields{};
    Encode(Time{2026U, 10U, 4U, 13U, 59U, 59U}, fields);
    Check(fields.second == 0x59U && fields.minute == 0x59U && fields.hour == 0x13U && fields.day == 0x04U &&
              fields.month == 0x10U && fields.year == 0x26U,
          "encoded calendar must be BCD");
    Check(fields.weekday == 0x00U, "Sunday must encode as weekday 0");
    Fields monday{};
    Encode(Time{2026U, 10U, 5U, 0U, 0U, 0U}, monday);
    Check(monday.weekday == 0x01U, "Monday must encode as weekday 1");
}

void PlausibilityWindow() {
    static_assert(IsPlausible(Time{2026U, 10U, 4U, 13U, 0U, 0U}));
    static_assert(!IsPlausible(Time{2000U, 1U, 1U, 0U, 0U, 0U}));
    static_assert(!IsPlausible(Time{2023U, 12U, 31U, 23U, 59U, 59U}));
    static_assert(!IsPlausible(Time{2026U, 0U, 4U, 0U, 0U, 0U}));
    static_assert(!IsPlausible(Time{2026U, 10U, 4U, 24U, 0U, 0U}));

    Time decoded{};
    Check(!FromEpochSeconds(0, decoded), "1970 predates the chip's two-digit year range");
    // 2000-01-01 is the oldest value the chip can store, and a never-set
    // calendar is storable but not plausible.
    Check(FromEpochSeconds(946684800, decoded) && decoded.year == 2000U, "2000 is the oldest storable year");
    Check(!IsPlausible(decoded), "a never-set calendar is not plausible");
    Check(!FromEpochSeconds(4102444800, decoded), "2100 must not be stored");
}

}  // namespace

int main() {
    BcdConversion();
    EpochAnchors();
    RoundTripsEveryMinute();
    DecodeMasksReservedBits();
    DecodeRejectsNoise();
    EncodeWritesBcdWithWeekday();
    PlausibilityWindow();
    return 0;
}
