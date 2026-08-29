#include "host/ui/lvgl/square_common/square_ui_state.hpp"

#include <algorithm>

#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

void StyleFullscreen(lv_obj_t* object, uint32_t background, int32_t width, int32_t height) {
    lv_obj_set_pos(object, 0, 0);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
}

uint32_t BitmapPixelRgb888(const device::BitmapView& bitmap, uint32_t x, uint32_t y) {
    const uint8_t* pixel = bitmap.data + static_cast<size_t>(y) * bitmap.stride + static_cast<size_t>(x) * 3U;
    return (static_cast<uint32_t>(pixel[2]) << 16U) | (static_cast<uint32_t>(pixel[1]) << 8U) |
           static_cast<uint32_t>(pixel[0]);
}

uint32_t LaunchBackgroundRgb888(const device::BitmapView& bitmap) {
    const uint32_t top_left = BitmapPixelRgb888(bitmap, 0U, 0U);
    const uint32_t top_right = BitmapPixelRgb888(bitmap, bitmap.width - 1U, 0U);
    const uint32_t bottom_left = BitmapPixelRgb888(bitmap, 0U, bitmap.height - 1U);
    const uint32_t bottom_right = BitmapPixelRgb888(bitmap, bitmap.width - 1U, bitmap.height - 1U);
    if (top_left == top_right || top_left == bottom_left || top_left == bottom_right) {
        return top_left;
    }
    if (top_right == bottom_left || top_right == bottom_right) {
        return top_right;
    }
    return bottom_left == bottom_right ? bottom_left : top_left;
}

}  // namespace

SquareSystemUiState::SquareSystemUiState(device::Input& physical_input,
                                         platform::lvgl::GuestGraphicsEngine& guest_graphics,
                                         StatusLayerTransition& transition,
                                         const SquareSystemUiProfile& selected_profile)
    : profile(selected_profile),
      system_detail_ui(profile.system_page),
      input_router(physical_input, static_cast<uint16_t>(profile.square.width),
                   static_cast<uint16_t>(profile.square.height)),
      hall_cover_cache({.target_size = profile.square.hall_card_width,
                        .corner_radius = static_cast<uint32_t>(profile.hall_card.radius),
                        .top_background_rgb = theme::kHallBackground,
                        .bottom_background_rgb = theme::kHallCardBackground}),
      guest_graphics_(guest_graphics),
      transition_(transition) {}

void SquareSystemUiState::BindHallReset(ResetHallCallback reset, void* context) {
    reset_hall_locked_ = reset;
    reset_hall_context_ = context;
}

void SquareSystemUiState::BindThemeChanged(ThemeChangedCallback changed, void* context) {
    theme_changed_locked_ = changed;
    theme_changed_context_ = context;
}

void SquareSystemUiState::BindBeforeLaunchPresentation(PrepareHardwareCallback prepare_locked, void* context) {
    before_launch_presentation_locked_ = prepare_locked;
    before_launch_presentation_context_ = context;
}

void SquareSystemUiState::BindBeforeRootRelease(PrepareHardwareCallback prepare_locked, void* context) {
    before_root_release_locked_ = prepare_locked;
    before_root_release_context_ = context;
}

void SquareSystemUiState::BindBackgroundExecutor(work::BackgroundExecutor& executor) {
    hall_cover_cache.BindBackgroundExecutor(executor);
}

