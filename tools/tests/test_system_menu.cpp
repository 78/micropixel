// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "host/ui/lvgl/square_common/system_menu_ui.hpp"

// Exercise real LVGL layout/events; hardware scanout and locking are not part of this fixture.
namespace micropixel::host_ui {
const char* DisplayLocale() { return "en"; }
}  // namespace micropixel::host_ui
namespace micropixel::platform::lvgl {
const lv_font_t* BuiltinLatinFont(SystemFontRole role) {
    switch (role) {
        case SystemFontRole::kSmall:
            return &lv_font_montserrat_10;
        case SystemFontRole::kMedium:
            return &lv_font_montserrat_14;
        default:
            return &lv_font_montserrat_20;
    }
}
}  // namespace micropixel::platform::lvgl
namespace micropixel::host_ui::lvgl::square_common {
class StatusLayerTransition {};
void ActionSheetPresenter::RequestLocked(const SystemPageLayout&, lv_obj_t*, SystemUiActionSink, void*) {}
}  // namespace micropixel::host_ui::lvgl::square_common
namespace {
using namespace micropixel::host_ui;
using namespace micropixel::host_ui::lvgl::square_common;
void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
unsigned actions{};
SystemUiAction last{};
void Action(void*, const SystemUiAction& action) {
    ++actions;
    last = action;
}
lv_obj_t* Find(lv_obj_t* parent, const char* text) {
    if (lv_obj_check_type(parent, &lv_label_class) && !std::strcmp(lv_label_get_text(parent), text)) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (auto* found = Find(lv_obj_get_child(parent, i), text)) return found;
    return nullptr;
}
lv_obj_t* Button(lv_obj_t* label) {
    for (auto* p = label; p; p = lv_obj_get_parent(p))
        if (lv_obj_check_type(p, &lv_button_class)) return p;
    return nullptr;
}
void CheckInformationRows(lv_obj_t* root, const SystemPageLayout& layout) {
    auto* panel = CreateSystemPanel(root, layout, 0);
    lv_obj_set_width(panel, layout.width - 2 * layout.safe_horizontal);
    auto* row = CreateSystemInformationRow(panel, layout, "Signal", "Ready", true);
    auto* value = lv_obj_get_child(row, 1);
    lv_obj_update_layout(root);
    lv_area_t row_area{}, value_area{};
    lv_obj_get_coords(row, &row_area);
    lv_obj_get_coords(value, &value_area);
    Check(std::abs((row_area.y1 + row_area.y2) - (value_area.y1 + value_area.y2)) <= 2,
          "single-line details centered between dividers");
    const int32_t short_height = lv_obj_get_height(row);
    lv_label_set_text(value,
                      "A long carrier name that wraps across multiple lines without touching the divider below it");
    lv_obj_update_layout(root);
    lv_obj_get_coords(row, &row_area);
    lv_obj_get_coords(value, &value_area);
    Check(lv_obj_get_height(row) > short_height, "wrapped values grow their row");
    Check(value_area.y1 > row_area.y1 && value_area.y2 < row_area.y2 - 1, "wrapped value stays above divider");
    Check(std::abs((row_area.y1 + row_area.y2) - (value_area.y1 + value_area.y2)) <= 2,
          "wrapped details retain symmetric spacing");
    lv_obj_delete(panel);
}
void Run(const SystemMenuLayout& layout, const SystemPageLayout& page) {
    auto* display = lv_display_create(layout.width, layout.height);
    std::vector<uint32_t> buffer(layout.width * layout.height);
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size() * 4, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); });
    auto* root = lv_screen_active();
    CheckInformationRows(root, page);
    StatusLayerTransition transition;
    ActionSheetPresenter presenter(transition);
    SystemMenuUi ui(page, presenter);
    SystemMenuModel model{};
    model.locale = "en";
    Check(ui.ShowLocked(root, display, layout, model, Action, nullptr).has_value(), "show settings menu");
    const auto dot = [&](const char* name) -> lv_obj_t* {
        auto* row = Button(Find(root, name));
        Check(row, "settings row exists");
        for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
            auto* child = lv_obj_get_child(row, i);
            if (lv_obj_get_style_width(child, LV_PART_MAIN) == layout.firmware_dot_size &&
                lv_obj_get_style_height(child, LV_PART_MAIN) == layout.firmware_dot_size)
                return child;
        }
        return nullptr;
    };
    auto* language_dot = dot("Language");
    auto* apps_dot = dot("Manage Apps");
    Check(language_dot && apps_dot && lv_obj_has_flag(language_dot, LV_OBJ_FLAG_HIDDEN) &&
              lv_obj_has_flag(apps_dot, LV_OBJ_FLAG_HIDDEN),
          "update badges start hidden");
    model.font_update_available = true;
    ui.Update(model);
    Check(!lv_obj_has_flag(language_dot, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(apps_dot, LV_OBJ_FLAG_HIDDEN),
          "font update only marks Language");
    model.font_update_available = false;
    model.app_updates_available = true;
    ui.Update(model);
    Check(lv_obj_has_flag(language_dot, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(apps_dot, LV_OBJ_FLAG_HIDDEN),
          "ordinary app update only marks Manage Apps");
    ui.Deactivate();
    lv_obj_clean(root);
    model.app_updates_available = false;
    model.language_view = true;
    Check(ui.ShowLocked(root, display, layout, model, Action, nullptr).has_value(), "show language menu");
    auto* current = Find(root, "English");
    Check(current && !Button(current), "current language has no clickable button");
    auto* update = Button(Find(root, "Update font"));
    Check(update && lv_obj_has_flag(update, LV_OBJ_FLAG_HIDDEN), "font update entry hidden without release");
    model.font_update_available = true;
    ui.Update(model);
    Check(!lv_obj_has_flag(update, LV_OBJ_FLAG_HIDDEN), "font update entry appears on discovery");
    const auto before = actions;
    lv_obj_send_event(update, LV_EVENT_SHORT_CLICKED, nullptr);
    Check(actions == before + 1 && last.type == SystemUiActionType::kUpdateLanguageFont,
          "dedicated font update action");
    model.language_sheet = true;
    model.language_updating = true;
    model.language_selected = 0;
    model.language_state = LanguageDownloadState::kChecking;
    model.font_current_version = "1.0.0";
    model.font_update_version = "1.1.0";
    ui.Update(model);
    auto* overlay = lv_obj_get_child(root, -1);
    Check(Find(overlay, "Update current font?") && Find(overlay, "1.0.0 -> 1.1.0"),
          "sheet identifies font update versions");
    auto* confirm = Button(Find(overlay, "Update font"));
    Check(confirm && lv_obj_has_state(confirm, LV_STATE_DISABLED), "discovery cannot confirm prematurely");
    model.language_state = LanguageDownloadState::kConfirm;
    model.language_download_bytes = 3 * 1024 * 1024;
    model.language_required_bytes = model.language_download_bytes + 65536;
    model.language_free_bytes = 5 * 1024 * 1024;
    ui.Update(model);
    Check(!lv_obj_has_state(confirm, LV_STATE_DISABLED), "resolved update can be confirmed");
    lv_obj_update_layout(root);
    auto* sheet = lv_obj_get_child(overlay, 0);
    lv_area_t area{};
    lv_obj_get_coords(sheet, &area);
    Check(area.y1 >= 0 && area.y2 < layout.height, "update sheet stays within display");
    auto* close = Button(Find(overlay, "Close"));
    Check(close, "update sheet has close action");
    lv_obj_scroll_to_view(close, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    lv_obj_get_coords(close, &area);
    Check(area.y1 >= 0 && area.y2 < layout.height, "close remains reachable on small displays");
    lv_obj_send_event(confirm, LV_EVENT_SHORT_CLICKED, nullptr);
    const auto submitted = actions;
    lv_obj_send_event(confirm, LV_EVENT_SHORT_CLICKED, nullptr);
    Check(actions == submitted && last.type == SystemUiActionType::kConfirmLanguage,
          "duplicate confirmation is latched");
    model.language_state = LanguageDownloadState::kDownloading;
    ui.Update(model);
    lv_obj_send_event(close, LV_EVENT_SHORT_CLICKED, nullptr);
    Check(actions == submitted && lv_obj_has_flag(close, LV_OBJ_FLAG_HIDDEN), "download blocks closing the modal");
    model.language_state = LanguageDownloadState::kFailed;
    ui.Update(model);
    lv_obj_send_event(close, LV_EVENT_SHORT_CLICKED, nullptr);
    Check(last.type == SystemUiActionType::kCancelLanguage && actions == submitted + 1, "failed download can close");
    ui.Deactivate();
    lv_display_delete(display);
}
}  // namespace
int main() {
    lv_init();
    for (int width : {320, 480, 720}) {
        const int height = width == 320 ? 240 : width;
        SystemMenuLayout menu{.width = width,
                              .height = height,
                              .header_height = 54,
                              .header_padding_horizontal = 12,
                              .header_padding_top = 10,
                              .header_gap = 10,
                              .back_button_size = 34,
                              .back_button_radius = 10,
                              .back_button_hit_padding = 5,
                              .content_padding_horizontal = 12,
                              .content_padding_top = 4,
                              .content_padding_bottom = 6,
                              .row_height = 52,
                              .row_gap = 6,
                              .row_radius = 10,
                              .row_padding_horizontal = 10,
                              .row_icon_width = 26,
                              .row_content_gap = 8,
                              .row_chevron_width = 14,
                              .firmware_dot_size = 6,
                              .scrollbar_width = 3,
                              .scrollbar_radius = 2};
        SystemPageLayout page{.width = width,
                              .height = height,
                              .header_height = 54,
                              .safe_horizontal = 12,
                              .header_padding_top = 10,
                              .header_gap = 10,
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
                              .scrollbar_radius = 2};
        Run(menu, page);
    }
    lv_deinit();
    std::puts("System menu LVGL tests passed (320x240, 480x480, 720x720).");
}
