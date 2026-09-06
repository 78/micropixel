#include "host/ui/lvgl/square_common/guest_gesture_hint_ui.hpp"

#include <cmath>

#include "esp_heap_caps.h"
#include "host/ui/gesture_thresholds.hpp"
#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

constexpr uint32_t kVisibleDurationMs = 3000U;
constexpr uint32_t kRefreshPeriodMs = 50U;
constexpr uint8_t kIndicatorOpacity = 176U;

}  // namespace

bool GuestGestureHintUi::DirectOverlayWanted() const {
    return guest_graphics_ != nullptr && guest_graphics_->PresenterOverlayWanted();
}

void GuestGestureHintUi::ShowLocked(lv_display_t* display) {
    if (display == nullptr) {
        return;
    }
    display_ = display;
    showing_ = true;
    if (hide_timer_ == nullptr) {
        hide_timer_ = lv_timer_create(HideTimerCallback, kVisibleDurationMs, this);
    } else {
        lv_timer_set_period(hide_timer_, kVisibleDurationMs);
        lv_timer_reset(hide_timer_);
        lv_timer_resume(hide_timer_);
    }
    if (refresh_timer_ == nullptr && guest_graphics_ != nullptr && guest_graphics_->DirectScanoutAvailable()) {
        refresh_timer_ = lv_timer_create(RefreshTimerCallback, kRefreshPeriodMs, this);
    }
    RefreshLocked();
}

