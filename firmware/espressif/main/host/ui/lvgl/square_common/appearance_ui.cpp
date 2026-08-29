#include <array>

#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui_internal.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {
using system_detail_internal::Header;
using system_detail_internal::Label;
using system_detail_internal::Scroll;

constexpr std::array<host_ui::SystemThemeMode, 3> kThemeModes{
    host_ui::SystemThemeMode::kPureBlack,
    host_ui::SystemThemeMode::kSoftIvory,
    host_ui::SystemThemeMode::kDeepBlue,
};

theme::Mode PaletteMode(host_ui::SystemThemeMode mode) {
    switch (mode) {
        case host_ui::SystemThemeMode::kPureBlack:
            return theme::Mode::kPureBlack;
        case host_ui::SystemThemeMode::kDeepBlue:
            return theme::Mode::kDeepBlue;
        case host_ui::SystemThemeMode::kSoftIvory:
            return theme::Mode::kSoftIvory;
    }
    return theme::Mode::kPureBlack;
}

const char* ThemeName(host_ui::SystemThemeMode mode) {
    switch (mode) {
        case host_ui::SystemThemeMode::kPureBlack:
            return "Pure Black";
        case host_ui::SystemThemeMode::kDeepBlue:
            return "Deep Blue";
        case host_ui::SystemThemeMode::kSoftIvory:
            return "Soft Ivory";
    }
    return "Pure Black";
}

const char* ThemeDescription(host_ui::SystemThemeMode mode) {
    switch (mode) {
        case host_ui::SystemThemeMode::kPureBlack:
            return "True black canvas optimized for OLED";
        case host_ui::SystemThemeMode::kDeepBlue:
            return "Deep navy surfaces with cool blue accents";
        case host_ui::SystemThemeMode::kSoftIvory:
            return "Warm ivory canvas for comfortable daytime use";
    }
    return "True black canvas optimized for OLED";
}

lv_obj_t* Swatch(lv_obj_t* parent, uint32_t color, uint32_t border_color) {
    lv_obj_t* swatch = lv_obj_create(parent);
    lv_obj_set_size(swatch, 32, 32);
    lv_obj_set_style_pad_all(swatch, 0, 0);
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swatch, 1, 0);
    lv_obj_set_style_border_color(swatch, lv_color_hex(border_color), 0);
    lv_obj_set_style_bg_color(swatch, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
    lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
    return swatch;
}

}  // namespace

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowAppearanceLocked(
    lv_obj_t* root, const host_ui::AppearanceModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    ResetActiveScreen();
    root_ = root;
    appearance_model_ = model;
    action_sink_ = action_sink;
    action_context_ = action_context;
    active_screen_ = Screen::kAppearance;
    RenderAppearanceLocked();
    return {};
}

void SystemDetailUi::RenderAppearanceLocked() {
    if (!AppearanceVisible() || root_ == nullptr) {
        return;
    }
    appearance_options_ = {};
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::kMenuBackground), 0);
    Header(layout_, root_, "Appearance", "Choose a theme", AppearanceBackEvent, this);
    lv_obj_t* content = Scroll(layout_, root_, ScrollEvent, this);

    for (size_t index = 0U; index < kThemeModes.size(); ++index) {
        const host_ui::SystemThemeMode mode = kThemeModes[index];
        const theme::ThemePalette& palette = theme::PaletteFor(PaletteMode(mode));
        const bool selected = mode == appearance_model_.theme_mode;
        lv_obj_t* option = CreateSystemButtonPanel(content, layout_, 10);
        appearance_options_[index] = option;
        lv_obj_set_style_border_width(option, selected ? 2 : 1, 0);
        lv_obj_set_style_border_color(option, lv_color_hex(selected ? palette.accent : palette.strong_border), 0);
        lv_obj_set_style_bg_color(option, lv_color_hex(palette.surface), 0);
        lv_obj_set_style_bg_color(option, lv_color_hex(palette.pressed_surface), LV_STATE_PRESSED);
        lv_obj_add_event_cb(option, AppearanceThemeEvent, LV_EVENT_SHORT_CLICKED, this);

        lv_obj_t* heading = lv_obj_create(option);
        StyleTransparentContainer(heading);
        lv_obj_set_size(heading, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* text = CreateSystemColumn(heading, 2);
        lv_obj_set_width(text, 0);
        lv_obj_set_flex_grow(text, 1);
        (void)Label(text, ThemeName(mode), layout_.heading_font, palette.primary_text);
        (void)Label(text, ThemeDescription(mode), layout_.detail_font, palette.secondary_text);
        if (selected) {
            (void)Label(heading, LV_SYMBOL_OK, platform::lvgl::SystemFontRole::kLarge, palette.accent);
        }

        lv_obj_t* preview = lv_obj_create(option);
        StyleTransparentContainer(preview);
        lv_obj_set_size(preview, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(preview, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(preview, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(preview, 10, 0);
        (void)Swatch(preview, palette.canvas, palette.strong_border);
        (void)Swatch(preview, palette.surface, palette.strong_border);
        (void)Swatch(preview, palette.elevated_surface, palette.strong_border);
        (void)Swatch(preview, palette.control_accent, palette.control_accent);
    }

    lv_obj_t* note = Label(content, "The selected theme is saved and restored after restart.", layout_.detail_font,
                           theme::kMutedText);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_move_foreground(root_);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::UpdateAppearanceLocked(const host_ui::AppearanceModel& model) {
    if (!AppearanceVisible()) {
        return;
    }
    appearance_model_ = model;
    RenderAppearanceLocked();
}

void SystemDetailUi::LeaveAppearance() {
    if (AppearanceVisible()) {
        appearance_model_ = {};
        ResetActiveScreen();
    }
}

bool SystemDetailUi::AppearanceVisible() const { return active_screen_ == Screen::kAppearance; }

void* SystemDetailUi::AppearanceActionContext() const { return action_context_; }

void SystemDetailUi::AppearanceBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseAppearance});
    }
}

void SystemDetailUi::AppearanceThemeEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->action_sink_ == nullptr) {
        return;
    }
    lv_obj_t* target = lv_event_get_target_obj(event);
    for (size_t index = 0U; index < ui->appearance_options_.size(); ++index) {
        if (ui->appearance_options_[index] == target) {
            ui->action_sink_(ui->action_context_,
                             host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetThemeMode,
                                                     .value = static_cast<uint32_t>(kThemeModes[index])});
            return;
        }
    }
}

}  // namespace micropixel::host_ui::lvgl::square_common
