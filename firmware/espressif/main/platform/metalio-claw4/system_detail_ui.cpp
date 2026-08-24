#include "platform/metalio-claw4/system_detail_ui.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>

#include "esp_log.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_details";
constexpr int32_t kScreenWidth = 720;
constexpr int32_t kScreenHeight = 720;

struct Bounds final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int32_t x, int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

lv_obj_t* CreatePanel(lv_obj_t* parent, const Bounds& bounds, uint32_t background, uint32_t border, int32_t radius) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, bounds.x, bounds.y);
    lv_obj_set_size(panel, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_border_width(panel, border == 0U ? 0 : 1, 0);
    if (border != 0U) {
        lv_obj_set_style_border_color(panel, lv_color_hex(border), 0);
    }
    lv_obj_set_style_bg_color(panel, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

lv_obj_t* CreateActionButton(lv_obj_t* parent, const Bounds& bounds, const char* text, uint32_t color,
                             uint32_t border = 0x365472U) {
    constexpr lv_style_selector_t kPressed = static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    lv_obj_t* button = CreatePanel(parent, bounds, 0x16263aU, border, 16);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0b1726U), kPressed);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_center(label);
    return button;
}

void FormatInformationSize(uint32_t kib, char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0U) {
        return;
    }
    if (kib >= 1024U) {
        const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(kib) * 10U + 512U) / 1024U);
        std::snprintf(buffer, capacity, "%" PRIu32 ".%" PRIu32 " MB", tenths / 10U, tenths % 10U);
    } else {
        std::snprintf(buffer, capacity, "%" PRIu32 " KB", kib);
    }
}

lv_obj_t* CreateDetailScrollContent(lv_obj_t* parent, int32_t content_height, lv_event_cb_t scroll_callback,
                                    void* user_data) {
    lv_obj_t* scroll = lv_obj_create(parent);
    lv_obj_set_pos(scroll, 0, 108);
    lv_obj_set_size(scroll, kScreenWidth, kScreenHeight - 108);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    constexpr lv_obj_flag_t kScrollFlags = static_cast<lv_obj_flag_t>(
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLLABLE) | static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM) |
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_add_flag(scroll, kScrollFlags);
    lv_obj_add_event_cb(scroll, scroll_callback, LV_EVENT_SCROLL, user_data);
    lv_obj_add_event_cb(scroll, scroll_callback, LV_EVENT_SCROLL_END, user_data);
    lv_obj_set_style_width(scroll, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(0x42607fU), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);

    lv_obj_t* bottom_spacer = lv_obj_create(scroll);
    lv_obj_set_pos(bottom_spacer, 0, content_height - 1);
    lv_obj_set_size(bottom_spacer, 1, 1);
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);
    return scroll;
}

lv_obj_t* DrawDetailHeader(lv_obj_t* root, const char* title, const char* subtitle, lv_event_cb_t back_callback,
                           void* user_data) {
    lv_obj_t* back = CreatePanel(root, Bounds{.x = 40, .y = 32, .width = 56, .height = 56}, 0x0d1929U, 0x42607fU, 18);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x081321U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_event_cb(back, back_callback, LV_EVENT_SHORT_CLICKED, user_data);
    lv_obj_t* back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_center(back_icon);
    (void)CreateLabel(root, title, &lv_font_montserrat_32, 0xf2f7ffU, 116, 28);
    if (subtitle != nullptr && subtitle[0] != '\0') {
        (void)CreateLabel(root, subtitle, &lv_font_montserrat_18, 0x91a4bdU, 116, 70);
    }
    return back;
}

void DrawInformationSectionLabel(lv_obj_t* parent, const char* text, int32_t y) {
    (void)CreateLabel(parent, text, &lv_font_montserrat_18, 0x748aa5U, 42, y);
}