void GuestGestureHintUi::RefreshLocked() {
    if (!showing_ || display_ == nullptr) {
        return;
    }
    // The adapter's idle pause is based on activity, not pending LVGL timers.
    // Direct scanout supplies no LVGL invalidations, so keep the worker awake
    // until the hide timer runs (3 s, longer than the P4's 1 s idle timeout).
    // This does not request a display redraw or disturb panel ownership.
    (void)esp_lv_adapter_request_wake();
    if (DirectOverlayWanted()) {
        // Presenter overlay: make sure no LVGL object competes for the panel.
        HideObjectLocked();
        (void)PublishOverlayLocked(display_);
        return;
    }
    ClearOverlay();
    if (indicator_ == nullptr) {
        const int32_t display_width = lv_display_get_horizontal_resolution(display_);
        const int32_t indicator_width = gesture_thresholds::GestureHintWidth(display_width);
        const int32_t indicator_height = gesture_thresholds::GestureHintHeight(display_width);
        const int32_t bottom_margin = gesture_thresholds::GestureHintBottomMargin(display_width);
        indicator_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(indicator_, indicator_width, indicator_height);
        lv_obj_align(indicator_, LV_ALIGN_BOTTOM_MID, 0, -bottom_margin);
        lv_obj_set_style_pad_all(indicator_, 0, 0);
        lv_obj_set_style_border_width(indicator_, 0, 0);
        lv_obj_set_style_radius(indicator_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(indicator_, kIndicatorOpacity, 0);
        lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_bg_color(indicator_, lv_color_hex(theme::kOverlayText), 0);
    if (!VisibleLocked()) {
        lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
        RaiseLocked();
        platform::lvgl::RequestDisplayRefresh(display_);
    }
}

GuestGestureHintUi::~GuestGestureHintUi() { heap_caps_free(overlay_pixels_); }

void GuestGestureHintUi::StopTimers() {
    if (hide_timer_ != nullptr) {
        lv_timer_delete(hide_timer_);
        hide_timer_ = nullptr;
    }
    if (refresh_timer_ != nullptr) {
        lv_timer_delete(refresh_timer_);
        refresh_timer_ = nullptr;
    }
}

void GuestGestureHintUi::HideLocked() {
    StopTimers();
    showing_ = false;
    HideObjectLocked();
    ClearOverlay();
}

void GuestGestureHintUi::RaiseLocked() {
    if (VisibleLocked()) {
        lv_obj_move_foreground(indicator_);
    }
}

bool GuestGestureHintUi::VisibleLocked() const {
    return indicator_ != nullptr && !lv_obj_has_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
}

void GuestGestureHintUi::HideTimerCallback(lv_timer_t* timer) {
    auto* ui = static_cast<GuestGestureHintUi*>(lv_timer_get_user_data(timer));
    if (ui == nullptr) {
        lv_timer_delete(timer);
        return;
    }
    // StopTimers() deletes this timer (hide_timer_) together with the refresh timer.
    ui->StopTimers();
    ui->showing_ = false;
    ui->HideObjectLocked();
    ui->ClearOverlay();
}

void GuestGestureHintUi::RefreshTimerCallback(lv_timer_t* timer) {
    auto* ui = static_cast<GuestGestureHintUi*>(lv_timer_get_user_data(timer));
    if (ui == nullptr) {
        lv_timer_delete(timer);
        return;
    }
    ui->RefreshLocked();
}

void GuestGestureHintUi::HideObjectLocked() {
    if (!VisibleLocked()) {
        return;
    }
    lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(indicator_));
}

void GuestGestureHintUi::ClearOverlay() {
    if (!overlay_published_) {
        return;
    }
    overlay_published_ = false;
    if (guest_graphics_ != nullptr) {
        guest_graphics_->ClearDirectSurfaceOverlay(platform::lvgl::ScanoutOverlayLayer::kGestureHint);
    }
}

bool GuestGestureHintUi::EnsureOverlayImageLocked(lv_display_t* display) {
    const int32_t display_width = lv_display_get_horizontal_resolution(display);
    const int32_t display_height = lv_display_get_vertical_resolution(display);
    const int32_t width = gesture_thresholds::GestureHintWidth(display_width);
    const int32_t height = gesture_thresholds::GestureHintHeight(display_width);
    const int32_t bottom_margin = gesture_thresholds::GestureHintBottomMargin(display_width);
    if (width <= 0 || height <= 0 || width > display_width || height > display_height) {
        return false;
    }
    if (overlay_pixels_ == nullptr) {
        overlay_pixels_ = static_cast<uint8_t*>(
            heap_caps_malloc(static_cast<size_t>(width) * static_cast<size_t>(height) * 4U, MALLOC_CAP_8BIT));
        if (overlay_pixels_ == nullptr) {
            return false;
        }
        overlay_width_ = static_cast<uint32_t>(width);
        overlay_height_ = static_cast<uint32_t>(height);
        overlay_rasterized_ = false;
    }
    overlay_x_ = (display_width - width) / 2;
    overlay_y_ = display_height - bottom_margin - height;
    if (!overlay_rasterized_ || overlay_color_ != theme::kOverlayText) {
        RasterizePillLocked();
    }
    return true;
}

void GuestGestureHintUi::RasterizePillLocked() {
    // Same look as the LVGL object: a capsule (LV_RADIUS_CIRCLE) filled with
    // the overlay text color at kIndicatorOpacity, anti-aliased by signed
    // distance to the capsule outline.
    overlay_rasterized_ = true;
    overlay_color_ = theme::kOverlayText;
    const uint8_t red = static_cast<uint8_t>((overlay_color_ >> 16U) & 0xFFU);
    const uint8_t green = static_cast<uint8_t>((overlay_color_ >> 8U) & 0xFFU);
    const uint8_t blue = static_cast<uint8_t>(overlay_color_ & 0xFFU);
    const float radius = static_cast<float>(overlay_height_) * 0.5F;
    const float segment_start = radius;
    const float segment_end = static_cast<float>(overlay_width_) - radius;
    uint8_t* out = overlay_pixels_;
    for (uint32_t y = 0U; y < overlay_height_; ++y) {
        const float py = static_cast<float>(y) + 0.5F - radius;
        for (uint32_t x = 0U; x < overlay_width_; ++x, out += 4U) {
            const float px = static_cast<float>(x) + 0.5F;
            const float nearest_x = px < segment_start ? segment_start : (px > segment_end ? segment_end : px);
            const float dx = px - nearest_x;
            const float distance = std::sqrt(dx * dx + py * py) - radius;
            float coverage = 0.5F - distance;
            coverage = coverage < 0.0F ? 0.0F : (coverage > 1.0F ? 1.0F : coverage);
            out[0] = blue;
            out[1] = green;
            out[2] = red;
            out[3] = static_cast<uint8_t>(coverage * static_cast<float>(kIndicatorOpacity) + 0.5F);
        }
    }
}

bool GuestGestureHintUi::PublishOverlayLocked(lv_display_t* display) {
    if (guest_graphics_ == nullptr || !EnsureOverlayImageLocked(display)) {
        return false;
    }
    if (overlay_published_) {
        return true;
    }
    const platform::lvgl::ScanoutOverlayImage image{
        .pixels = overlay_pixels_,
        .width = overlay_width_,
        .height = overlay_height_,
        .stride = overlay_width_ * 4U,
        .x = overlay_x_,
        .y = overlay_y_,
    };
    overlay_published_ =
        guest_graphics_->SetDirectSurfaceOverlay(platform::lvgl::ScanoutOverlayLayer::kGestureHint, image);
    return overlay_published_;
}

}  // namespace micropixel::host_ui::lvgl::square_common
