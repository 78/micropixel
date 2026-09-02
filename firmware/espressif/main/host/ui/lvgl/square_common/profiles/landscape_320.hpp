#pragma once

#include "host/ui/gesture_thresholds.hpp"
#include "host/ui/lvgl/square_common/profiles/landscape_320_layout.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"

namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_320 {

inline constexpr SquareLayout kSquareLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .hall_card_width = Layout::kHallCardWidth,
    .hall_card_height = Layout::kHallCardHeight,
    .transition_intermediate_width = 160U,
    .guest_transition_duration_ms = 0U,
    .fullscreen_to_intermediate_scale = 0.5F,
    .intermediate_to_card_scale = 0.55F,
};

inline constexpr HallCardLayout kHallCardLayout{
    .width = Layout::kHallCardWidth,
    .height = Layout::kHallCardHeight,
    .radius = 10,
    .border_width = 1,
    .label_y = 94,
    .label_x = 5,
    .label_width = 78,
    .label_height = 18,
    .running_x = 5,
    .running_y = 5,
    .running_width = 52,
    .running_height = 24,
    .stop_x = 61,
    .stop_y = 5,
    .stop_size = 22,
    .stop_hit_padding = 5,
    .stop_icon_size = 9,
    .label_font = platform::lvgl::SystemFontRole::kSmall,
    .badge_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr HallSceneLayout kHallSceneLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .brand = {.x = 12, .y = 34, .width = 7, .height = 25},
    .brand_radius = 4,
    .title = {.x = 27, .y = 32},
    .section = {.x = 12, .y = 68},
    .settings_button = {.x = 230, .y = 31, .width = 78, .height = 32},
    .update_button = {.x = 146, .y = 31, .width = 76, .height = 32},
    .header_button_radius = 10,
    .header_button_icon_x = 7,
    .header_button_label_x = 25,
    .header_button_font = platform::lvgl::SystemFontRole::kSmall,
    .header_button_pad_horizontal = 7,
    .carousel = {.x = Layout::kHallLeft,
                 .y = Layout::kHallTop,
                 .width = Layout::kHallViewportWidth,
                 .height = Layout::kHallCardHeight},
    .card_width = Layout::kHallCardWidth,
    .card_gap = Layout::kHallCardGap,
    .content_trailing_width = Layout::kHallLeft,
    .fully_visible_cards = 3U,
    .scroll_track = {.x = 124,
                     .y = 220,
                     .width = Layout::kHallScrollTrackWidth,
                     .height = Layout::kHallScrollTrackHeight},
    .scroll_min_thumb_width = 14,
    .empty_message = {.x = 12, .y = 110},
    .failure_status = {.x = 12, .y = 92},
    .failure_app_id = {.x = 12, .y = 116},
    .failure_identity = {.x = 12, .y = 138},
    .failure_detail = {.x = 12, .y = 160},
    .simple_status = {.x = 12, .y = 150},
    .simple_app_id = {.x = 12, .y = 174},
    .status_text_width = 296,
    .status_bar =
        {
            .height = 24,
            .padding_left = 12,
            .padding_right = 4,
            .item_gap = 7,
            .battery_gap = 3,
            .time_width = 48,
            .cellular = {.width = 14, .height = 10},
            .wifi_scale = 224U,
            .battery_width = 20,
            .battery_percent_width = 27,
        },
};

static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kTopReservedEdge) >=
              kHallSceneLayout.status_bar.height);
static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kBottomReservedEdge) >=
              gesture_thresholds::GestureHintBottomExtent(Layout::kWidth));

inline constexpr SystemMenuLayout kSystemMenuLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 54,
    .header_padding_horizontal = 12,
    .header_padding_top = 10,
    .header_gap = 10,
    .back_button_size = 34,
    .back_button_radius = 10,
    .back_button_hit_padding = 5,
    .header_text_gap = 0,
    .content_padding_horizontal = 12,
    .content_padding_top = 4,
    .content_padding_bottom = 6,
    .row_height = 52,
    .row_gap = 6,
    .row_radius = 10,
    .row_padding_horizontal = 10,
    .row_icon_width = 26,
    .row_content_gap = 8,
    .row_text_gap = 0,
    .row_chevron_width = 14,
    .firmware_dot_size = 6,
    .scrollbar_width = 3,
    .scrollbar_radius = 2,
    .row_icon_font = platform::lvgl::SystemFontRole::kMedium,
    .row_name_font = platform::lvgl::SystemFontRole::kMedium,
    .row_detail_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr SystemPageLayout kSystemPageLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 54,
    .safe_horizontal = 12,
    .header_padding_top = 10,
    .header_gap = 10,
    .header_text_gap = 0,
    .back_button_size = 34,
    .back_button_radius = 10,
    .back_button_hit_padding = 5,
    .content_padding_top = 5,
    .content_padding_bottom = 10,
    .section_gap = 8,
    .panel_gap = 5,
    .panel_padding = 10,
    .panel_radius = 10,
    .row_height = 38,
    .control_height = 40,
    .scrollbar_width = 3,
    .scrollbar_radius = 2,
    .title_font = platform::lvgl::SystemFontRole::kLarge,
    .subtitle_font = platform::lvgl::SystemFontRole::kSmall,
    .heading_font = platform::lvgl::SystemFontRole::kMedium,
    .body_font = platform::lvgl::SystemFontRole::kSmall,
    .detail_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr SquareSystemUiProfile kSystemUiProfile{
    .square = kSquareLayout,
    .hall_card = kHallCardLayout,
    .hall_scene = kHallSceneLayout,
    .system_menu = kSystemMenuLayout,
    .system_page = kSystemPageLayout,
    .launch_label_bottom_offset = 10,
    .scale_oversized_launch_bitmap = true,
    .derive_launch_background = false,
    .allow_software_status_animation = false,
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_320
