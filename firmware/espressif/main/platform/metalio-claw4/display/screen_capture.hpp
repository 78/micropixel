#pragma once

#include <cstdint>

#include "esp_err.h"

struct _lv_display_t;
using lv_display_t = _lv_display_t;

namespace micropixel::platform::metalio_claw4 {

class Gt911Input;

// Optional Host-only capture transport; never exposed through the Guest ABI.
[[nodiscard]] esp_err_t InitializeScreenCapture(lv_display_t* display, Gt911Input& touch_input, uint32_t width,
                                                uint32_t height);

}  // namespace micropixel::platform::metalio_claw4
