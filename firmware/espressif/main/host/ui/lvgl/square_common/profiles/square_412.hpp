#pragma once

#include "host/ui/gesture_thresholds.hpp"
#include "host/ui/lvgl/square_common/profiles/square_412_layout.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"

namespace micropixel::host_ui::lvgl::square_common::profiles::square_412 {

inline constexpr SquareLayout kSquareLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .hall_card_width = Layout::kHallCardWidth,
    .hall_card_height = Layout::kHallCardHeight,
    .transition_intermediate_width = 206U,
    .guest_transition_duration_ms = 140U,
    .fullscreen_to_intermediate_scale = 8.0F / 16.0F,
    .intermediate_to_card_scale = 9.0F / 16.0F,
};

inline constexpr int32_t kLaunchLabelBottomOffset = 20;
// Same presentation choices as square_480: the other round target, where the
// launch bitmap is scaled to fit and the background stays transparent so the
// Host shows the Hall through it.
inline constexpr bool kScaleOversizedLaunchBitmap = true;
inline constexpr bool kDeriveLaunchBackground = false;
inline constexpr bool kAllowSoftwareStatusAnimation = false;

inline constexpr HallCardLayout kHallCardLayout{
    .width = Layout::kHallCardWidth,
    .height = Layout::kHallCardHeight,
    .radius = 12,
    .border_width = 1,
    .label_y = 114,
    .label_x = 7,
    .label_width = 94,
    .label_height = 16,
    .running_x = 6,
    .running_y = 6,
    .running_width = 62,
    .running_height = 24,
    .stop_x = 76,
    .stop_y = 6,
    .stop_size = 26,
    .stop_hit_padding = 14,
    .stop_icon_size = 9,
    .label_font = platform::lvgl::SystemFontRole::kSmall,
    .badge_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr HallSceneLayout kHallSceneLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .brand = {.x = 60, .y = 66, .width = 10, .height = 31},
    .brand_radius = 5,
    .title = {.x = 83, .y = 65},
    .section = {.x = 60, .y = 146},
    .settings_button = {.x = 212, .y = 96, .width = 104, .height = 39},
    .update_button = {.x = 96, .y = 96, .width = 104, .height = 39},
    .header_button_radius = 14,
    .header_button_icon_x = 9,
    .header_button_label_x = 29,
    .header_button_font = platform::lvgl::SystemFontRole::kMedium,
    .header_button_pad_horizontal = 10,
    .carousel = {.x = Layout::kHallLeft,
                 .y = Layout::kHallTop,
                 .width = Layout::kHallViewportWidth,
                 .height = Layout::kHallCardHeight},
    .card_width = Layout::kHallCardWidth,
    .card_gap = Layout::kHallCardGap,
    .content_trailing_width = Layout::kHallLeft,
    .fully_visible_cards = 3U,
    .scroll_track = {.x = 165,
                     .y = 318,
                     .width = Layout::kHallScrollTrackWidth,
                     .height = Layout::kHallScrollTrackHeight},
    .scroll_min_thumb_width = 17,
    .empty_message = {.x = 60, .y = 210},
    .simple_status = {.x = 60, .y = 330},
    .simple_app_id = {.x = 64, .y = 350},
    .status_text_width = 320,
    .status_bar =
        {
            .height = 34,
            // The shared scene lays this row out full width with space-between,
            // so the time and the status icons are pushed onto the padding
            // edges and the whole row has to stay inside the circle. Dropping it
            // 24 px puts the items' centre line at y = 41, where the circle is
            // 247 px wide, so 96 px of padding on each side leaves a margin.
            // Pinned to the top instead, y = 17 is only 164 px wide and the row
            // ends up exactly on the bezel - which is what clipped the time and
            // the Wi-Fi icon. The bar still ends at y = 58, clear of the brand
            // at y = 66.
            .top_offset = 24,
            .padding_left = 96,
            .padding_right = 96,
            .item_gap = 7,
            .battery_gap = 6,
            .time_width = 46,
            .cellular = {.width = 14, .height = 9},
            .wifi_scale = 256U,
        },
    // Selection is driven by the knob as well as by a finger, so a released
    // drag has to leave the highlight on a card.
    .snap_carousel_to_cards = true,
};

static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kTopReservedEdge) >=
              kHallSceneLayout.status_bar.height);
static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kBottomReservedEdge) >=
              gesture_thresholds::GestureHintBottomExtent(Layout::kWidth));

inline constexpr SystemMenuLayout kSystemMenuLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    // The title bar is the top strip of the same inscribed square the rows use:
    // inset by kRoundInset and starting at y = kRoundInset, so it lines up with
    // the rows below it, its corners land exactly on the cover's edge, and the
    // title block inside it is never clipped. The bar takes the height its
    // content wants and the rows start below its real bottom, so this band is
    // only the bound used when no bar was built.
    .header_height = Layout::kRoundInset + 68,
    .header_padding_horizontal = Layout::kRoundInset,
    .header_padding_top = Layout::kRoundInset,
    .header_bar_radius = 14,
    .header_bar_padding_horizontal = 12,
    .header_bar_padding_vertical = 6,
    .header_gap = 14,
    .back_button_size = 38,
    .back_button_radius = 12,
    .back_button_hit_padding = 7,
    .header_text_gap = 0,
    .content_padding_horizontal = Layout::kRoundInset,
    .content_padding_top = 5,
    .content_padding_bottom = 7,
    .row_height = 62,
    .row_gap = 9,
    .row_radius = 14,
    .row_padding_horizontal = 12,
    .row_icon_width = 34,
    .row_content_gap = 10,
    .row_text_gap = 0,
    .row_chevron_width = 15,
    .firmware_dot_size = 7,
    .scrollbar_width = 4,
    .scrollbar_radius = 2,
    .row_icon_font = platform::lvgl::SystemFontRole::kMedium,
    .row_name_font = platform::lvgl::SystemFontRole::kMedium,
    .row_detail_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr SystemPageLayout kSystemPageLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    // Same title bar as the menu: it is the top strip of the inscribed square.
    .header_height = Layout::kRoundInset + 68,
    .safe_horizontal = Layout::kRoundInset,
    .header_padding_horizontal = Layout::kRoundInset,
    .header_padding_top = Layout::kRoundInset,
    .header_bar_radius = 14,
    .header_bar_padding_horizontal = 12,
    .header_bar_padding_vertical = 6,
    .header_gap = 14,
    .header_text_gap = 0,
    .back_button_size = 38,
    .back_button_radius = 12,
    .back_button_hit_padding = 7,
    .content_padding_top = 7,
    .content_padding_bottom = 17,
    .section_gap = 10,
    .panel_gap = 7,
    .panel_padding = 14,
    .panel_radius = 14,
    .row_height = 41,
    .control_height = 43,
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
    .status_layer_layout = StatusLayerLayoutProfile::kRound412,
    .launch_label_bottom_offset = kLaunchLabelBottomOffset,
    .scale_oversized_launch_bitmap = kScaleOversizedLaunchBitmap,
    .derive_launch_background = kDeriveLaunchBackground,
    .allow_software_status_animation = kAllowSoftwareStatusAnimation,
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::square_412
