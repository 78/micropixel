#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "host/ui/lvgl/square_common/system_detail_ui.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui_internal.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {
using system_detail_internal::Button;
using system_detail_internal::CreateActionSheet;
using system_detail_internal::FormatSize;
using system_detail_internal::Header;
using system_detail_internal::InformationRow;
using system_detail_internal::kTag;
using system_detail_internal::Label;
using system_detail_internal::Panel;
using system_detail_internal::Scroll;
}  // namespace

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowAppManagementLocked(
    lv_obj_t* root, const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    ResetActiveScreen();
    root_ = root;
    app_management_model_ = model;
    app_management_selected_index_ = 0U;
    app_management_overlay_ = AppOverlay::kNone;
    action_sink_ = action_sink;
    action_context_ = action_context;
    active_screen_ = Screen::kAppManagement;
    lv_display_t* display = lv_obj_get_display(root_);
    if (display != nullptr && app_management_probe_display_ == nullptr) {
        app_management_probe_display_ = display;
        lv_display_add_event_cb(display, AppManagementDisplayEvent, LV_EVENT_RENDER_READY, this);
        lv_display_add_event_cb(display, AppManagementDisplayEvent, LV_EVENT_REFR_READY, this);
    }
    RenderAppManagementLocked();
    return {};
}

void SystemDetailUi::RenderAppManagementLocked() {
    if (!AppManagementVisible() || root_ == nullptr) {
        return;
    }
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::kMenuBackground), 0);
    Header(layout_, root_, "App Management", "Installed apps and storage", AppManagementBackEvent, this);
    lv_obj_t* scroll = Scroll(layout_, root_, ScrollEvent, this);
    char storage[80]{};
    const uint32_t used_tenths =
        static_cast<uint32_t>((static_cast<uint64_t>(app_management_model_.storage_used_kib) * 10U) / 1024U);
    const uint32_t total_tenths =
        static_cast<uint32_t>((static_cast<uint64_t>(app_management_model_.storage_total_kib) * 10U) / 1024U);
    std::snprintf(storage, sizeof(storage), "Storage %" PRIu32 ".%" PRIu32 " / %" PRIu32 ".%" PRIu32 " MB",
                  used_tenths / 10U, used_tenths % 10U, total_tenths / 10U, total_tenths % 10U);
    (void)Label(scroll, storage, platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
    app_bindings_ = {};

    if (app_management_model_.app_count == 0U) {
        lv_obj_t* empty = Panel(layout_, scroll);
        (void)Label(empty, "No apps installed", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    }
    for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
        const host_ui::InstalledAppModel& app = app_management_model_.apps[index];
        app_bindings_[index] = {.ui = this, .index = index};
        lv_obj_t* row = CreateSystemButtonPanel(scroll, layout_);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(theme::kPressedBackground),
                                  static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_add_event_cb(row, AppManagementRowEvent, LV_EVENT_SHORT_CLICKED, &app_bindings_[index]);
        lv_obj_t* app_text = square_common::CreateSystemColumn(row, 4);
        lv_obj_set_width(app_text, 0);
        lv_obj_set_flex_grow(app_text, 1);
        lv_obj_t* name = Label(app_text, app.display_name, platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char size[28]{};
        FormatSize(app.bundle_size_kib, size, sizeof(size));
        (void)Label(app_text, size, platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);

        (void)square_common::CreateSystemMoreIndicator(row, 32, 52, 6, 5);
    }
    RenderAppManagementOverlayLocked();
    lv_obj_move_foreground(root_);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::RenderAppManagementOverlayLocked() {
    if (!AppManagementVisible() || root_ == nullptr) {
        return;
    }
    if (app_management_overlay_root_ != nullptr) {
        lv_obj_delete(app_management_overlay_root_);
        app_management_overlay_root_ = nullptr;
    }
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
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::LeaveAppManagement() {
    if (AppManagementVisible()) {
        app_management_model_ = {};
        app_bindings_ = {};
        ResetActiveScreen();
    }
}

bool SystemDetailUi::AppManagementVisible() const { return active_screen_ == Screen::kAppManagement; }

void* SystemDetailUi::AppManagementActionContext() const { return action_context_; }

void SystemDetailUi::AppManagementBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseAppManagement});
    }
}

void SystemDetailUi::AppManagementRenderAsync(void* context) {
    auto* ui = static_cast<SystemDetailUi*>(context);
    if (ui != nullptr && ui->AppManagementVisible()) {
        ui->StartAppManagementLatencyProbe();
        ui->RenderAppManagementOverlayLocked();
    }
}

void SystemDetailUi::BeginAppManagementLatencyProbe(const char* operation) {
    app_management_probe_operation_ = operation;
    app_management_probe_touch_us_ = esp_timer_get_time();
    app_management_probe_render_start_us_ = 0;
    app_management_probe_render_ready_us_ = 0;
    app_management_probe_armed_ = false;
}

void SystemDetailUi::StartAppManagementLatencyProbe() {
    if (app_management_probe_touch_us_ == 0) {
        return;
    }
    app_management_probe_render_start_us_ = esp_timer_get_time();
    app_management_probe_armed_ = true;
}

