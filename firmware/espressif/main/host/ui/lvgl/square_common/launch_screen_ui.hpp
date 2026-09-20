// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/ui/lvgl/square_common/hall_card_ui.hpp"

namespace micropixel::host_ui::lvgl::square_common {

struct LaunchScreenLayout final {
    uint32_t width{};
    uint32_t height{};
    int32_t label_bottom_offset{};
    bool scale_oversized_bitmap{};
};

// The caller owns the root, its background and the optional image descriptor.
// A missing cover renders the same placeholder as Hall; the loading label is
// independent of cover readiness. No storage reads or bitmap allocation.
void DrawLaunchScreen(lv_obj_t* root, const LaunchScreenLayout& layout, const lv_image_dsc_t* cover,
                      const HallCardLayout& card_layout, const HallCardPresentation& app, uint32_t app_index);

}  // namespace micropixel::host_ui::lvgl::square_common
