// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_SDK_GAMEPAD_STYLE_HPP
#define MICROPIXEL_SDK_GAMEPAD_STYLE_HPP

#include "sdk/graphics.hpp"

namespace micropixel {

// Per-button appearance, baked into the skin atlas when it is initialized.
struct GamepadButtonStyle final {
    Color fill{Color::Rgb(24U, 24U, 24U)};
    uint8_t fill_opacity{0U};
    Color rim{Color::White()};
    uint8_t rim_opacity{40U};
    Color pressed_fill{Color::Rgb(32U, 32U, 32U)};
    uint8_t pressed_fill_opacity{128U};
    Color glyph{Color::Rgb(112U, 112U, 112U)};
};

}  // namespace micropixel

#endif
