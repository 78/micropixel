#pragma once

#include <algorithm>
#include <cstdint>

#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::platform::lvgl::square_common {

// Density and safe-area tokens for responsive System UI pages. Page code uses
// Flex/Grid and content sizing; it never contains resolution-specific object
// coordinates.
struct SystemPageLayout final {
    int32_t width{};
    int32_t height{};
    int32_t header_height{};
    int32_t safe_horizontal{};
    int32_t header_padding_top{};
    int32_t header_gap{};
    int32_t header_text_gap{};
    int32_t back_button_size{};
    int32_t back_button_radius{};
    int32_t back_button_hit_padding{};
    int32_t content_padding_top{};
    int32_t content_padding_bottom{};
    int32_t section_gap{};
    int32_t panel_gap{};
    int32_t panel_padding{};
    int32_t panel_radius{};
    int32_t row_height{};
    int32_t control_height{};
    int32_t scrollbar_width{};
    int32_t scrollbar_radius{};
    SystemFontRole title_font{SystemFontRole::kTitle};
    SystemFontRole subtitle_font{SystemFontRole::kMedium};
    SystemFontRole heading_font{SystemFontRole::kLarge};
    SystemFontRole body_font{SystemFontRole::kMedium};
    SystemFontRole detail_font{SystemFontRole::kSmall};
};

inline void StyleTransparentContainer(lv_obj_t* object) {
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
}

inline lv_obj_t* CreateSystemLabel(lv_obj_t* parent, const char* text, SystemFontRole font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, BuiltinLatinFont(font), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

inline lv_obj_t* CreateSystemColumn(lv_obj_t* parent, int32_t gap = 0) {
    lv_obj_t* column = lv_obj_create(parent);
    StyleTransparentContainer(column);
    lv_obj_set_size(column, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(column, gap, 0);
    return column;
}

inline lv_obj_t* CreateSystemHeader(lv_obj_t* root, const SystemPageLayout& layout, const char* title,
                                    const char* subtitle, lv_event_cb_t back_event, void* context) {
    lv_obj_t* header = lv_obj_create(root);
    StyleTransparentContainer(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, layout.width, layout.header_height);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(header, layout.safe_horizontal, 0);
    lv_obj_set_style_pad_right(header, layout.safe_horizontal, 0);
    lv_obj_set_style_pad_top(header, layout.header_padding_top, 0);
    lv_obj_set_style_pad_column(header, layout.header_gap, 0);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_set_size(back, layout.back_button_size, layout.back_button_size);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_radius(back, layout.back_button_radius, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x42607fU), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0d1929U), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x081321U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(back, std::max<int32_t>(0, layout.back_button_hit_padding));
    lv_obj_add_event_cb(back, back_event, LV_EVENT_SHORT_CLICKED, context);
    lv_obj_t* icon = CreateSystemLabel(back, LV_SYMBOL_LEFT, SystemFontRole::kLarge, 0xf2f7ffU);
    lv_obj_center(icon);

    lv_obj_t* text = CreateSystemColumn(header, layout.header_text_gap);
    lv_obj_set_width(text, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_t* title_label = CreateSystemLabel(text, title, layout.title_font, 0xf2f7ffU);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    if (subtitle != nullptr && subtitle[0] != '\0') {
        lv_obj_t* subtitle_label = CreateSystemLabel(text, subtitle, layout.subtitle_font, 0x91a4bdU);
        lv_obj_set_width(subtitle_label, LV_PCT(100));
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_DOT);
    }
    return header;
}

inline lv_obj_t* CreateSystemScrollColumn(lv_obj_t* root, const SystemPageLayout& layout, lv_event_cb_t scroll_event,
                                          void* context) {
    lv_obj_t* scroll = lv_obj_create(root);
    lv_obj_set_pos(scroll, 0, layout.header_height);
    lv_obj_set_size(scroll, layout.width, layout.height - layout.header_height);
    lv_obj_set_style_pad_left(scroll, layout.safe_horizontal, 0);
    lv_obj_set_style_pad_right(scroll, layout.safe_horizontal, 0);
    lv_obj_set_style_pad_top(scroll, layout.content_padding_top, 0);
    lv_obj_set_style_pad_bottom(scroll, layout.content_padding_bottom, 0);
    lv_obj_set_style_pad_row(scroll, layout.section_gap, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    constexpr lv_obj_flag_t kScrollFlags = static_cast<lv_obj_flag_t>(
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLLABLE) | static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM) |
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_add_flag(scroll, kScrollFlags);
    if (scroll_event != nullptr) {
        lv_obj_add_event_cb(scroll, scroll_event, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(scroll, scroll_event, LV_EVENT_SCROLL_END, context);
    }
    lv_obj_set_style_width(scroll, layout.scrollbar_width, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll, layout.scrollbar_radius, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(0x42607fU), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
    return scroll;
}

inline void StyleSystemPanel(lv_obj_t* panel, const SystemPageLayout& layout, int32_t gap = -1) {
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(panel, layout.panel_padding, 0);
    lv_obj_set_style_pad_row(panel, gap >= 0 ? gap : layout.panel_gap, 0);
    lv_obj_set_style_radius(panel, layout.panel_radius, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2e4562U), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111f32U), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

inline lv_obj_t* CreateSystemPanel(lv_obj_t* parent, const SystemPageLayout& layout, int32_t gap = -1) {
    lv_obj_t* panel = lv_obj_create(parent);
    StyleSystemPanel(panel, layout, gap);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

inline lv_obj_t* CreateSystemButtonPanel(lv_obj_t* parent, const SystemPageLayout& layout, int32_t gap = -1) {
    lv_obj_t* button = lv_button_create(parent);
    StyleSystemPanel(button, layout, gap);
    return button;
}

inline lv_obj_t* CreateSystemInformationRow(lv_obj_t* parent, const SystemPageLayout& layout, const char* name,
                                            const char* value) {
    lv_obj_t* row = lv_obj_create(parent);
    StyleTransparentContainer(row);
    lv_obj_set_size(row, LV_PCT(100), layout.row_height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x21364eU), 0);
    lv_obj_t* name_label = CreateSystemLabel(row, name, layout.detail_font, 0x91a4bdU);
    lv_obj_set_width(name_label, LV_PCT(42));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_t* value_label = CreateSystemLabel(row, value, layout.detail_font, 0xf2f7ffU);
    lv_obj_set_width(value_label, 0);
    lv_obj_set_flex_grow(value_label, 1);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
    return row;
}

inline lv_obj_t* CreateSystemActionButton(lv_obj_t* parent, const SystemPageLayout& layout, const char* text,
                                          uint32_t color = 0xf2f7ffU) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), layout.control_height);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, layout.panel_radius - 2, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x365472U), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x16263aU), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0b1726U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* label = CreateSystemLabel(button, text, layout.body_font, color);
    lv_obj_center(label);
    return button;
}

inline lv_obj_t* CreateSystemMoreIndicator(lv_obj_t* parent, int32_t width, int32_t height, int32_t dot_size,
                                           int32_t dot_gap, uint32_t color = 0xa9bdd5U) {
    lv_obj_t* indicator = lv_obj_create(parent);
    StyleTransparentContainer(indicator);
    lv_obj_set_size(indicator, width, height);
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(indicator, dot_gap, 0);
    for (int32_t index = 0; index < 3; ++index) {
        lv_obj_t* dot = lv_obj_create(indicator);
        lv_obj_set_size(dot, dot_size, dot_size);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
    return indicator;
}

}  // namespace micropixel::platform::lvgl::square_common