void DrawInformationRow(lv_obj_t* panel, int32_t y, const char* label, const char* value) {
    if (y > 0) {
        (void)CreatePanel(panel, Bounds{.x = 20, .y = y, .width = 600, .height = 1}, 0x21364eU, 0U, 0);
    }
    (void)CreateLabel(panel, label, &lv_font_montserrat_18, 0x91a4bdU, 22, y + 18);
    lv_obj_t* value_label = CreateLabel(panel, value, &lv_font_montserrat_18, 0xf2f7ffU, 260, y + 18);
    lv_obj_set_width(value_label, 356);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
}

void DrawMemoryMetric(lv_obj_t* panel, int32_t x, int32_t y, const char* name, uint32_t kib, uint32_t color) {
    char value[24]{};
    FormatInformationSize(kib, value, sizeof(value));
    (void)CreateLabel(panel, name, &lv_font_montserrat_18, 0x748aa5U, x, y);
    lv_obj_t* label = CreateLabel(panel, value, &lv_font_montserrat_18, color, x, y + 30);
    lv_obj_set_width(label, 106);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void DrawMemoryStatisticsRow(lv_obj_t* panel, int32_t y, const char* name,
                             const host_ui::MemoryStatisticsModel& memory) {
    if (y > 0) {
        (void)CreatePanel(panel, Bounds{.x = 20, .y = y, .width = 600, .height = 1}, 0x21364eU, 0U, 0);
    }
    (void)CreateLabel(panel, name, &lv_font_montserrat_18, 0xf2f7ffU, 22, y + 35);
    DrawMemoryMetric(panel, 176, y + 17, "TOTAL", memory.total_kib, 0xf2f7ffU);
    DrawMemoryMetric(panel, 288, y + 17, "FREE", memory.free_kib, 0xf2f7ffU);
    DrawMemoryMetric(panel, 400, y + 17, "MINIMUM", memory.minimum_free_kib, 0x69a7ffU);
    DrawMemoryMetric(panel, 512, y + 17, "LARGEST", memory.largest_free_block_kib, 0xf2f7ffU);
}

void DrawAppMoreIndicator(lv_obj_t* parent) {
    for (int32_t index = 0; index < 3; ++index) {
        (void)CreatePanel(parent, Bounds{.x = 594, .y = 35 + index * 13, .width = 7, .height = 7}, 0xa9bdd5U, 0U,
                          LV_RADIUS_CIRCLE);
    }
}

}  // namespace

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowSystemInformationLocked(
    lv_obj_t* root, const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    LeaveAppManagement();
    root_ = root;
    system_information_action_sink_ = action_sink;
    system_information_action_context_ = action_context;
    active_screen_ = Screen::kSystemInformation;
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    (void)DrawDetailHeader(root_, "System Information", "Device and software", SystemInformationBackEvent, this);

    constexpr int32_t kContentHeight = 1300;
    lv_obj_t* scroll_content = CreateDetailScrollContent(root_, kContentHeight, DetailScrollEvent, this);
    lv_obj_t* firmware =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 14, .width = 640, .height = 128}, 0x111f32U, 0x2e4562U, 22);
    (void)CreateLabel(firmware, "MicroPixel Firmware", &lv_font_montserrat_18, 0x91a4bdU, 24, 18);
    char version[96]{};
    std::snprintf(version, sizeof(version), "Version %s", model.firmware_version.data());
    (void)CreateLabel(firmware, version, &lv_font_montserrat_24, 0xf2f7ffU, 24, 49);
    char build[224]{};
    std::snprintf(build, sizeof(build), "Build %s   %s %s", model.build_id.data(), model.build_date.data(),
                  model.build_time.data());
    lv_obj_t* build_label = CreateLabel(firmware, build, &lv_font_montserrat_18, 0x69a7ffU, 24, 88);
    lv_obj_set_width(build_label, 592);
    lv_label_set_long_mode(build_label, LV_LABEL_LONG_DOT);

    DrawInformationSectionLabel(scroll_content, "MEMORY", 166);
    lv_obj_t* memory =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 198, .width = 640, .height = 194}, 0x111f32U, 0x2e4562U, 22);
    DrawMemoryStatisticsRow(memory, 0, "SRAM", model.internal_sram);
    DrawMemoryStatisticsRow(memory, 97, "PSRAM", model.psram);

    DrawInformationSectionLabel(scroll_content, "HARDWARE", 420);
    lv_obj_t* hardware =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 452, .width = 640, .height = 232}, 0x111f32U, 0x2e4562U, 22);
    DrawInformationRow(hardware, 0, "Host Chip", model.host_chip.data());
    DrawInformationRow(hardware, 58, "CPU", model.cpu.data());
    DrawInformationRow(hardware, 116, "Wi-Fi Coprocessor", model.wifi_coprocessor.data());
    DrawInformationRow(hardware, 174, "Flash", model.flash_capacity.data());

    DrawInformationSectionLabel(scroll_content, "DISPLAY", 712);
    lv_obj_t* display_panel =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 744, .width = 640, .height = 232}, 0x111f32U, 0x2e4562U, 22);
    DrawInformationRow(display_panel, 0, "Panel", model.panel.data());
    DrawInformationRow(display_panel, 58, "Interface", model.display_interface.data());
    DrawInformationRow(display_panel, 116, "Resolution", model.resolution.data());
    DrawInformationRow(display_panel, 174, "Touch", model.touch_controller.data());

    DrawInformationSectionLabel(scroll_content, "SOFTWARE", 1004);
    lv_obj_t* software =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 1036, .width = 640, .height = 232}, 0x111f32U, 0x2e4562U, 22);
    DrawInformationRow(software, 0, "ESP-IDF", model.idf_version.data());
    DrawInformationRow(software, 58, "Uptime", model.uptime.data());
    DrawInformationRow(software, 116, "Last Reset", model.last_reset.data());
    DrawInformationRow(software, 174, "Build Date", model.build_date.data());

    lv_obj_update_layout(scroll_content);
    lv_obj_move_foreground(root_);
    lv_timer_ready(lv_display_get_refr_timer(lv_obj_get_display(root_)));
    ESP_LOGI(kTag, "System Information visible: chip=%s firmware=%s", model.host_chip.data(),
             model.firmware_version.data());
    return {};
}

