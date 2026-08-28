#pragma once

#include <cstdint>

#include "platform/lvgl/display/display_pipeline.hpp"

namespace micropixel::platform::lvgl::square_common {

struct SquareLayout final {
    uint32_t width{};
    uint32_t height{};
    uint32_t hall_card_width{};
    uint32_t hall_card_height{};
    uint32_t transition_intermediate_width{};
    float fullscreen_to_intermediate_scale{};
    float intermediate_to_card_scale{};
};

[[nodiscard]] constexpr SystemTransitionProfile TransitionProfile(const SquareLayout& layout) {
    return {.intermediate_width = layout.transition_intermediate_width,
            .card_width = layout.hall_card_width,
            .fullscreen_to_intermediate_scale = layout.fullscreen_to_intermediate_scale,
            .intermediate_to_card_scale = layout.intermediate_to_card_scale};
}

}  // namespace micropixel::platform::lvgl::square_common
