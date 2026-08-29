#pragma once

#include <cstdint>
#include <expected>

#include "host/ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::host_ui::lvgl::square_common {

struct HallPresentationRect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

struct HallTransitionPresentation final {
    HallPresentationRect card{};
    uint32_t cover_size{};
    uint32_t cover_stride{};
    uint32_t cover_bytes{};
    uint32_t intermediate_size{};
    uint32_t cover_corner_radius{};
    uint32_t cover_top_background_rgb{};
    uint32_t cover_bottom_background_rgb{};
    bool card_valid{};
};

void MaskHallTransitionCoverRgb888(const HallTransitionPresentation& presentation, uint8_t* destination);

// Optional accelerated Guest <-> Hall presentation boundary. A Board either
// supplies a complete implementation or omits it; Host never
// infers support from a partially populated callback table.
class DisplayTransition {
   public:
    virtual ~DisplayTransition() = default;
    DisplayTransition(const DisplayTransition&) = delete;
    DisplayTransition& operator=(const DisplayTransition&) = delete;

    [[nodiscard]] virtual bool RetainNativeCover(const host_ui::HallCoverModel& source,
                                                 host_ui::HallCoverModel& prepared) = 0;
    [[nodiscard]] virtual bool EnterTransitionPending() const = 0;
    [[nodiscard]] virtual bool BackgroundAvailable() const = 0;
    [[nodiscard]] virtual bool PrepareBackgroundLocked(lv_obj_t* root) = 0;
    [[nodiscard]] virtual bool UpdateBackgroundRegionLocked(lv_obj_t* object, HallPresentationRect rect) = 0;
    [[nodiscard]] virtual bool AnimateToHall(HallPresentationRect rect, uint32_t duration_ms,
                                             uint64_t trigger_timestamp_us) = 0;
    virtual void CancelEnterTransition() = 0;
    [[nodiscard]] virtual bool AnimateToGuest(lv_obj_t* hall_root, lv_obj_t* guest_frame,
                                              const HallTransitionPresentation& presentation, uint32_t duration_ms) = 0;
    [[nodiscard]] virtual std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame(
        const HallTransitionPresentation& presentation, uint64_t trigger_timestamp_us, uint32_t duration_ms) = 0;
    virtual void ReleaseGuestSnapshot() = 0;
    [[nodiscard]] virtual bool SynchronizeGuestReveal() const = 0;

   protected:
    DisplayTransition() = default;
};

class ScreenCapture {
   public:
    virtual ~ScreenCapture() = default;
    [[nodiscard]] virtual std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() = 0;
};

class BrightnessControl {
   public:
    virtual ~BrightnessControl() = default;
    virtual void ApplyBrightness(uint8_t percent) = 0;
};

class VolumeControl {
   public:
    virtual ~VolumeControl() = default;
    virtual void ApplyVolume(uint8_t percent) = 0;
};

class ShutdownPresentation {
   public:
    virtual ~ShutdownPresentation() = default;
    virtual void PrepareShutdownLocked() = 0;
};

// Common Host UI consumes typed optional presentation roles. Basic LVGL UI
// remains usable when every optional method returns nullptr.
class SquarePresentation {
   public:
    virtual ~SquarePresentation() = default;
    [[nodiscard]] virtual DisplayTransition* Transition() { return nullptr; }
    [[nodiscard]] virtual ScreenCapture* Capture() { return nullptr; }
    [[nodiscard]] virtual BrightnessControl* Brightness() { return nullptr; }
    [[nodiscard]] virtual VolumeControl* Volume() { return nullptr; }
    [[nodiscard]] virtual ShutdownPresentation* Shutdown() { return nullptr; }
    virtual void BeforeHallRebuildLocked() {}
};

}  // namespace micropixel::host_ui::lvgl::square_common
