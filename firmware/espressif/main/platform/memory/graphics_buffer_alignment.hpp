#pragma once

#include <cstdint>

#include "sdkconfig.h"

namespace micropixel::platform::memory {

// P4 PSRAM DMA destinations must own complete configured cache lines.
// Other targets retain their existing graphics allocation alignment.
#if CONFIG_IDF_TARGET_ESP32P4
inline constexpr uint32_t kGraphicsBufferAlignment = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
#else
inline constexpr uint32_t kGraphicsBufferAlignment = 128U;
#endif
static_assert(kGraphicsBufferAlignment >= 64U && (kGraphicsBufferAlignment & (kGraphicsBufferAlignment - 1U)) == 0U);

}  // namespace micropixel::platform::memory