esp_err_t SquareSystemUiState::InitializeLocked(lv_display_t* initialized_display) {
    if (initialized_display == nullptr || display != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    display = initialized_display;
    if (!theme::Install(display)) {
        display = nullptr;
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t status = host_pointer.InitializeLocked(
        display, [](void* context) { static_cast<WifiSettingsUi*>(context)->PointerReleased(); }, &wifi_settings_ui);
    if (status != ESP_OK) {
        display = nullptr;
        return status;
    }
    ShowStartingScreenLocked();
    return ESP_OK;
}

platform::lvgl::GuestGraphicsHooks SquareSystemUiState::GraphicsHooks() {
    return {
        .context = this,
        .show_launch_bitmap =
            [](void* context, const device::BitmapView& bitmap) {
                return static_cast<SquareSystemUiState*>(context)->ShowLaunchBitmap(bitmap);
            },
        .dismiss_launch_bitmap =
            [](void* context) { (void)static_cast<SquareSystemUiState*>(context)->DismissLaunchBitmap(); },
    };
}

platform::lvgl::GuestPresentationHooks SquareSystemUiState::GuestFrameHooks() {
    return {
        .context = this,
        .prepare_frame_locked =
            [](void* context, lv_obj_t* guest_frame, bool created_guest_frame, bool& needs_present) {
                static_cast<SquareSystemUiState*>(context)->PrepareGuestFrameLocked(guest_frame, created_guest_frame,
                                                                                    needs_present);
            },
    };
}

lv_obj_t* SquareSystemUiState::EnsureRootLocked(uint32_t background) {
    if (root == nullptr) {
        root = lv_obj_create(lv_screen_active());
        if (root != nullptr) {
            StyleFullscreen(root, background, static_cast<int32_t>(profile.square.width),
                            static_cast<int32_t>(profile.square.height));
        }
    }
    return root;
}

lv_obj_t* SquareSystemUiState::PrepareSystemPageRootLocked() {
    lv_obj_t* page_root = EnsureRootLocked(theme::kMenuBackground);
    if (page_root == nullptr) {
        return nullptr;
    }
    ResetHallLocked();
    lv_obj_clean(page_root);
    lv_obj_set_style_bg_color(page_root, lv_color_hex(theme::kMenuBackground), 0);
    launch_image_descriptor = {};
    return page_root;
}

void SquareSystemUiState::DeleteRootLocked() {
    if (root != nullptr) {
        ResetHallLocked();
        lv_obj_delete(root);
        root = nullptr;
    }
    hall_scene_ui.ResetLocked();
    launch_image_descriptor = {};
}

void SquareSystemUiState::ResetHallPresentationLocked() { ResetHallLocked(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowShutdown(PrepareHardwareCallback prepare_locked,
                                                                              void* prepare_context) {
    if (display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    hall_cover_cache.Pause();
    SetHostPointerEnabledLocked(false);
    if (prepare_locked != nullptr) {
        prepare_locked(prepare_context);
    }
    UnbindHostPointerTouchSink();
    hall_action_sink = nullptr;
    hall_action_context = nullptr;
    if (EnsureRootLocked(theme::kLoadingBackground) == nullptr) {
        esp_lv_adapter_unlock();
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    ResetHallLocked();
    lv_obj_clean(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme::kLoadingBackground), 0);
    lv_obj_t* label = lv_label_create(root);
    lv_label_set_text(label, "Shutting down...");
    lv_obj_set_style_text_color(label, lv_color_hex(theme::kLoadingText), 0);
    lv_obj_set_style_text_font(label, platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kTitle), 0);
    lv_obj_center(label);
    lv_obj_move_foreground(root);
    platform::lvgl::RequestDisplayRefresh(display);
    esp_lv_adapter_unlock();
    return {};
}

void SquareSystemUiState::ShowStartingScreenLocked() {
    lv_obj_t* starting_root = EnsureRootLocked(theme::kLoadingBackground);
    if (starting_root != nullptr) {
        ResetHallLocked();
        lv_obj_clean(starting_root);
        lv_obj_set_style_bg_color(starting_root, lv_color_hex(theme::kLoadingBackground), 0);
        lv_obj_t* label = lv_label_create(starting_root);
        lv_label_set_text(label, "Starting MicroPixel...");
        lv_obj_set_style_text_color(label, lv_color_hex(theme::kLoadingText), 0);
        lv_obj_set_style_text_font(label, platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kTitle), 0);
        lv_obj_center(label);
        platform::lvgl::RequestDisplayRefresh(display);
    }
}

int32_t SquareSystemUiState::ShowLaunchBitmap(const device::BitmapView& bitmap) {
    const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 3U;
    const uint32_t maximum_dimension = std::max(bitmap.width, bitmap.height);
    if (display == nullptr || root == nullptr || bitmap.data == nullptr ||
        (!esp_ptr_in_drom(bitmap.data) && !esp_ptr_external_ram(bitmap.data)) ||
        (bitmap.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
         bitmap.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888) ||
        bitmap.width == 0U || bitmap.height == 0U ||
        (!profile.scale_oversized_launch_bitmap &&
         (bitmap.width > profile.square.width || bitmap.height > profile.square.height)) ||
        bitmap.stride != bitmap.width * bytes_per_pixel || bitmap.size != bitmap.stride * bitmap.height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (esp_ptr_external_ram(bitmap.data)) {
        lv_draw_buf_t draw_buffer{};
        if (lv_draw_buf_init(&draw_buffer, bitmap.width, bitmap.height,
                             bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? LV_COLOR_FORMAT_ARGB8888
                                                                                     : LV_COLOR_FORMAT_RGB888,
                             bitmap.stride, const_cast<uint8_t*>(bitmap.data), bitmap.size) != LV_RESULT_OK) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        lv_draw_buf_flush_cache(&draw_buffer, nullptr);
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (before_launch_presentation_locked_ != nullptr) {
        before_launch_presentation_locked_(before_launch_presentation_context_);
    }
    ResetHallLocked();
    lv_obj_clean(root);
    const uint32_t background =
        profile.derive_launch_background && bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
            ? LaunchBackgroundRgb888(bitmap)
            : theme::kLoadingBackground;
    lv_obj_set_style_bg_color(root, lv_color_hex(background), 0);
    launch_image_descriptor = {};
    launch_image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    launch_image_descriptor.header.cf =
        bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB888;
    launch_image_descriptor.header.w = bitmap.width;
    launch_image_descriptor.header.h = bitmap.height;
    launch_image_descriptor.header.stride = bitmap.stride;
    launch_image_descriptor.data_size = bitmap.size;
    launch_image_descriptor.data = bitmap.data;
    lv_obj_t* image = lv_image_create(root);
    lv_image_set_src(image, &launch_image_descriptor);
    if (profile.scale_oversized_launch_bitmap && maximum_dimension > profile.square.width) {
        lv_image_set_scale(
            image, static_cast<uint16_t>(256U * static_cast<uint32_t>(profile.square.width) / maximum_dimension));
    }
    lv_obj_center(image);
    lv_obj_t* label = lv_label_create(root);
    lv_label_set_text(label, "Loading...");
    lv_obj_set_style_text_color(label, lv_color_hex(theme::kLoadingText), 0);
    lv_obj_set_style_text_font(label, platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kLarge), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -profile.launch_label_bottom_offset);
    platform::lvgl::RequestDisplayRefresh(display);
    esp_lv_adapter_unlock();
    return MICROPIXEL_STATUS_OK;
}

bool SquareSystemUiState::DismissLaunchBitmap() {
    if (display == nullptr || launch_image_descriptor.data == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return false;
    }
    DeleteRootLocked();
    platform::lvgl::RequestDisplayRefresh(display);
    esp_lv_adapter_unlock();
    return true;
}

void SquareSystemUiState::PrepareGuestFrameLocked(lv_obj_t* guest_frame, bool created_guest_frame,
                                                  bool& needs_present) {
    const bool releasing_root = root != nullptr;
    if (releasing_root) {
        if (before_root_release_locked_ != nullptr) {
            before_root_release_locked_(before_root_release_context_);
        }
        DeleteRootLocked();
        needs_present = true;
    }
    const bool performance_visible = status_layer_ui.PerformanceOverlayVisibleLocked();
    const bool gesture_hint_visible = guest_gesture_hint_ui.VisibleLocked();
    if (performance_visible || gesture_hint_visible) {
        if (created_guest_frame) {
            if (performance_visible) {
                status_layer_ui.RaisePerformanceOverlayLocked();
            }
            if (gesture_hint_visible) {
                guest_gesture_hint_ui.RaiseLocked();
            }
        }
    } else if (guest_frame != nullptr) {
        lv_obj_move_foreground(guest_frame);
    }
}

void SquareSystemUiState::SetHostPointerEnabledLocked(bool enabled) { host_pointer.SetEnabledLocked(enabled); }

bool SquareSystemUiState::HostPointerBusy() { return host_pointer.Busy(); }

void SquareSystemUiState::BindHostPointerTouchSink() { input_router.BindTouchSink(HostPointerTouchSink, this); }

void SquareSystemUiState::UnbindHostPointerTouchSink() { input_router.UnbindTouchSink(this); }

bool SquareSystemUiState::HostPointerTouchSink(void* context, const device::TouchSample& sample) {
    auto* state = static_cast<SquareSystemUiState*>(context);
    return state == nullptr ? false : state->host_pointer.Inject(sample);
}

theme::Mode SquareSystemUiState::ThemeMode(host_ui::SystemThemeMode mode) {
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

void SquareSystemUiState::ResetHallLocked() {
    if (reset_hall_locked_ != nullptr) {
        reset_hall_locked_(reset_hall_context_);
    } else {
        hall_scene_ui.ResetLocked();
    }
}

void SquareSystemUiState::BindPageInput(host_ui::SystemUiActionSink action_sink, void* action_context) {
    BindHostPointerTouchSink();
    input_router.BindSystemActionSink(action_sink, action_context);
}

void SquareSystemUiState::UnbindPageInput(void* action_context) {
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(action_context);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(false);
        esp_lv_adapter_unlock();
    }
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                                host_ui::SystemUiActionSink action_sink,
                                                                                void* action_context) {
    if (display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(hall_action_context);
    hall_action_sink = nullptr;
    hall_action_context = nullptr;
    hall_app_count = 0U;
    hall_launch_enabled = false;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(false);
    lv_obj_t* page_root = PrepareSystemPageRootLocked();
    auto result =
        page_root == nullptr
            ? std::expected<void, host_ui::SystemUiError>(std::unexpected(host_ui::SystemUiError::kRenderFailed))
            : system_menu_ui.ShowLocked(page_root, display, profile.system_menu, model, action_sink, action_context);
    if (result.has_value() && status_layer_ui.PerformanceOverlayVisibleLocked()) {
        status_layer_ui.RaisePerformanceOverlayLocked();
    }
    SetHostPointerEnabledLocked(result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        system_menu_ui.Deactivate();
        return result;
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::UpdateSystemMenu(const host_ui::SystemMenuModel& model) { system_menu_ui.Update(model); }

void SquareSystemUiState::LeaveSystemMenu() {
    if (!system_menu_ui.Active()) {
        return;
    }
    UnbindPageInput(system_menu_ui.ActionContext());
    system_menu_ui.Deactivate();
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowSystemInformation(
    const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    if (display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(system_menu_ui.ActionContext());
    system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(false);
    lv_obj_t* page_root = PrepareSystemPageRootLocked();
    auto result =
        page_root == nullptr
            ? std::expected<void, host_ui::SystemUiError>(std::unexpected(host_ui::SystemUiError::kRenderFailed))
            : system_detail_ui.ShowSystemInformationLocked(page_root, model, action_sink, action_context);
    SetHostPointerEnabledLocked(result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        system_detail_ui.LeaveSystemInformation();
        return result;
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::UpdateSystemInformation(const host_ui::SystemInformationModel& model) {
    if (!system_detail_ui.SystemInformationVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    system_detail_ui.UpdateSystemInformationLocked(model);
    esp_lv_adapter_unlock();
}

void SquareSystemUiState::LeaveSystemInformation() {
    if (!system_detail_ui.SystemInformationVisible()) {
        return;
    }
    UnbindPageInput(system_detail_ui.SystemInformationActionContext());
    system_detail_ui.LeaveSystemInformation();
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowPowerManagement(
    const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    if (display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(system_menu_ui.ActionContext());
    system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(false);
    lv_obj_t* page_root = PrepareSystemPageRootLocked();
    auto result =
        page_root == nullptr
            ? std::expected<void, host_ui::SystemUiError>(std::unexpected(host_ui::SystemUiError::kRenderFailed))
            : system_detail_ui.ShowPowerManagementLocked(page_root, model, action_sink, action_context);
    SetHostPointerEnabledLocked(result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        system_detail_ui.LeavePowerManagement();
        return result;
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::UpdatePowerManagement(const host_ui::PowerManagementModel& model) {
    if (!system_detail_ui.PowerManagementVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    system_detail_ui.UpdatePowerManagementLocked(model);
    esp_lv_adapter_unlock();
}

void SquareSystemUiState::LeavePowerManagement() {
    if (!system_detail_ui.PowerManagementVisible()) {
        return;
    }
    UnbindPageInput(system_detail_ui.PowerManagementActionContext());
    system_detail_ui.LeavePowerManagement();
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowAppearance(const host_ui::AppearanceModel& model,
                                                                                host_ui::SystemUiActionSink action_sink,
                                                                                void* action_context) {
    if (display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(system_menu_ui.ActionContext());
    system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(false);
    lv_obj_t* page_root = PrepareSystemPageRootLocked();
    auto result =
        page_root == nullptr
            ? std::expected<void, host_ui::SystemUiError>(std::unexpected(host_ui::SystemUiError::kRenderFailed))
            : system_detail_ui.ShowAppearanceLocked(page_root, model, action_sink, action_context);
    SetHostPointerEnabledLocked(result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        system_detail_ui.LeaveAppearance();
        return result;
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::UpdateAppearance(const host_ui::AppearanceModel& model) {
    if (!system_detail_ui.AppearanceVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    system_detail_ui.UpdateAppearanceLocked(model);
    esp_lv_adapter_unlock();
}

void SquareSystemUiState::LeaveAppearance() {
    if (!system_detail_ui.AppearanceVisible()) {
        return;
    }
    UnbindPageInput(system_detail_ui.AppearanceActionContext());
    system_detail_ui.LeaveAppearance();
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowRemoteControl(
    const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    if (display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(system_menu_ui.ActionContext());
    system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(false);
    lv_obj_t* page_root = PrepareSystemPageRootLocked();
    auto result =
        page_root == nullptr
            ? std::expected<void, host_ui::SystemUiError>(std::unexpected(host_ui::SystemUiError::kRenderFailed))
            : system_detail_ui.ShowRemoteControlLocked(page_root, model, action_sink, action_context);
    SetHostPointerEnabledLocked(result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        system_detail_ui.LeaveRemoteControl();
        return result;
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::UpdateRemoteControl(const host_ui::RemoteControlModel& model) {
    if (!system_detail_ui.RemoteControlVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    system_detail_ui.UpdateRemoteControlLocked(model);
    esp_lv_adapter_unlock();
}

void SquareSystemUiState::LeaveRemoteControl() {
    if (!system_detail_ui.RemoteControlVisible()) {
        return;
    }
    UnbindPageInput(system_detail_ui.RemoteControlActionContext());
    system_detail_ui.LeaveRemoteControl();
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowAppManagement(
    const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    if (display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(system_menu_ui.ActionContext());
    system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(false);
    lv_obj_t* page_root = PrepareSystemPageRootLocked();
    auto result =
        page_root == nullptr
            ? std::expected<void, host_ui::SystemUiError>(std::unexpected(host_ui::SystemUiError::kRenderFailed))
            : system_detail_ui.ShowAppManagementLocked(page_root, model, action_sink, action_context);
    SetHostPointerEnabledLocked(result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        system_detail_ui.LeaveAppManagement();
        return result;
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::LeaveAppManagement() {
    if (!system_detail_ui.AppManagementVisible()) {
        return;
    }
    UnbindPageInput(system_detail_ui.AppManagementActionContext());
    system_detail_ui.LeaveAppManagement();
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowWifiSettings(
    const host_ui::WifiSettingsModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    if (display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    UnbindHostPointerTouchSink();
    input_router.ClearSystemActionSink(system_menu_ui.ActionContext());
    system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(false);
    lv_obj_t* page_root = PrepareSystemPageRootLocked();
    auto result =
        page_root == nullptr
            ? std::expected<void, host_ui::SystemUiError>(std::unexpected(host_ui::SystemUiError::kRenderFailed))
            : wifi_settings_ui.ShowLocked(
                  page_root, display, profile.system_page, model, action_sink, action_context,
                  [](void* context) { static_cast<StatusLayerUi*>(context)->RaisePerformanceOverlayLocked(); },
                  &status_layer_ui);
    SetHostPointerEnabledLocked(result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        wifi_settings_ui.Leave();
        return result;
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::UpdateWifiSettings(const host_ui::WifiSettingsModel& model) {
    wifi_settings_ui.Update(model, HostPointerBusy());
}

void SquareSystemUiState::LeaveWifiSettings() {
    if (!wifi_settings_ui.Visible()) {
        return;
    }
    UnbindPageInput(wifi_settings_ui.ActionContext());
    wifi_settings_ui.Leave();
}

void SquareSystemUiState::WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) {
    input_router.BindSystemActionSink(action_sink, action_context);
    if (display != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
        guest_gesture_hint_ui.ShowLocked(display);
        esp_lv_adapter_unlock();
    }
}

void SquareSystemUiState::StopWatchingGuestActions(void* action_context) {
    input_router.ClearSystemActionSink(action_context);
    if (display != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
        guest_gesture_hint_ui.HideLocked();
        esp_lv_adapter_unlock();
    }
}

std::expected<void, host_ui::SystemUiError> SquareSystemUiState::ShowStatusLayer(
    const host_ui::StatusLayerModel& model, uint64_t trigger_timestamp_us, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    hall_retained_for_status = false;
    if (display != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
        const auto& objects = hall_scene_ui.objects();
        hall_retained_for_status = root != nullptr && lv_obj_is_valid(root) && objects.carousel_content != nullptr &&
                                   lv_obj_is_valid(objects.carousel_content) && hall_action_context != nullptr;
        esp_lv_adapter_unlock();
    }
    auto result = PresentStatusLayer(display, status_layer_ui, transition_, model, trigger_timestamp_us, action_sink,
                                     action_context, profile.allow_software_status_animation);
    if (!result.has_value()) {
        hall_retained_for_status = false;
        return std::unexpected(result.error());
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(true);
        esp_lv_adapter_unlock();
    }
    BindPageInput(action_sink, action_context);
    return {};
}

void SquareSystemUiState::UpdateStatusLayer(const host_ui::StatusLayerModel& model) {
    if (display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    status_layer_ui.UpdateLocked(model);
    esp_lv_adapter_unlock();
}

void SquareSystemUiState::LeaveStatusLayer(uint64_t trigger_timestamp_us) {
    void* action_context = status_layer_ui.ActionContext();
    UnbindPageInput(action_context);
    (void)DismissStatusLayer(display, status_layer_ui, transition_, trigger_timestamp_us,
                             profile.allow_software_status_animation);
}

void SquareSystemUiState::UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) {
    if (display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    status_layer_ui.UpdatePerformanceOverlayLocked(enabled, cpu_percent, guest_graphics_.GuestPresentedFrameSequence());
    esp_lv_adapter_unlock();
}

void SquareSystemUiState::ApplyTheme(host_ui::SystemThemeMode mode) {
    hall_cover_cache.Pause();
    if (display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (theme::SetModeLocked(ThemeMode(mode))) {
        hall_cover_cache.SetBackgroundColorsLocked(theme::kHallBackground, theme::kHallCardBackground);
        if (theme_changed_locked_ != nullptr) {
            theme_changed_locked_(theme_changed_context_);
        }
        platform::lvgl::RequestDisplayRefresh(display);
    }
    esp_lv_adapter_unlock();
}

}  // namespace micropixel::host_ui::lvgl::square_common
