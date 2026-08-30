#pragma once

#include <array>

#include "platform/drivers/power/bq27220_profile.hpp"

namespace micropixel::platform::esp_mosaico::battery_profile {

using drivers::Bq27220DataMemoryValue;
using drivers::Bq27220DataWidth;

inline constexpr uint16_t kDesignCapacityMah = 80U;
inline constexpr uint16_t kGaugingConfiguration = 0x0d11U;

// Espressif's interim fixed-EDV profile was characterized on two 80 mAh cells at 25 C. Keep the board-specific
// chemistry data here; the shared BQ27220 driver only owns the bounded configuration transaction.
inline constexpr std::array kParameters{
    Bq27220DataMemoryValue{0x929bU, kGaugingConfiguration},
    Bq27220DataMemoryValue{0x929dU, 80U},
    Bq27220DataMemoryValue{0x929fU, kDesignCapacityMah},
    Bq27220DataMemoryValue{0x926bU, 5U},
    Bq27220DataMemoryValue{0x9268U, 20U, Bq27220DataWidth::kU8},
    Bq27220DataMemoryValue{0x926dU, 0U},
    Bq27220DataMemoryValue{0x92a7U, 3670U},
    Bq27220DataMemoryValue{0x92a9U, 115U},
    Bq27220DataMemoryValue{0x92abU, 968U},
    Bq27220DataMemoryValue{0x92adU, 4547U},
    Bq27220DataMemoryValue{0x92afU, 4764U},
    Bq27220DataMemoryValue{0x92b1U, 0x0b00U},
    Bq27220DataMemoryValue{0x92bdU, 4147U},
    Bq27220DataMemoryValue{0x92bfU, 4002U},
    Bq27220DataMemoryValue{0x92c1U, 3969U},
    Bq27220DataMemoryValue{0x92c3U, 3938U},
    Bq27220DataMemoryValue{0x92c5U, 3880U},
    Bq27220DataMemoryValue{0x92c7U, 3824U},
    Bq27220DataMemoryValue{0x92c9U, 3794U},
    Bq27220DataMemoryValue{0x92cbU, 3753U},
    Bq27220DataMemoryValue{0x92cdU, 3677U},
    Bq27220DataMemoryValue{0x92cfU, 3574U},
    Bq27220DataMemoryValue{0x92d1U, 3490U},
    Bq27220DataMemoryValue{0x92b4U, 3000U},
    Bq27220DataMemoryValue{0x92b7U, 3410U},
    Bq27220DataMemoryValue{0x92baU, 3530U},
};

inline constexpr std::array kVerificationParameters{
    Bq27220DataMemoryValue{0x92a7U, 3670U},
    Bq27220DataMemoryValue{0x92adU, 4547U},
    Bq27220DataMemoryValue{0x92c1U, 3969U},
};

inline constexpr drivers::Bq27220Profile kProfile{
    .design_capacity_mah = kDesignCapacityMah,
    .parameters = kParameters,
    .verification_parameters = kVerificationParameters,
};

}  // namespace micropixel::platform::esp_mosaico::battery_profile
