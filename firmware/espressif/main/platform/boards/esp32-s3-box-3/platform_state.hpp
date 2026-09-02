#pragma once

#include "platform/boards/esp32-s3-common/landscape_320_state.hpp"

namespace micropixel::platform::esp32_s3_box_3::detail {

inline constexpr char kTag[] = "esp32_s3_box_3";
inline constexpr int32_t kWidth = esp32_s3_common::kWidth;
inline constexpr int32_t kHeight = esp32_s3_common::kHeight;
using Box3BoardState = esp32_s3_common::Landscape320State;

}  // namespace micropixel::platform::esp32_s3_box_3::detail
