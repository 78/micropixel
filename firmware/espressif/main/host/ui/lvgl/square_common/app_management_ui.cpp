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
using system_detail_internal::kTag;
using system_detail_internal::Label;
using system_detail_internal::Panel;
using system_detail_internal::Scroll;

void AppSizeRow(lv_obj_t* parent, const char* text, uint32_t size_kib, platform::lvgl::SystemFontRole text_role,
                uint32_t text_color) {
    lv_obj_t* row = CreateSystemColumn(parent, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t* label = Label(row, text, text_role, text_color);
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    char size[28]{};
    FormatSize(size_kib, size, sizeof(size));
    (void)Label(row, size, platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
}

void FormatUsageCapacity(const host_ui::StorageUsageModel& usage, char* output, size_t capacity) {
    const uint32_t used_tenths = static_cast<uint32_t>((static_cast<uint64_t>(usage.used_kib) * 10U) / 1024U);
    const uint32_t total_tenths = static_cast<uint32_t>((static_cast<uint64_t>(usage.total_kib) * 10U) / 1024U);
    std::snprintf(output, capacity, "%" PRIu32 ".%" PRIu32 " / %" PRIu32 ".%" PRIu32 " MB", used_tenths / 10U,
                  used_tenths % 10U, total_tenths / 10U, total_tenths % 10U);
}

void SectionTitleWithUsage(lv_obj_t* parent, const char* title, const host_ui::StorageUsageModel& usage) {
    lv_obj_t* row = CreateSystemColumn(parent, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t* label = Label(row, title, platform::lvgl::SystemFontRole::kSmall, theme::kMutedText);
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    char capacity[40]{};
    FormatUsageCapacity(usage, capacity, sizeof(capacity));
    (void)Label(row, capacity, platform::lvgl::SystemFontRole::kSmall, theme::kMutedText);
}

const char* AppManagementSubtitle(uint8_t store_check_state) {
    if (store_check_state == 1U) {
        return "Update check pending; keep device online";
    }
    if (store_check_state != 0U && store_check_state != 2U) {
        return "Update check failed; reopen to retry";
    }
    return "Installed apps and storage";
}

const char* ExternalStorageStatusText(host_ui::ExternalStorageStatus status) {
    switch (status) {
        case host_ui::ExternalStorageStatus::kNotFormatted:
            return "Extension storage is not formatted";
        case host_ui::ExternalStorageStatus::kUnsupportedFormat:
            return "Extension storage has an incompatible format";
        case host_ui::ExternalStorageStatus::kCorrupt:
            return "Extension storage is damaged";
        case host_ui::ExternalStorageStatus::kUnavailable:
            return "Extension storage is unavailable";
        case host_ui::ExternalStorageStatus::kReady:
        case host_ui::ExternalStorageStatus::kAbsent:
        default:
            return "";
    }
}
}  // namespace

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowAppManagementLocked(
    lv_obj_t* root, const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    // The Host re-shows the same screen when the Store update check changes
    // state; an open action or confirmation sheet must survive that refresh
    // as long as the App it refers to is still listed.
    const bool refresh = AppManagementVisible() && root_ == root;
    const uint32_t previous_index = app_management_selected_index_;
    const AppOverlay previous_overlay = app_management_overlay_;
    ResetActiveScreen();
    root_ = root;
    app_management_model_ = model;
    app_management_selected_index_ = 0U;
    app_management_overlay_ = AppOverlay::kNone;
    if (refresh && previous_overlay != AppOverlay::kNone) {
        const bool app_sheet = previous_overlay == AppOverlay::kActions ||
                               previous_overlay == AppOverlay::kUninstallConfirmation ||
                               previous_overlay == AppOverlay::kUninstallUnavailable;
        if (!app_sheet || previous_index < model.app_count) {
            app_management_selected_index_ = app_sheet ? previous_index : 0U;
            app_management_overlay_ = previous_overlay;
        }
    }
    action_sink_ = action_sink;
    action_context_ = action_context;
    active_screen_ = Screen::kAppManagement;
    lv_display_t* display = lv_obj_get_display(root_);
    if (display != nullptr && app_management_probe_display_ == nullptr) {
        app_management_probe_display_ = display;
        lv_display_add_event_cb(display, AppManagementDisplayEvent, LV_EVENT_RENDER_READY, this);
        lv_display_add_event_cb(display, AppManagementDisplayEvent, LV_EVENT_REFR_READY, this);
    }
    if (model.action_app_index < model.app_count) {
        // Opened directly from the Hall: only the sheet is drawn over it.
        app_management_selected_index_ = model.action_app_index;
        if (app_management_overlay_ == AppOverlay::kNone) {
            app_management_overlay_ = AppOverlay::kActions;
        }
        RenderAppManagementOverlayLocked();
    } else {
        RenderAppManagementLocked();
    }
    return {};
}

void SystemDetailUi::RenderAppManagementLocked() {
    if (!AppManagementVisible() || root_ == nullptr) {
        return;
    }
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::kMenuBackground), 0);
    Header(layout_, root_, "App Management", AppManagementSubtitle(app_management_model_.store_check_state),
           AppManagementBackEvent, this);
    lv_obj_t* scroll = Scroll(layout_, root_, ScrollEvent, this);
    app_bindings_ = {};

    const auto status = app_management_model_.external_storage_status;
    if (status == host_ui::ExternalStorageStatus::kAbsent) {
        SectionTitleWithUsage(scroll, "STORAGE",
                              host_ui::StorageUsageModel{.used_kib = app_management_model_.storage_used_kib,
                                                         .total_kib = app_management_model_.storage_total_kib});
    } else if (status != host_ui::ExternalStorageStatus::kReady) {
        DrawAppManagementFormatRowLocked(scroll);
    }

    if (app_management_model_.app_count == 0U) {
        if (status == host_ui::ExternalStorageStatus::kReady) {
            DrawAppManagementGroupLocked(scroll, "EXTENSION STORAGE", app_management_model_.external_storage, true);
            DrawAppManagementGroupLocked(scroll, "SYSTEM STORAGE", app_management_model_.system_storage, false);
        } else if (status != host_ui::ExternalStorageStatus::kAbsent) {
            DrawAppManagementGroupLocked(scroll, "SYSTEM STORAGE", app_management_model_.system_storage, false);
        }
        lv_obj_t* empty = Panel(layout_, scroll);
        (void)Label(empty, "No apps installed", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    } else if (status == host_ui::ExternalStorageStatus::kAbsent) {
        for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
            DrawAppManagementRowLocked(scroll, index);
        }
    } else {
        // Two media: downloaded Apps on the extension storage lead, factory
        // Apps on system storage follow, matching the catalog order. Capacity
        // sits on the section title so it does not need its own row.
        if (status == host_ui::ExternalStorageStatus::kReady) {
            DrawAppManagementGroupLocked(scroll, "EXTENSION STORAGE", app_management_model_.external_storage, true);
        }
        DrawAppManagementGroupLocked(scroll, "SYSTEM STORAGE", app_management_model_.system_storage, false);
    }
    RenderAppManagementOverlayLocked();
    lv_obj_move_foreground(root_);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::DrawAppManagementFormatRowLocked(lv_obj_t* scroll) {
    // Not ready: one tappable row explains the state and leads to the
    // user-confirmed format. Capacity for the system store still appears on
    // its section title below.
    lv_obj_t* row = CreateSystemButtonPanel(scroll, layout_);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(theme::kPressedBackground),
                              static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_event_cb(row, AppManagementStorageEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* text = square_common::CreateSystemColumn(row, 4);
    lv_obj_set_width(text, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_t* title = Label(text, ExternalStorageStatusText(app_management_model_.external_storage_status),
                            platform::lvgl::SystemFontRole::kMedium, theme::kPrimaryText);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    (void)Label(text, "Tap to format it for Apps", platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
    (void)square_common::CreateSystemMoreIndicator(row, 32, 52, 6, 5);
}

void SystemDetailUi::DrawAppManagementGroupLocked(lv_obj_t* scroll, const char* title,
                                                  const host_ui::StorageUsageModel& usage, bool external_storage) {
    SectionTitleWithUsage(scroll, title, usage);
    for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
        if (app_management_model_.apps[index].external_storage != external_storage) {
            continue;
        }
        DrawAppManagementRowLocked(scroll, index);
    }
}

void SystemDetailUi::DrawAppManagementRowLocked(lv_obj_t* scroll, uint32_t index) {
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
    AppSizeRow(app_text, system_detail_internal::DisplayText(app.app_id, "Unknown"), app.bundle_size_kib,
               platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);

    char version[112]{};
    if (app.update_version[0] != '\0')
        std::snprintf(version, sizeof(version), "%s -> %s available", app.version != nullptr ? app.version : "?",
                      app.update_version.data());
    else
        std::snprintf(version, sizeof(version), "Version %s",
                      app.version != nullptr && app.version[0] != '\0' ? app.version : "unknown");
    (void)Label(app_text, version, platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
    (void)square_common::CreateSystemMoreIndicator(row, 32, 52, 6, 5);
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
        case AppOverlay::kUninstallConfirmation:
            DrawAppManagementUninstallConfirmationLocked();
            break;
        case AppOverlay::kUninstallUnavailable:
            DrawAppManagementUninstallUnavailableLocked();
            break;
        case AppOverlay::kFormatConfirmation:
            DrawAppManagementFormatConfirmationLocked();
            break;
        case AppOverlay::kFormatUnavailable:
            DrawAppManagementFormatUnavailableLocked();
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
        if (ui->app_management_model_.action_app_index < ui->app_management_model_.app_count) {
            AppManagementBackEvent(event);
            return;
        }
        ui->app_management_overlay_ = AppOverlay::kNone;
        ui->BeginAppManagementLatencyProbe("sheet.close");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementUpdateEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr &&
        ui->app_management_selected_index_ < ui->app_management_model_.app_count)
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kUpdateInstalledApp,
                                                 .app_index = ui->app_management_selected_index_});
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

void SystemDetailUi::AppManagementStorageEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->app_management_overlay_ = ui->app_management_model_.format_available ? AppOverlay::kFormatConfirmation
                                                                                 : AppOverlay::kFormatUnavailable;
        ui->BeginAppManagementLatencyProbe("format.open");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementConfirmFormatEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr && ui->app_management_model_.format_available &&
        ui->app_management_model_.external_storage_status != host_ui::ExternalStorageStatus::kAbsent) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kFormatExternalStorage});
    }
}