void SystemDetailUi::LeaveSystemInformation() {
    system_information_action_sink_ = nullptr;
    system_information_action_context_ = nullptr;
    if (active_screen_ == Screen::kSystemInformation) {
        active_screen_ = Screen::kNone;
        root_ = nullptr;
    }
}

bool SystemDetailUi::SystemInformationVisible() const { return active_screen_ == Screen::kSystemInformation; }

void* SystemDetailUi::SystemInformationActionContext() const { return system_information_action_context_; }

void SystemDetailUi::DetailScrollEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->root_ != nullptr) {
        lv_timer_ready(lv_display_get_refr_timer(lv_obj_get_display(ui->root_)));
    }
}

void SystemDetailUi::SystemInformationBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->system_information_action_sink_ != nullptr) {
        ui->system_information_action_sink_(
            ui->system_information_action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseSystemInformation});
    }
}

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowAppManagementLocked(
    lv_obj_t* root, const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    LeaveSystemInformation();
    root_ = root;
    app_management_action_sink_ = action_sink;
    app_management_action_context_ = action_context;
    app_management_model_ = model;
    app_management_selected_index_ = 0U;
    app_management_overlay_ = AppOverlay::kNone;
    active_screen_ = Screen::kAppManagement;
    RenderAppManagementLocked(false);
    ESP_LOGI(kTag, "App Management visible: apps=%" PRIu32, model.app_count);
    return {};
}

void SystemDetailUi::LeaveAppManagement() {
    app_management_action_sink_ = nullptr;
    app_management_action_context_ = nullptr;
    app_management_overlay_ = AppOverlay::kNone;
    app_management_model_ = {};
    app_management_rows_.fill(nullptr);
    if (active_screen_ == Screen::kAppManagement) {
        active_screen_ = Screen::kNone;
        root_ = nullptr;
    }
}

bool SystemDetailUi::AppManagementVisible() const { return active_screen_ == Screen::kAppManagement; }

