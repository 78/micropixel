#pragma once

#include "host/ui/gesture_thresholds.hpp"
#include "host/ui/lvgl/square_common/profiles/square_480_layout.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"

namespace micropixel::host_ui::lvgl::square_common::profiles::square_480 {

inline constexpr SquareLayout kSquareLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .hall_card_width = Layout::kHallCardWidth,
    .hall_card_height = Layout::kHallCardHeight,
    .transition_intermediate_width = 240U,
    .guest_transition_duration_ms = 140U,
    .fullscreen_to_intermediate_scale = 8.0F / 16.0F,
    .intermediate_to_card_scale = 9.0F / 16.0F,
};

inline constexpr int32_t kLaunchLabelBottomOffset = 24;
inline constexpr bool kScaleOversizedLaunchBitmap = true;
inline constexpr bool kDeriveLaunchBackground = false;
inline constexpr bool kAllowSoftwareStatusAnimation = false;

inline constexpr HallCardLayout kHallCardLayout{
    .width = Layout::kHallCardWidth,
    .height = Layout::kHallCardHeight,
    .radius = 15,
    .border_width = 1,
    .label_y = 143,
    .label_x = 8,
    .label_width = 119,
    .label_height = 20,
    .running_x = 7,
    .running_y = 7,
    .running_width = 78,
    .running_height = 30,
    .stop_x = 96,
    .stop_y = 7,
    .stop_size = 32,
    .stop_hit_padding = 6,
    .stop_icon_size = 12,
    .label_font = platform::lvgl::SystemFontRole::kSmall,
    .badge_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr HallSceneLayout kHallSceneLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .brand = {.x = 24, .y = 56, .width = 12, .height = 36},
    .brand_radius = 6,
    .title = {.x = 50, .y = 55},
    .section = {.x = 24, .y = 136},
    .settings_button = {.x = 330, .y = 54, .width = 126, .height = 46},
    .update_button = {.x = 192, .y = 54, .width = 126, .height = 46},
    .header_button_radius = 16,
    .header_button_icon_x = 10,
    .header_button_label_x = 34,
    .header_button_font = platform::lvgl::SystemFontRole::kMedium,
    .header_button_pad_horizontal = 12,
    .carousel = {.x = Layout::kHallLeft,
                 .y = Layout::kHallTop,
                 .width = Layout::kHallViewportWidth,
                 .height = Layout::kHallCardHeight},
    .card_width = Layout::kHallCardWidth,
    .card_gap = Layout::kHallCardGap,
    .content_trailing_width = Layout::kHallLeft,
    .fully_visible_cards = 3U,
    .scroll_track = {.x = 192,
                     .y = 362,
                     .width = Layout::kHallScrollTrackWidth,
                     .height = Layout::kHallScrollTrackHeight},
    .scroll_min_thumb_width = 20,
    .empty_message = {.x = 24, .y = 200},
    .failure_status = {.x = 24, .y = 365},
    .failure_app_id = {.x = 24, .y = 389},
    .failure_identity = {.x = 24, .y = 411},
    .failure_detail = {.x = 24, .y = 433},
    .simple_status = {.x = 24, .y = 390},
    .simple_app_id = {.x = 24, .y = 418},
    .status_text_width = 432,
    .status_bar =
        {
            .height = 40,
            .padding_left = 40,
            .padding_right = 28,
            .item_gap = 8,
            .battery_gap = 7,
            .time_width = 54,
            .cellular = {.width = 16, .height = 11},
            .wifi_scale = 256U,
            .battery_width = 24,
            .battery_percent_width = 48,
        },
};

static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kTopReservedEdge) >=
              kHallSceneLayout.status_bar.height);
static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kBottomReservedEdge) >=
              gesture_thresholds::GestureHintBottomExtent(Layout::kWidth));

inline constexpr SystemMenuLayout kSystemMenuLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 96,
    .header_padding_horizontal = 24,
    .header_padding_top = 24,
    .header_gap = 16,
    .back_button_size = 44,
    .back_button_radius = 14,
    .back_button_hit_padding = 8,
    .header_text_gap = 0,
    .content_padding_horizontal = 24,
    .content_padding_top = 6,
    .content_padding_bottom = 8,
    .row_height = 72,
    .row_gap = 10,
    .row_radius = 16,
    .row_padding_horizontal = 14,
    .row_icon_width = 40,
    .row_content_gap = 12,
    .row_text_gap = 0,
    .row_chevron_width = 18,
    .firmware_dot_size = 8,
    .scrollbar_width = 4,
    .scrollbar_radius = 2,
    .row_icon_font = platform::lvgl::SystemFontRole::kMedium,
    .row_name_font = platform::lvgl::SystemFontRole::kMedium,
    .row_detail_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr SystemPageLayout kSystemPageLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 96,
    .safe_horizontal = 24,
    .header_padding_top = 24,
    .header_gap = 16,
    .header_text_gap = 0,
    .back_button_size = 44,
    .back_button_radius = 14,
    .back_button_hit_padding = 8,
    .content_padding_top = 8,
    .content_padding_bottom = 20,
    .section_gap = 12,
    .panel_gap = 8,
    .panel_padding = 16,
    .panel_radius = 16,
    .row_height = 48,
    .control_height = 50,
    .scrollbar_width = 4,
    .scrollbar_radius = 2,
    .title_font = platform::lvgl::SystemFontRole::kTitle,
    .subtitle_font = platform::lvgl::SystemFontRole::kSmall,
    .heading_font = platform::lvgl::SystemFontRole::kLarge,
    .body_font = platform::lvgl::SystemFontRole::kMedium,
    .detail_font = platform::lvgl::SystemFontRole::kSmall,
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

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::square_480