void SystemDetailUi::DrawAppManagementFormatConfirmationLocked() {
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, theme::kDangerBorder,
                                        &app_management_overlay_root_);
    (void)Label(sheet, "Format extension storage?", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_t* detail =
        Label(sheet, "Everything on it is erased and it becomes App storage. Apps on system storage are kept.",
              platform::lvgl::SystemFontRole::kMedium, theme::kSecondaryText);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* format = Button(layout_, sheet, "Format", theme::kDanger);
    lv_obj_add_event_cb(format, AppManagementConfirmFormatEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel");
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementFormatUnavailableLocked() {
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, theme::kStrongBorder,
                                        &app_management_overlay_root_);
    (void)Label(sheet, "Format unavailable", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_t* detail = Label(sheet, "Close the running App from the Hall before formatting storage.",
                             platform::lvgl::SystemFontRole::kMedium, theme::kSecondaryText);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* done = Button(layout_, sheet, "Done");
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementActionsLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, theme::kStrongBorder,
                                        &app_management_overlay_root_);
    AppSizeRow(sheet, app.display_name, app.bundle_size_kib, platform::lvgl::SystemFontRole::kLarge,
               theme::kPrimaryText);
    if (app_management_model_.external_storage_status != host_ui::ExternalStorageStatus::kAbsent) {
        (void)Label(sheet, app.external_storage ? "On extension storage" : "On system storage",
                    platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
    }
    if (app.update_version[0] != '\0' && app_management_model_.action_app_index < app_management_model_.app_count) {
        char title[80]{};
        std::snprintf(title, sizeof(title), "Install %s and run", app.update_version.data());
        lv_obj_t* update = Button(layout_, sheet, title, theme::kPrimaryText);
        lv_obj_add_event_cb(update, AppManagementUpdateEvent, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_t* current = Button(layout_, sheet, "Run current version", theme::kPrimaryText);
        lv_obj_add_event_cb(current, AppManagementOpenEvent, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_t* cancel = Button(layout_, sheet, "Cancel", theme::kSecondaryText);
        lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
        return;
    }
    lv_obj_t* open = Button(layout_, sheet, app_management_model_.launch_available ? "Open" : "Open unavailable",
                            app_management_model_.launch_available ? theme::kPrimaryText : theme::kDisabledText);
    if (app_management_model_.launch_available) {
        lv_obj_add_event_cb(open, AppManagementOpenEvent, LV_EVENT_SHORT_CLICKED, this);
    } else {
        lv_obj_remove_flag(open, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(open, LV_OPA_40, 0);
    }
    if (app.update_version[0] != '\0') {
        char title[64]{};
        std::snprintf(title, sizeof(title), "Update to %s", app.update_version.data());
        lv_obj_t* update = Button(layout_, sheet, title, theme::kPrimaryText);
        lv_obj_add_event_cb(update, AppManagementUpdateEvent, LV_EVENT_SHORT_CLICKED, this);
    }
    lv_obj_t* uninstall = Button(layout_, sheet, "Uninstall", theme::kDanger);
    lv_obj_add_event_cb(uninstall, AppManagementUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel", theme::kSecondaryText);
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
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
