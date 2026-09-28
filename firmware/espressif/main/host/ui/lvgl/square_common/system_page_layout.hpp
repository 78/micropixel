#pragma once

#include <algorithm>
#include <cstdint>

#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// This build does not enable LVGL object names (LV_USE_OBJ_NAME), so a page
// header records the back button it built instead of looking it up by name.
// Pages are built and destroyed on the LVGL task, only one of them is on
// display at a time, and every button removes its own record before it is
// freed, so the record can never outlive the object it points at.
inline constexpr uint32_t kMaxPageBackButtons = 8U;

inline lv_obj_t* g_page_back_buttons[kMaxPageBackButtons]{};
inline uint32_t g_page_back_button_count{};

inline void ForgetPageBackButton(lv_obj_t* button) {
    for (uint32_t index = 0U; index < g_page_back_button_count; ++index) {
        if (g_page_back_buttons[index] != button) {
            continue;
        }
        for (uint32_t move = index + 1U; move < g_page_back_button_count; ++move) {
            g_page_back_buttons[move - 1U] = g_page_back_buttons[move];
        }
        --g_page_back_button_count;
        g_page_back_buttons[g_page_back_button_count] = nullptr;
        return;
    }
}

inline void OnPageBackButtonDeleted(lv_event_t* event) { ForgetPageBackButton(lv_event_get_target_obj(event)); }

// Called by the header that creates the button. Deep pages are created last, so
// the newest record is the page on display.
inline void RegisterPageBackButton(lv_obj_t* button) {
    if (button == nullptr) {
        return;
    }
    lv_obj_add_event_cb(button, OnPageBackButtonDeleted, LV_EVENT_DELETE, nullptr);
    if (g_page_back_button_count >= kMaxPageBackButtons) {
        return;
    }
    g_page_back_buttons[g_page_back_button_count] = button;
    ++g_page_back_button_count;
}

// Runs the back button of the page on display, which is what a Host-level back
// request means. Call it from the LVGL context, because it clicks a widget.
inline void InvokePageBack() {
    for (uint32_t index = g_page_back_button_count; index > 0U; --index) {
        lv_obj_t* const button = g_page_back_buttons[index - 1U];
        if (button == nullptr || lv_obj_is_hidden(button)) {
            continue;
        }
        (void)lv_obj_send_event(button, LV_EVENT_SHORT_CLICKED, nullptr);
        return;
    }
}

// Density and safe-area tokens for responsive System UI pages. Page code uses
// Flex/Grid and content sizing; it never contains resolution-specific object
// coordinates.
struct SystemPageLayout final {
    int32_t width{};
    int32_t height{};
    int32_t header_height{};
    int32_t safe_horizontal{};
    // The header band sits where a round panel is narrowest, so it needs more
    // inset than the content below it. 0 keeps the content inset, which is what
    // every rectangular profile wants.
    int32_t header_padding_horizontal{};
    int32_t header_padding_top{};
    // Title-bar tokens. A profile with a bar radius turns the header into a
    // rounded rectangle that fills the top strip of the same inscribed square
    // the content uses: inset by the content inset and starting at the square's
    // top edge, so its width matches the rows below it and all four corners stay
    // inside a round cover. Its height follows its content, so a title block is
    // never clipped by the band. 0 keeps the plain transparent band that square
    // panels use.
    int32_t header_bar_radius{};
    int32_t header_bar_padding_horizontal{};
    int32_t header_bar_padding_vertical{};
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
    platform::lvgl::SystemFontRole title_font{platform::lvgl::SystemFontRole::kTitle};
    platform::lvgl::SystemFontRole subtitle_font{platform::lvgl::SystemFontRole::kMedium};
    platform::lvgl::SystemFontRole heading_font{platform::lvgl::SystemFontRole::kLarge};
    platform::lvgl::SystemFontRole body_font{platform::lvgl::SystemFontRole::kMedium};
    platform::lvgl::SystemFontRole detail_font{platform::lvgl::SystemFontRole::kSmall};
};

inline void StyleTransparentContainer(lv_obj_t* object) {
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_clickable(object, false);
}