void* SystemDetailUi::AppManagementActionContext() const { return app_management_action_context_; }

uint32_t SystemDetailUi::FindAppManagementRowIndex(lv_obj_t* row) const {
    for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
        if (app_management_rows_[index] == row) {
            return index;
        }
    }
    return app_management_model_.app_count;
}

void SystemDetailUi::AppManagementRenderAsync(void* context) {
    auto* ui = static_cast<SystemDetailUi*>(context);
    if (ui != nullptr && ui->AppManagementVisible()) {
        ui->RenderAppManagementLocked(true);
    }
}

void SystemDetailUi::QueueAppManagementRender() { (void)lv_async_call(AppManagementRenderAsync, this); }

void SystemDetailUi::AppManagementBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->app_management_action_sink_ != nullptr) {
        ui->app_management_action_sink_(
            ui->app_management_action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseAppManagement});
    }
}

void SystemDetailUi::AppManagementRowEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    const uint32_t index = ui->FindAppManagementRowIndex(lv_event_get_current_target_obj(event));
    if (index >= ui->app_management_model_.app_count) {
        return;
    }
    ui->app_management_selected_index_ = index;
    ui->app_management_overlay_ = AppOverlay::kActions;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementCancelEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    ui->app_management_overlay_ = AppOverlay::kNone;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementOpenEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->app_management_action_sink_ == nullptr ||
        ui->app_management_selected_index_ >= ui->app_management_model_.app_count) {
        return;
    }
    ui->app_management_action_sink_(ui->app_management_action_context_,
                                    host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchManagedApp,
                                                            .app_index = ui->app_management_selected_index_});
}

void SystemDetailUi::AppManagementInformationEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    ui->app_management_overlay_ = AppOverlay::kInformation;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    ui->app_management_overlay_ = ui->app_management_model_.uninstall_available ? AppOverlay::kUninstallConfirmation
                                                                                : AppOverlay::kUninstallUnavailable;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementConfirmUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->app_management_action_sink_ == nullptr || !ui->app_management_model_.uninstall_available ||
        ui->app_management_selected_index_ >= ui->app_management_model_.app_count) {
        return;
    }
    ui->app_management_action_sink_(ui->app_management_action_context_,
                                    host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kUninstallManagedApp,
                                                            .app_index = ui->app_management_selected_index_});
}

