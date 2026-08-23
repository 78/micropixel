#pragma once

#include <cstdint>

#include "esp_err.h"

struct _lv_display_t;
using lv_display_t = _lv_display_t;  // NOLINT(readability-identifier-naming)

namespace micropixel::platform::metalio_claw4 {

class Gt911Input;

// Host USB development transport; present in the P4 product firmware and kept
// outside the Guest ABI.
[[nodiscard]] esp_err_t InitializeScreenCapture(lv_display_t* display, Gt911Input& touch_input, uint32_t width,
                                                uint32_t height);

}  // namespace micropixel::platform::metalio_claw4
