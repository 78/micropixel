// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel::platform::m5stack_tab5::board {

// Board-level RTC test shadow of the real board_config.hpp. Only the address the
// RTC path needs, so the test does not pull the display, GPIO and LEDC headers
// of the board; keep the value in step with board_config.hpp.
inline constexpr uint8_t kRtcAddress = 0x32U;

}  // namespace micropixel::platform::m5stack_tab5::board
