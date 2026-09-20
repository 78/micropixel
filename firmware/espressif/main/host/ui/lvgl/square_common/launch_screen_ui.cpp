// SPDX-License-Identifier: Apache-2.0
#include "host/ui/lvgl/square_common/launch_screen_ui.hpp"

#include <algorithm>

#include "host/ui/lvgl/square_common/host_ui_theme.hpp"

namespace micropixel::host_ui::lvgl::square_common {

void DrawLaunchScreen(lv_obj_t* root, const LaunchScreenLayout& layout, const lv_image_dsc_t* cover,
                      const HallCardLayout& card_layout, const HallCardPresentation& app, uint32_t app_index) {
    if (cover != nullptr && cover->data != nullptr) {
        uint32_t scale = 256U;
        if (layout.scale_oversized_bitmap) {
            // Preserve aspect ratio and never enlarge a small source image.
            const uint32_t maximum_width = layout.width * 3U / 4U;
            const uint32_t maximum_height = layout.height * 3U / 4U;
            const uint32_t width_scale =
                static_cast<uint32_t>(static_cast<uint64_t>(256U) * maximum_width / cover->header.w);
            const uint32_t height_scale =
                static_cast<uint32_t>(static_cast<uint64_t>(256U) * maximum_height / cover->header.h);
            scale = std::max<uint32_t>(1U, std::min<uint32_t>({256U, width_scale, height_scale}));
        }
        const uint32_t width = std::max<uint32_t>(1U, cover->header.w * scale / 256U);
        const uint32_t height = std::max<uint32_t>(1U, cover->header.h * scale / 256U);
        // Hall pixels only mask the top corners, where the cover meets the card.
        // Clip the standalone launch art without modifying the shared cover cache.
        lv_obj_t* frame = lv_obj_create(root);
        lv_obj_remove_style_all(frame);
        lv_obj_set_size(frame, width, height);
        const uint32_t radius = card_layout.width > 0 && card_layout.radius > 0
                                    ? static_cast<uint32_t>(card_layout.radius) * std::min(width, height) /
                                          static_cast<uint32_t>(card_layout.width)
                                    : 0U;
        lv_obj_set_style_radius(frame, radius, 0);
        lv_obj_set_style_clip_corner(frame, true, 0);
        lv_obj_set_scrollable(frame, false);
        lv_obj_set_clickable(frame, false);
        lv_obj_center(frame);
        lv_obj_t* image = lv_image_create(frame);
        lv_image_set_src(image, cover);
        if (scale < 256U) lv_image_set_scale(image, static_cast<uint16_t>(scale));
        lv_obj_center(image);
    } else {
        lv_obj_t* placeholder = CreateHallCoverPlaceholder(root, card_layout, app, app_index);
        lv_obj_center(placeholder);
    }
    lv_obj_t* label = lv_label_create(root);
    lv_label_set_text(label, "Loading...");
    lv_obj_set_style_text_color(label, lv_color_hex(theme::kLoadingText), 0);
    lv_obj_set_style_text_font(label, platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kLarge), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -layout.label_bottom_offset);
}

}  // namespace micropixel::host_ui::lvgl::square_common