void SystemDetailUi::AppManagementDisplayEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || !ui->app_management_probe_armed_) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (lv_event_get_code(event) == LV_EVENT_RENDER_READY) {
        if (ui->app_management_probe_render_ready_us_ == 0) {
            ui->app_management_probe_render_ready_us_ = now;
        }
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_REFR_READY || ui->app_management_probe_render_ready_us_ == 0) {
        return;
    }
    const int64_t queue_us = ui->app_management_probe_render_start_us_ - ui->app_management_probe_touch_us_;
    const int64_t render_us = ui->app_management_probe_render_ready_us_ - ui->app_management_probe_render_start_us_;
    const int64_t refresh_us = now - ui->app_management_probe_render_ready_us_;
    const int64_t total_us = now - ui->app_management_probe_touch_us_;
    ESP_LOGI(kTag,
             "latency %s: queue=%" PRId64 ".%03" PRId64 " render=%" PRId64 ".%03" PRId64 " flush=%" PRId64 ".%03" PRId64
             " total=%" PRId64 ".%03" PRId64 " ms",
             ui->app_management_probe_operation_ != nullptr ? ui->app_management_probe_operation_ : "unknown",
             queue_us / 1000, queue_us % 1000, render_us / 1000, render_us % 1000, refresh_us / 1000, refresh_us % 1000,
             total_us / 1000, total_us % 1000);
    ui->app_management_probe_touch_us_ = 0;
    ui->app_management_probe_armed_ = false;
}

void SystemDetailUi::QueueAppManagementRender() {
    if (lv_async_call(AppManagementRenderAsync, this) != LV_RESULT_OK) {
        ESP_LOGW(kTag, "failed to queue App Management render");
    }
}

void SystemDetailUi::AppManagementRowEvent(lv_event_t* event) {
    auto* binding = static_cast<AppBinding*>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->ui == nullptr ||
        binding->index >= binding->ui->app_management_model_.app_count) {
        return;
    }
    binding->ui->app_management_selected_index_ = binding->index;
    binding->ui->app_management_overlay_ = AppOverlay::kActions;
    binding->ui->BeginAppManagementLatencyProbe("actions.open");
    binding->ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementCancelEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->app_management_overlay_ = AppOverlay::kNone;
        ui->BeginAppManagementLatencyProbe("sheet.close");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementOpenEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr &&
        ui->app_management_selected_index_ < ui->app_management_model_.app_count &&
        ui->app_management_model_.launch_available) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchInstalledApp,
                                                 .app_index = ui->app_management_selected_index_});
    }
}

void SystemDetailUi::AppManagementInformationEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->app_management_overlay_ = AppOverlay::kInformation;
        ui->BeginAppManagementLatencyProbe("information.open");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->app_management_overlay_ = ui->app_management_model_.uninstall_available ? AppOverlay::kUninstallConfirmation
                                                                                    : AppOverlay::kUninstallUnavailable;
        ui->BeginAppManagementLatencyProbe("uninstall.open");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementConfirmUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr && ui->app_management_model_.uninstall_available &&
        ui->app_management_selected_index_ < ui->app_management_model_.app_count) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kUninstallInstalledApp,
                                                 .app_index = ui->app_management_selected_index_});
    }
}

void SystemDetailUi::DrawAppManagementActionsLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, theme::kStrongBorder,
                                        &app_management_overlay_root_);
    lv_obj_t* title = Label(sheet, app.display_name, platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* open = Button(layout_, sheet, app_management_model_.launch_available ? "Open" : "Open unavailable",
                            app_management_model_.launch_available ? theme::kPrimaryText : theme::kDisabledText);
    if (app_management_model_.launch_available) {
        lv_obj_add_event_cb(open, AppManagementOpenEvent, LV_EVENT_SHORT_CLICKED, this);
    } else {
        lv_obj_remove_flag(open, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(open, LV_OPA_40, 0);
    }
    lv_obj_t* information = Button(layout_, sheet, "App Information");
    lv_obj_add_event_cb(information, AppManagementInformationEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* uninstall = Button(layout_, sheet, "Uninstall", theme::kDanger);
    lv_obj_add_event_cb(uninstall, AppManagementUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel", theme::kSecondaryText);
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementInformationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, theme::kStrongBorder,
                                        &app_management_overlay_root_);
    lv_obj_t* title = Label(sheet, app.display_name, platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* details = Panel(layout_, sheet, 0);
    InformationRow(layout_, details, "App ID", app.app_id != nullptr ? app.app_id : "Unknown");
    char size[24]{};
    FormatSize(app.bundle_size_kib, size, sizeof(size));
    InformationRow(layout_, details, "Bundle Size", size);
    InformationRow(layout_, details, "Runtime", "WebAssembly AOT");
    lv_obj_t* done = Button(layout_, sheet, "Done");
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallUnavailableLocked() {
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, theme::kStrongBorder,
                                        &app_management_overlay_root_);
    (void)Label(sheet, "Uninstall unavailable", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_t* detail = Label(sheet, "Close the running App from the Hall before uninstalling Apps.",
                             platform::lvgl::SystemFontRole::kMedium, theme::kSecondaryText);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* done = Button(layout_, sheet, "Done");
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallConfirmationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, theme::kDangerBorder,
                                        &app_management_overlay_root_);
    (void)Label(sheet, "Uninstall App?", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_t* name = Label(sheet, app.display_name, platform::lvgl::SystemFontRole::kMedium, theme::kSecondaryText);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t* uninstall = Button(layout_, sheet, "Uninstall", theme::kDanger);
    lv_obj_add_event_cb(uninstall, AppManagementConfirmUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel");
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

}  // namespace micropixel::host_ui::lvgl::square_common