inline lv_obj_t* CreateSystemLabel(lv_obj_t* parent, const char* text, platform::lvgl::SystemFontRole font,
                                   uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_font(label, platform::lvgl::BuiltinLatinFont(font), 0);
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

// The header's horizontal inset: its own token when a profile sets one, the
// content inset otherwise.
[[nodiscard]] inline int32_t HeaderPaddingHorizontal(const SystemPageLayout& layout) {
    return layout.header_padding_horizontal > 0 ? layout.header_padding_horizontal : layout.safe_horizontal;
}

struct SystemHeaderBar final {
    int32_t width{};
    int32_t header_height{};
    int32_t inset{};
    int32_t top{};
    int32_t gap{};
    int32_t radius{};
    int32_t padding_horizontal{};
    int32_t padding_vertical{};
};

inline constexpr int32_t kHeaderBarContentGap = 8;

// The title bar the page just built. Pages build their bar before their content
// and both run on the LVGL task, which is the same reason the back-button
// registry above can be a plain slot.
inline lv_obj_t* g_last_header_bar{};

// The y a page's content starts at: right below the bar that belongs to `root`
// when the profile draws one, so any title block - builtin or CJK TTF metrics -
// gets the room it needs; the profile's band otherwise.
[[nodiscard]] inline int32_t SystemContentTop(lv_obj_t* root, int32_t band_height) {
    lv_obj_t* const bar = g_last_header_bar;
    if (bar == nullptr || root == nullptr || lv_obj_get_parent(bar) != root) {
        return band_height;
    }
    // The bar's height is its content's, so it has to be laid out before it can
    // be read.
    lv_obj_update_layout(bar);
    return lv_obj_get_y(bar) + lv_obj_get_height(bar) + kHeaderBarContentGap;
}

// Builds the title bar. Without a radius it is the plain transparent band a
// square panel uses; with one it is a rounded rectangle in the top strip of the
// inscribed square, with the height its content needs.
inline lv_obj_t* CreateSystemHeaderBar(lv_obj_t* root, const SystemHeaderBar& bar) {
    lv_obj_t* header = lv_obj_create(root);
    lv_obj_set_scrollable(header, false);
    lv_obj_set_clickable(header, false);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(header, bar.gap, 0);
    if (bar.radius > 0) {
        lv_obj_set_pos(header, bar.inset, bar.top);
        lv_obj_set_size(header, bar.width - 2 * bar.inset, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(header, bar.radius, 0);
        lv_obj_set_style_bg_color(header, lv_color_hex(theme::kPanelBackground), 0);
        lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(header, 1, 0);
        lv_obj_set_style_border_color(header, lv_color_hex(theme::kStrongBorder), 0);
        lv_obj_set_style_shadow_width(header, 0, 0);
        lv_obj_set_style_pad_left(header, bar.padding_horizontal, 0);
        lv_obj_set_style_pad_right(header, bar.padding_horizontal, 0);
        lv_obj_set_style_pad_top(header, bar.padding_vertical, 0);
        lv_obj_set_style_pad_bottom(header, bar.padding_vertical, 0);
        lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        g_last_header_bar = header;
        return header;
    }
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, bar.width, bar.header_height);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_left(header, bar.inset, 0);
    lv_obj_set_style_pad_right(header, bar.inset, 0);
    lv_obj_set_style_pad_top(header, bar.top, 0);
    lv_obj_set_style_pad_bottom(header, 0, 0);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return header;
}

inline lv_obj_t* CreateSystemHeader(lv_obj_t* root, const SystemPageLayout& layout, const char* title,
                                    const char* subtitle, lv_event_cb_t back_event, void* context) {
    lv_obj_t* header = CreateSystemHeaderBar(root, {.width = layout.width,
                                                    .header_height = layout.header_height,
                                                    .inset = HeaderPaddingHorizontal(layout),
                                                    .top = layout.header_padding_top,
                                                    .gap = layout.header_gap,
                                                    .radius = layout.header_bar_radius,
                                                    .padding_horizontal = layout.header_bar_padding_horizontal,
                                                    .padding_vertical = layout.header_bar_padding_vertical});

    lv_obj_t* back = lv_button_create(header);
    RegisterPageBackButton(back);
    lv_obj_set_size(back, layout.back_button_size, layout.back_button_size);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_radius(back, layout.back_button_radius, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(theme::kStrongBorder), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(theme::kNavigationBackground), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(theme::kPressedBackground),
                              static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_scrollable(back, false);
    lv_obj_set_ext_click_area(back, std::max<int32_t>(0, layout.back_button_hit_padding));
    lv_obj_add_event_cb(back, back_event, LV_EVENT_SHORT_CLICKED, context);
    lv_obj_t* icon =
        CreateSystemLabel(back, LV_SYMBOL_LEFT, platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_center(icon);

    lv_obj_t* text = CreateSystemColumn(header, layout.header_text_gap);
    lv_obj_set_width(text, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_t* title_label = CreateSystemLabel(text, title, layout.title_font, theme::kPrimaryText);
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    if (subtitle != nullptr && subtitle[0] != '\0') {
        lv_obj_t* subtitle_label = CreateSystemLabel(text, subtitle, layout.subtitle_font, theme::kSecondaryText);
        lv_obj_set_width(subtitle_label, LV_PCT(100));
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_DOT);
    }
    return header;
}

inline lv_obj_t* CreateSystemScrollColumn(lv_obj_t* root, const SystemPageLayout& layout, lv_event_cb_t scroll_event,
                                          void* context) {
    lv_obj_t* scroll = lv_obj_create(root);
    const int32_t top = SystemContentTop(root, layout.header_height);
    lv_obj_set_pos(scroll, 0, top);
    lv_obj_set_size(scroll, layout.width, layout.height - top);
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
    lv_obj_set_scrollable(scroll, true);
    lv_obj_set_scroll_momentum(scroll, true);
    lv_obj_set_scroll_elastic(scroll, true);
    if (scroll_event != nullptr) {
        lv_obj_add_event_cb(scroll, scroll_event, LV_EVENT_SCROLL, context);
        lv_obj_add_event_cb(scroll, scroll_event, LV_EVENT_SCROLL_END, context);
    }
    lv_obj_set_style_width(scroll, layout.scrollbar_width, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll, layout.scrollbar_radius, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(theme::kStrongBorder), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);
    return scroll;
}

inline void StyleSystemPanel(lv_obj_t* panel, const SystemPageLayout& layout, int32_t gap = -1) {
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(panel, layout.panel_padding, 0);
    lv_obj_set_style_pad_row(panel, gap >= 0 ? gap : layout.panel_gap, 0);
    lv_obj_set_style_radius(panel, layout.panel_radius, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(theme::kBorder), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(theme::kPanelBackground), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollable(panel, false);
}

inline lv_obj_t* CreateSystemPanel(lv_obj_t* parent, const SystemPageLayout& layout, int32_t gap = -1) {
    lv_obj_t* panel = lv_obj_create(parent);
    StyleSystemPanel(panel, layout, gap);
    lv_obj_set_clickable(panel, false);
    return panel;
}

inline lv_obj_t* CreateSystemButtonPanel(lv_obj_t* parent, const SystemPageLayout& layout, int32_t gap = -1) {
    lv_obj_t* button = lv_button_create(parent);
    StyleSystemPanel(button, layout, gap);
    return button;
}

inline lv_obj_t* CreateSystemInformationRow(lv_obj_t* parent, const SystemPageLayout& layout, const char* name,
                                            const char* value, bool wrap_value = false) {
    lv_obj_t* row = lv_obj_create(parent);
    StyleTransparentContainer(row);
    lv_obj_set_size(row, LV_PCT(100), layout.row_height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(theme::kDivider), 0);
    lv_obj_t* name_label = CreateSystemLabel(row, name, layout.detail_font, theme::kSecondaryText);
    lv_obj_set_width(name_label, LV_PCT(42));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_t* value_label = CreateSystemLabel(row, value, layout.detail_font, theme::kPrimaryText);
    lv_obj_set_width(value_label, 0);
    lv_obj_set_flex_grow(value_label, 1);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value_label, wrap_value ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    if (wrap_value) {
        // A content-sized Flex row applies min_height after laying out its children.
        // Symmetric padding keeps text centered without relying on that late clamp.
        const int32_t line_height = lv_font_get_line_height(lv_obj_get_style_text_font(value_label, LV_PART_MAIN));
        const int32_t padding = std::max<int32_t>(0, (layout.row_height - line_height - 1) / 2);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, layout.row_height, 0);
        lv_obj_set_style_pad_top(row, padding, 0);
        lv_obj_set_style_pad_bottom(row, padding, 0);
    }
    return row;
}

inline lv_obj_t* CreateSystemActionButton(lv_obj_t* parent, const SystemPageLayout& layout, const char* text,
                                          uint32_t color = theme::kPrimaryText, bool inherit_theme = true) {
    lv_obj_t* button = lv_button_create(parent);
    // Snapshot-animated buttons can opt out of theme transitions and recolor layers.
    if (!inherit_theme) lv_obj_remove_style_all(button);
    lv_obj_set_size(button, LV_PCT(100), layout.control_height);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, layout.panel_radius - 2, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(theme::kActionBorder), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme::kActionBackground), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme::kActionPressedBackground),
                              static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_scrollable(button, false);
    lv_obj_t* label = CreateSystemLabel(button, text, layout.body_font, color);
    lv_obj_center(label);
    return button;
}

inline lv_obj_t* CreateSystemMoreIndicator(lv_obj_t* parent, int32_t width, int32_t height, int32_t dot_size,
                                           int32_t dot_gap, uint32_t color = theme::kKeyboardDot) {
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
        lv_obj_set_scrollable(dot, false);
        lv_obj_set_clickable(dot, false);
    }
    return indicator;
}

}  // namespace micropixel::host_ui::lvgl::square_common
