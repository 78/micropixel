#pragma once

#include "host/ui/lvgl/square_common/profiles/square_720_layout.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"

namespace micropixel::host_ui::lvgl::square_common::profiles::square_720 {

inline constexpr SquareLayout kSquareLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .hall_card_width = Layout::kHallCardWidth,
    .hall_card_height = Layout::kHallCardHeight,
    .transition_intermediate_width = 360U,
    .guest_transition_duration_ms = 100U,
    .fullscreen_to_intermediate_scale = 8.0F / 16.0F,
    .intermediate_to_card_scale = 9.0F / 16.0F,
};

inline constexpr int32_t kLaunchLabelBottomOffset = 70;
inline constexpr bool kScaleOversizedLaunchBitmap = false;
inline constexpr bool kDeriveLaunchBackground = true;
// Both square targets use their PPA/DMA2D transition compositor. If that
// hardware path is unavailable, fall back to a static presentation rather
// than animating full-screen LVGL frames in software.
inline constexpr bool kAllowSoftwareStatusAnimation = false;

inline constexpr HallCardLayout kHallCardLayout{
    .width = Layout::kHallCardWidth,
    .height = Layout::kHallCardHeight,
    .radius = 22,
    .border_width = 1,
    .label_y = 214,
    .label_x = 12,
    .label_width = 178,
    .label_height = 24,
    .running_x = 10,
    .running_y = 10,
    .running_width = 112,
    .running_height = 44,
    .stop_x = 146,
    .stop_y = 10,
    .stop_size = 46,
    .stop_hit_padding = 10,
    .stop_icon_size = 18,
    .label_font = platform::lvgl::SystemFontRole::kMedium,
    .badge_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr HallSceneLayout kHallSceneLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .brand = {.x = 40, .y = 72, .width = 18, .height = 44},
    .brand_radius = 9,
    .title = {.x = 76, .y = 68},
    .section = {.x = 40, .y = 163},
    .settings_button = {.x = 496, .y = 74, .width = 184, .height = 64},
    .update_button = {.x = 296, .y = 74, .width = 184, .height = 64},
    .header_button_radius = 22,
    .header_button_icon_x = 18,
    .header_button_label_x = 50,
    .header_button_font = platform::lvgl::SystemFontRole::kLarge,
    .header_button_pad_horizontal = 18,
    .carousel = {.x = Layout::kHallLeft,
                 .y = Layout::kHallTop,
                 .width = Layout::kHallViewportWidth,
                 .height = Layout::kHallCardHeight},
    .card_width = Layout::kHallCardWidth,
    .card_gap = Layout::kHallCardGap,
    .content_trailing_width = Layout::kHallLeft,
    .fully_visible_cards = 3U,
    .scroll_track = {.x = 290,
                     .y = 486,
                     .width = Layout::kHallScrollTrackWidth,
                     .height = Layout::kHallScrollTrackHeight},
    .scroll_min_thumb_width = 28,
    .empty_message = {.x = 40, .y = 235},
    .failure_status = {.x = 40, .y = 560},
    .failure_app_id = {.x = 40, .y = 592},
    .failure_identity = {.x = 40, .y = 618},
    .failure_detail = {.x = 40, .y = 646},
    .simple_status = {.x = 40, .y = 604},
    .simple_app_id = {.x = 40, .y = 638},
    .status_text_width = 640,
    .status_bar =
        {
            .time = {.x = 40, .y = 14},
            .time_width = 72,
            .cellular = {.x = 548, .y = 18, .width = 22, .height = 16},
            .wifi = {.x = 584, .y = 17},
            .wifi_scale = 256U,
            .battery = {.x = 616, .y = 10},
            .battery_width = 30,
            .battery_percent = {.x = 649, .y = 14},
            .battery_percent_width = 48,
        },
};

inline constexpr SystemMenuLayout kSystemMenuLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 108,
    .header_padding_horizontal = 40,
    .header_padding_top = 26,
    .header_gap = 20,
    .back_button_size = 56,
    .back_button_radius = 18,
    .back_button_hit_padding = 12,
    .header_text_gap = 1,
    .content_padding_horizontal = 40,
    .content_padding_top = 8,
    .content_padding_bottom = 8,
    .row_height = 96,
    .row_gap = 12,
    .row_radius = 22,
    .row_padding_horizontal = 26,
    .row_icon_width = 56,
    .row_content_gap = 22,
    .row_text_gap = 1,
    .row_chevron_width = 24,
    .firmware_dot_size = 10,
    .scrollbar_width = 5,
    .scrollbar_radius = 3,
    .row_icon_font = platform::lvgl::SystemFontRole::kLarge,
    .row_name_font = platform::lvgl::SystemFontRole::kLarge,
    .row_detail_font = platform::lvgl::SystemFontRole::kMedium,
};

inline constexpr SystemPageLayout kSystemPageLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 108,
    .safe_horizontal = 40,
    .header_padding_top = 26,
    .header_gap = 20,
    .header_text_gap = 1,
    .back_button_size = 56,
    .back_button_radius = 18,
    .back_button_hit_padding = 12,
    .content_padding_top = 12,
    .content_padding_bottom = 28,
    .section_gap = 18,
    .panel_gap = 12,
    .panel_padding = 22,
    .panel_radius = 22,
    .row_height = 64,
    .control_height = 64,
    .scrollbar_width = 5,
    .scrollbar_radius = 3,
    .title_font = platform::lvgl::SystemFontRole::kTitle,
    .subtitle_font = platform::lvgl::SystemFontRole::kMedium,
    .heading_font = platform::lvgl::SystemFontRole::kLarge,
    .body_font = platform::lvgl::SystemFontRole::kMedium,
    .detail_font = platform::lvgl::SystemFontRole::kMedium,
};

inline constexpr SquareSystemUiProfile kSystemUiProfile{
    .square = kSquareLayout,
    .hall_card = kHallCardLayout,
    .hall_scene = kHallSceneLayout,
    .system_menu = kSystemMenuLayout,
    .system_page = kSystemPageLayout,
    .launch_label_bottom_offset = kLaunchLabelBottomOffset,
    .scale_oversized_launch_bitmap = kScaleOversizedLaunchBitmap,
    .derive_launch_background = kDeriveLaunchBackground,
    .allow_software_status_animation = kAllowSoftwareStatusAnimation,
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::square_720
