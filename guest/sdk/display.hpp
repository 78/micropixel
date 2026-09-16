// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_SDK_DISPLAY_HPP
#define MICROPIXEL_SDK_DISPLAY_HPP

#include "sdk/geometry.hpp"

namespace micropixel {

enum class DisplayScaleMode : uint8_t { kNative, kAspectFit, kAspectFill, kExpand };

struct DisplayConfiguration final {
    Size logical_size{};
    DisplayScaleMode scale_mode{DisplayScaleMode::kNative};
};

}  // namespace micropixel
#endif
