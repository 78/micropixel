#include "host/ui/lvgl/square_common/host_ui_theme.hpp"

#include <array>

#include "lvgl.h"

namespace micropixel::host_ui::lvgl::square_common::theme {
namespace {

struct ThemeStyles final {
    lv_style_t screen{};
    lv_style_t label{};
    lv_style_t action{};
    lv_style_t pressed{};
    lv_style_t disabled{};
    lv_style_t dropdown_indicator{};
    lv_style_t dropdown_list{};
    lv_style_t selected{};
    lv_style_t selected_checked{};
    lv_style_t selected_pressed{};
    lv_style_t switch_track{};
    lv_style_t switch_checked{};
    lv_style_t switch_knob{};
    lv_style_t bar_track{};
    lv_style_t bar_indicator{};
    lv_style_t textarea_placeholder{};
    lv_style_t textarea_cursor{};
    lv_style_t keyboard{};
    lv_style_t keyboard_key{};
    lv_style_t keyboard_key_checked{};
    lv_style_t scrollbar{};
};

ThemeStyles g_styles{};
lv_theme_t* g_theme{};
lv_display_t* g_display{};
Mode g_mode{Mode::kPureBlack};
bool g_styles_initialized{};

constexpr lv_style_selector_t Selector(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(static_cast<uint32_t>(part) | static_cast<uint32_t>(state));
}

void InitializeStyles() {
    if (g_styles_initialized) {
        return;
    }
    constexpr auto kStyles = [](ThemeStyles& styles) {
        return std::array<lv_style_t*, 21>{
            &styles.screen,           &styles.label,
            &styles.action,           &styles.pressed,
            &styles.disabled,         &styles.dropdown_indicator,
            &styles.dropdown_list,    &styles.selected,
            &styles.selected_checked, &styles.selected_pressed,
            &styles.switch_track,     &styles.switch_checked,
            &styles.switch_knob,      &styles.bar_track,
            &styles.bar_indicator,    &styles.textarea_placeholder,
            &styles.textarea_cursor,  &styles.keyboard,
            &styles.keyboard_key,     &styles.keyboard_key_checked,
            &styles.scrollbar,
        };
    };
    for (lv_style_t* style : kStyles(g_styles)) {
        lv_style_init(style);
    }
    g_styles_initialized = true;
}

void ConfigureStyles(const ThemePalette& palette) {
    lv_style_set_bg_color(&g_styles.screen, lv_color_hex(palette.canvas));
    lv_style_set_bg_opa(&g_styles.screen, LV_OPA_COVER);
    lv_style_set_text_color(&g_styles.screen, lv_color_hex(palette.primary_text));

    lv_style_set_text_color(&g_styles.label, lv_color_hex(palette.primary_text));

    lv_style_set_bg_color(&g_styles.action, lv_color_hex(palette.elevated_surface));
    lv_style_set_bg_opa(&g_styles.action, LV_OPA_COVER);
    lv_style_set_border_color(&g_styles.action, lv_color_hex(palette.border));
    lv_style_set_border_width(&g_styles.action, 1);
    lv_style_set_text_color(&g_styles.action, lv_color_hex(palette.primary_text));
    lv_style_set_shadow_width(&g_styles.action, 0);

    lv_style_set_bg_color(&g_styles.pressed, lv_color_hex(palette.pressed_surface));
    lv_style_set_bg_opa(&g_styles.pressed, LV_OPA_COVER);
    lv_style_set_text_color(&g_styles.pressed, lv_color_hex(palette.primary_text));

    lv_style_set_opa(&g_styles.disabled, LV_OPA_40);

    lv_style_set_text_color(&g_styles.dropdown_indicator, lv_color_hex(palette.secondary_text));

    lv_style_set_bg_color(&g_styles.dropdown_list, lv_color_hex(palette.surface));
    lv_style_set_bg_opa(&g_styles.dropdown_list, LV_OPA_COVER);
    lv_style_set_border_color(&g_styles.dropdown_list, lv_color_hex(palette.strong_border));
    lv_style_set_border_width(&g_styles.dropdown_list, 1);
    lv_style_set_text_color(&g_styles.dropdown_list, lv_color_hex(palette.primary_text));
    lv_style_set_shadow_width(&g_styles.dropdown_list, 0);

    lv_style_set_bg_color(&g_styles.selected, lv_color_hex(palette.pressed_surface));
    lv_style_set_bg_opa(&g_styles.selected, LV_OPA_COVER);
    lv_style_set_text_color(&g_styles.selected, lv_color_hex(palette.primary_text));

    lv_style_set_bg_color(&g_styles.selected_checked, lv_color_hex(palette.strong_accent));
    lv_style_set_bg_opa(&g_styles.selected_checked, LV_OPA_COVER);
    lv_style_set_text_color(&g_styles.selected_checked, lv_color_hex(palette.overlay_text));

    lv_style_set_bg_color(&g_styles.selected_pressed, lv_color_hex(palette.pressed_surface));
    lv_style_set_bg_opa(&g_styles.selected_pressed, LV_OPA_COVER);

    lv_style_set_bg_color(&g_styles.switch_track, lv_color_hex(palette.pressed_surface));
    lv_style_set_bg_opa(&g_styles.switch_track, LV_OPA_COVER);
    lv_style_set_border_color(&g_styles.switch_track, lv_color_hex(palette.border));
    lv_style_set_border_width(&g_styles.switch_track, 1);

    lv_style_set_bg_color(&g_styles.switch_checked, lv_color_hex(palette.control_accent));
    lv_style_set_bg_opa(&g_styles.switch_checked, LV_OPA_COVER);

    lv_style_set_bg_color(&g_styles.switch_knob, lv_color_hex(palette.overlay_text));
    lv_style_set_bg_opa(&g_styles.switch_knob, LV_OPA_COVER);

    lv_style_set_bg_color(&g_styles.bar_track, lv_color_hex(palette.border));
    lv_style_set_bg_opa(&g_styles.bar_track, LV_OPA_COVER);
    lv_style_set_bg_color(&g_styles.bar_indicator, lv_color_hex(palette.control_accent));
    lv_style_set_bg_opa(&g_styles.bar_indicator, LV_OPA_COVER);

    lv_style_set_text_color(&g_styles.textarea_placeholder, lv_color_hex(palette.muted_text));
    lv_style_set_border_color(&g_styles.textarea_cursor, lv_color_hex(palette.control_accent));
    lv_style_set_border_width(&g_styles.textarea_cursor, 2);

    lv_style_set_bg_color(&g_styles.keyboard, lv_color_hex(palette.canvas));
    lv_style_set_bg_opa(&g_styles.keyboard, LV_OPA_COVER);
    lv_style_set_text_color(&g_styles.keyboard, lv_color_hex(palette.primary_text));

    lv_style_set_bg_color(&g_styles.keyboard_key, lv_color_hex(palette.elevated_surface));
    lv_style_set_bg_opa(&g_styles.keyboard_key, LV_OPA_COVER);
    lv_style_set_border_color(&g_styles.keyboard_key, lv_color_hex(palette.border));
    lv_style_set_border_width(&g_styles.keyboard_key, 1);
    lv_style_set_text_color(&g_styles.keyboard_key, lv_color_hex(palette.primary_text));

    lv_style_set_bg_color(&g_styles.keyboard_key_checked, lv_color_hex(palette.strong_accent));
    lv_style_set_bg_opa(&g_styles.keyboard_key_checked, LV_OPA_COVER);
    lv_style_set_text_color(&g_styles.keyboard_key_checked, lv_color_hex(palette.overlay_text));

    lv_style_set_bg_color(&g_styles.scrollbar, lv_color_hex(palette.strong_border));
    lv_style_set_bg_opa(&g_styles.scrollbar, LV_OPA_COVER);
}

void ApplyTheme(lv_theme_t*, lv_obj_t* object) {
    lv_obj_t* parent = lv_obj_get_parent(object);
    if (parent == nullptr) {
        lv_obj_add_style(object, &g_styles.screen, 0);
        return;
    }
    if (lv_obj_check_type(object, &lv_dropdown_class)) {
        lv_obj_add_style(object, &g_styles.action, 0);
        lv_obj_add_style(object, &g_styles.pressed, LV_STATE_PRESSED);
        lv_obj_add_style(object, &g_styles.disabled, LV_STATE_DISABLED);
        lv_obj_add_style(object, &g_styles.dropdown_indicator, LV_PART_INDICATOR);
    } else if (lv_obj_check_type(object, &lv_dropdownlist_class)) {
        lv_obj_add_style(object, &g_styles.dropdown_list, 0);
        lv_obj_add_style(object, &g_styles.selected, LV_PART_SELECTED);
        lv_obj_add_style(object, &g_styles.selected_checked, Selector(LV_PART_SELECTED, LV_STATE_CHECKED));
        lv_obj_add_style(object, &g_styles.selected_pressed, Selector(LV_PART_SELECTED, LV_STATE_PRESSED));
        lv_obj_add_style(object, &g_styles.scrollbar, LV_PART_SCROLLBAR);
    } else if (lv_obj_check_type(object, &lv_switch_class)) {
        lv_obj_add_style(object, &g_styles.switch_track, 0);
        lv_obj_add_style(object, &g_styles.switch_checked, Selector(LV_PART_INDICATOR, LV_STATE_CHECKED));
        lv_obj_add_style(object, &g_styles.switch_knob, LV_PART_KNOB);
        lv_obj_add_style(object, &g_styles.disabled, LV_STATE_DISABLED);
    } else if (lv_obj_check_type(object, &lv_bar_class)) {
        lv_obj_add_style(object, &g_styles.bar_track, 0);
        lv_obj_add_style(object, &g_styles.bar_indicator, LV_PART_INDICATOR);
    } else if (lv_obj_check_type(object, &lv_textarea_class)) {
        lv_obj_add_style(object, &g_styles.action, 0);
        lv_obj_add_style(object, &g_styles.disabled, LV_STATE_DISABLED);
        lv_obj_add_style(object, &g_styles.textarea_placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_add_style(object, &g_styles.textarea_cursor, Selector(LV_PART_CURSOR, LV_STATE_FOCUSED));
        lv_obj_add_style(object, &g_styles.scrollbar, LV_PART_SCROLLBAR);
    } else if (lv_obj_check_type(object, &lv_keyboard_class)) {
        lv_obj_add_style(object, &g_styles.keyboard, 0);
        lv_obj_add_style(object, &g_styles.keyboard_key, LV_PART_ITEMS);
        lv_obj_add_style(object, &g_styles.pressed, Selector(LV_PART_ITEMS, LV_STATE_PRESSED));
        lv_obj_add_style(object, &g_styles.keyboard_key_checked, Selector(LV_PART_ITEMS, LV_STATE_CHECKED));
        lv_obj_add_style(object, &g_styles.disabled, Selector(LV_PART_ITEMS, LV_STATE_DISABLED));
    } else if (lv_obj_check_type(object, &lv_button_class)) {
        lv_obj_add_style(object, &g_styles.action, 0);
        lv_obj_add_style(object, &g_styles.pressed, LV_STATE_PRESSED);
        lv_obj_add_style(object, &g_styles.disabled, LV_STATE_DISABLED);
    } else if (lv_obj_check_type(object, &lv_label_class)) {
        lv_obj_add_style(object, &g_styles.label, 0);
    }
}

void SelectMode(Mode mode) {
    g_mode = mode;
    kActiveTheme = PaletteFor(mode);
    kHallCoverColors = {kOrange, kSuccess, kControlAccent};
}

bool IsDarkMode(Mode mode) { return mode != Mode::kSoftIvory; }

}  // namespace

const ThemePalette& PaletteFor(Mode mode) {
    switch (mode) {
        case Mode::kPureBlack:
            return kPureBlackTheme;
        case Mode::kDeepBlue:
            return kDeepBlueTheme;
        case Mode::kSoftIvory:
            return kSoftIvoryTheme;
    }
    return kPureBlackTheme;
}

const ThemePalette& ActivePalette() { return kActiveTheme; }

Mode ActiveMode() { return g_mode; }

bool Install(lv_display_t* display, Mode mode) {
    if (display == nullptr) {
        return false;
    }
    InitializeStyles();
    SelectMode(mode);
    ConfigureStyles(kActiveTheme);

    lv_theme_t* base = lv_theme_default_init(display, lv_color_hex(kControlAccent), lv_color_hex(kDanger),
                                             IsDarkMode(mode), LV_FONT_DEFAULT);
    if (base == nullptr) {
        return false;
    }
    if (g_theme == nullptr) {
        g_theme = lv_theme_create();
        if (g_theme == nullptr) {
            return false;
        }
        lv_theme_set_apply_cb(g_theme, ApplyTheme);
    }
    lv_theme_set_parent(g_theme, base);
    g_display = display;
    lv_display_set_theme(display, g_theme);
    lv_obj_report_style_change(nullptr);
    return true;
}

bool SetModeLocked(Mode mode) {
    if (g_display == nullptr || g_theme == nullptr) {
        return false;
    }
    SelectMode(mode);
    lv_theme_t* base = lv_theme_default_init(g_display, lv_color_hex(kControlAccent), lv_color_hex(kDanger),
                                             IsDarkMode(mode), LV_FONT_DEFAULT);
    if (base == nullptr) {
        return false;
    }
    lv_theme_set_parent(g_theme, base);
    ConfigureStyles(kActiveTheme);
    lv_obj_report_style_change(nullptr);
    lv_obj_t* screen = lv_display_get_screen_active(g_display);
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
    }
    return true;
}

}  // namespace micropixel::host_ui::lvgl::square_common::theme