void SystemDetailUi::DrawAppManagementActionsLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* scrim =
        CreatePanel(root_, Bounds{.x = 0, .y = 0, .width = kScreenWidth, .height = kScreenHeight}, 0x030913U, 0U, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 330, .width = 664, .height = 362}, 0x101c2cU, 0x42607fU, 24);
    lv_obj_t* title = CreateLabel(sheet, app.display_name, &lv_font_montserrat_24, 0xf2f7ffU, 24, 20);
    lv_obj_set_width(title, 616);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* open = CreateActionButton(sheet, Bounds{.x = 20, .y = 72, .width = 624, .height = 58},
                                        app_management_model_.launch_available ? "Open" : "Open unavailable",
                                        app_management_model_.launch_available ? 0xf2f7ffU : 0x708198U);
    if (app_management_model_.launch_available) {
        lv_obj_add_event_cb(open, AppManagementOpenEvent, LV_EVENT_SHORT_CLICKED, this);
    } else {
        lv_obj_remove_flag(open, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t* information =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 140, .width = 624, .height = 58}, "App Information", 0xf2f7ffU);
    lv_obj_add_event_cb(information, AppManagementInformationEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* uninstall = CreateActionButton(sheet, Bounds{.x = 20, .y = 208, .width = 624, .height = 58}, "Uninstall",
                                             0xff6b74U, 0x653c48U);
    lv_obj_add_event_cb(uninstall, AppManagementUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 286, .width = 624, .height = 54}, "Cancel", 0xf2f7ffU);
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementInformationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* scrim =
        CreatePanel(root_, Bounds{.x = 0, .y = 0, .width = kScreenWidth, .height = kScreenHeight}, 0x030913U, 0U, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 352, .width = 664, .height = 340}, 0x101c2cU, 0x42607fU, 24);
    lv_obj_t* title = CreateLabel(sheet, app.display_name, &lv_font_montserrat_24, 0xf2f7ffU, 24, 20);
    lv_obj_set_width(title, 616);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* details =
        CreatePanel(sheet, Bounds{.x = 20, .y = 66, .width = 624, .height = 174}, 0x111f32U, 0x2e4562U, 18);
    DrawInformationRow(details, 0, "App ID", app.app_id != nullptr ? app.app_id : "Unknown");
    char size[24]{};
    FormatInformationSize(app.bundle_size_kib, size, sizeof(size));
    DrawInformationRow(details, 58, "Bundle Size", size);
    DrawInformationRow(details, 116, "Runtime", "WebAssembly AOT");
    lv_obj_t* done =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 264, .width = 624, .height = 54}, "Done", 0xf2f7ffU);
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallUnavailableLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* scrim =
        CreatePanel(root_, Bounds{.x = 0, .y = 0, .width = kScreenWidth, .height = kScreenHeight}, 0x030913U, 0U, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 420, .width = 664, .height = 272}, 0x101c2cU, 0x42607fU, 24);
    (void)CreateLabel(sheet, "Uninstall unavailable", &lv_font_montserrat_24, 0xf2f7ffU, 24, 20);
    char message[160]{};
    std::snprintf(message, sizeof(message), "%s is stored in the read-only App Store. Storage migration is required.",
                  app.display_name != nullptr ? app.display_name : "This App");
    lv_obj_t* detail = CreateLabel(sheet, message, &lv_font_montserrat_18, 0x91a4bdU, 24, 64);
    lv_obj_set_size(detail, 616, 92);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* done =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 196, .width = 624, .height = 54}, "Done", 0xf2f7ffU);
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallConfirmationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* scrim =
        CreatePanel(root_, Bounds{.x = 0, .y = 0, .width = kScreenWidth, .height = kScreenHeight}, 0x030913U, 0U, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 390, .width = 664, .height = 302}, 0x101c2cU, 0x653c48U, 24);
    (void)CreateLabel(sheet, "Uninstall App?", &lv_font_montserrat_24, 0xf2f7ffU, 24, 20);
    lv_obj_t* name = CreateLabel(sheet, app.display_name, &lv_font_montserrat_18, 0x91a4bdU, 24, 61);
    lv_obj_set_width(name, 616);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t* uninstall = CreateActionButton(sheet, Bounds{.x = 20, .y = 112, .width = 624, .height = 58}, "Uninstall",
                                             0xff6b74U, 0x653c48U);
    lv_obj_add_event_cb(uninstall, AppManagementConfirmUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 190, .width = 624, .height = 58}, "Cancel", 0xf2f7ffU);
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::RenderAppManagementLocked(bool clean_root) {
    if (clean_root) {
        lv_obj_clean(root_);
    }
    app_management_rows_.fill(nullptr);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    (void)DrawDetailHeader(root_, "App Management", "Installed applications", AppManagementBackEvent, this);

    const int32_t content_height = std::max<int32_t>(
        kScreenHeight - 108,
        106 + static_cast<int32_t>(std::max<uint32_t>(1U, app_management_model_.app_count)) * 116 + 30);
    lv_obj_t* scroll_content = CreateDetailScrollContent(root_, content_height, DetailScrollEvent, this);
    char count[48]{};
    if (app_management_model_.app_count == 1U) {
        std::snprintf(count, sizeof(count), "1 App");
    } else {
        std::snprintf(count, sizeof(count), "%" PRIu32 " Apps", app_management_model_.app_count);
    }
    (void)CreateLabel(scroll_content, count, &lv_font_montserrat_24, 0xf2f7ffU, 42, 14);
    (void)CreateLabel(scroll_content, "Installed on this device", &lv_font_montserrat_18, 0x91a4bdU, 42, 49);
    char used[24]{};
    char total[24]{};
    char storage[56]{};
    FormatInformationSize(app_management_model_.storage_used_kib, used, sizeof(used));
    FormatInformationSize(app_management_model_.storage_total_kib, total, sizeof(total));
    if (app_management_model_.storage_total_kib != 0U) {
        std::snprintf(storage, sizeof(storage), "%s / %s", used, total);
    } else {
        std::snprintf(storage, sizeof(storage), "%s used", used);
    }
    lv_obj_t* used_label = CreateLabel(scroll_content, storage, &lv_font_montserrat_18, 0x91a4bdU, 448, 27);
    lv_obj_set_width(used_label, 230);
    lv_obj_set_style_text_align(used_label, LV_TEXT_ALIGN_RIGHT, 0);

    constexpr std::array<uint32_t, 3U> kAppIconColors{0x2c7867U, 0x305f9eU, 0x76539bU};
    if (app_management_model_.app_count == 0U) {
        lv_obj_t* empty = CreatePanel(scroll_content, Bounds{.x = 40, .y = 90, .width = 640, .height = 104}, 0x111f32U,
                                      0x2e4562U, 22);
        (void)CreateLabel(empty, "No installed Apps", &lv_font_montserrat_18, 0x91a4bdU, 22, 39);
    } else {
        for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
            const auto& app = app_management_model_.apps[index];
            const int32_t y = 90 + static_cast<int32_t>(index) * 116;
            lv_obj_t* row = CreatePanel(scroll_content, Bounds{.x = 40, .y = y, .width = 640, .height = 104}, 0x111f32U,
                                        0x2e4562U, 22);
            app_management_rows_[index] = row;
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x091522U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
            lv_obj_add_event_cb(row, AppManagementRowEvent, LV_EVENT_SHORT_CLICKED, this);
            lv_obj_t* icon = CreatePanel(row, Bounds{.x = 16, .y = 20, .width = 64, .height = 64},
                                         kAppIconColors[index % kAppIconColors.size()], 0U, 17);
            char initial[2]{'?', '\0'};
            if (app.display_name != nullptr && app.display_name[0] != '\0') {
                initial[0] = app.display_name[0];
            }
            lv_obj_t* initial_label = lv_label_create(icon);
            lv_label_set_text(initial_label, initial);
            lv_obj_set_style_text_font(initial_label, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(initial_label, lv_color_white(), 0);
            lv_obj_center(initial_label);
            lv_obj_t* name = CreateLabel(row, app.display_name != nullptr ? app.display_name : "Unknown App",
                                         &lv_font_montserrat_24, 0xf2f7ffU, 98, 20);
            lv_obj_set_width(name, 438);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            char metadata[128]{};
            char size[24]{};
            FormatInformationSize(app.bundle_size_kib, size, sizeof(size));
            std::snprintf(metadata, sizeof(metadata), "%s   %s", app.app_id != nullptr ? app.app_id : "Unknown", size);
            lv_obj_t* metadata_label = CreateLabel(row, metadata, &lv_font_montserrat_18, 0x91a4bdU, 98, 59);
            lv_obj_set_width(metadata_label, 438);
            lv_label_set_long_mode(metadata_label, LV_LABEL_LONG_DOT);
            DrawAppMoreIndicator(row);
        }
    }
    lv_obj_update_layout(scroll_content);
    switch (app_management_overlay_) {
        case AppOverlay::kActions:
            DrawAppManagementActionsLocked();
            break;
        case AppOverlay::kInformation:
            DrawAppManagementInformationLocked();
            break;
        case AppOverlay::kUninstallConfirmation:
            DrawAppManagementUninstallConfirmationLocked();
            break;
        case AppOverlay::kUninstallUnavailable:
            DrawAppManagementUninstallUnavailableLocked();
            break;
        case AppOverlay::kNone:
        default:
            break;
    }
    lv_obj_move_foreground(root_);
    lv_timer_ready(lv_display_get_refr_timer(lv_obj_get_display(root_)));
}

}  // namespace micropixel::platform::metalio_claw4
