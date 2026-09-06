#pragma once

#include <cstdint>

#include "lvgl.h"
#include "platform/lvgl/guest_graphics_engine.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// Host-owned foreground affordance for the bottom-center suspend gesture.
// Callers hold the LVGL adapter lock for every public method.
//
// Two presentations share one visibility timer:
//   - LVGL object above the Guest frame (Scene Apps, composited boards);
//   - presenter overlay blended into scanned-out frames while the Guest owns
//     the panel through a Direct Surface. An LVGL object there would force
//     every Guest frame through the full-frame composited path (black when no
//     App Surface is available), so the pill is rasterized once into a small
//     ARGB image and handed to the presenter instead.
class GuestGestureHintUi final {
   public:
    GuestGestureHintUi() = default;
    // Best-effort: frees the overlay raster. LVGL objects and timers belong to
    // the display and are torn down with it.
    ~GuestGestureHintUi();
    GuestGestureHintUi(const GuestGestureHintUi&) = delete;
    GuestGestureHintUi& operator=(const GuestGestureHintUi&) = delete;

    void Bind(platform::lvgl::GuestGraphicsEngine* guest_graphics) { guest_graphics_ = guest_graphics; }

    // Starts (or restarts) the visible period in whichever presentation the
    // Guest currently needs.
    void ShowLocked(lv_display_t* display);
    // Re-evaluates the presentation while the hint is showing, e.g. after the
    // Guest created its Direct Surface. Cheap when nothing changed.
    void RefreshLocked();
    void HideLocked();
    void RaiseLocked();
    // True when the LVGL object is on screen (it then counts as Host UI above
    // the Guest frame). The overlay presentation is invisible to LVGL.
    [[nodiscard]] bool VisibleLocked() const;

   private:
    static void HideTimerCallback(lv_timer_t* timer);
    static void RefreshTimerCallback(lv_timer_t* timer);
    void HideObjectLocked();
    void StopTimers();
    void ClearOverlay();
    [[nodiscard]] bool DirectOverlayWanted() const;
    [[nodiscard]] bool EnsureOverlayImageLocked(lv_display_t* display);
    void RasterizePillLocked();
    [[nodiscard]] bool PublishOverlayLocked(lv_display_t* display);

    lv_obj_t* indicator_{};
    lv_timer_t* hide_timer_{};
    // Polls for the Guest gaining or losing the panel while the hint shows,
    // so the presentation follows without a hook into the Guest task. Also
    // keeps the LVGL worker awake until the visibility timer has expired.
    lv_timer_t* refresh_timer_{};
    lv_display_t* display_{};
    platform::lvgl::GuestGraphicsEngine* guest_graphics_{};
    // ARGB8888 pill for the presenter overlay (LVGL memory order B,G,R,A).
    uint8_t* overlay_pixels_{};
    uint32_t overlay_width_{};
    uint32_t overlay_height_{};
    int32_t overlay_x_{};
    int32_t overlay_y_{};
    uint32_t overlay_color_{};   // 0xRRGGBB the pixels were rasterized with
    bool overlay_rasterized_{};  // overlay_pixels_ hold a pill for the current size
    bool showing_{};
    bool overlay_published_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
