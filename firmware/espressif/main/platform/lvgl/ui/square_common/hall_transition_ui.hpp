#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace micropixel::platform::lvgl::square_common {

struct HallTransitionRect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

enum class HallTransitionDirection : uint8_t {
    kToHall,
    kToGuest,
};

struct HallTransitionTimeline final {
    uint32_t duration_ms{100U};
};

struct HallTransitionFrame final {
    const uint8_t* data{};
    uint32_t size{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
};

// Shared LVGL presenter for display pipelines without directly exchangeable
// full-screen framebuffers (for example a QSPI controller with internal GRAM).
// It uses two pre-scaled frames, avoiding a CPU transform of a full-screen
// image on every QSPI flush. The transition state and geometry are board
// independent; the LVGL display driver remains responsible for the
// panel-specific RGB conversion and flush.
class HallTransitionUi final {
   public:
    HallTransitionUi() = default;
    HallTransitionUi(const HallTransitionUi&) = delete;
    HallTransitionUi& operator=(const HallTransitionUi&) = delete;

    void Initialize(lv_display_t* display, uint32_t width, uint32_t height);
    [[nodiscard]] bool PrepareLocked(HallTransitionFrame intermediate, HallTransitionFrame cover,
                                     const HallTransitionRect& card, HallTransitionDirection direction);
    [[nodiscard]] bool Animate(HallTransitionDirection direction,
                               HallTransitionTimeline timeline = HallTransitionTimeline{});
    void FinishLocked();
    [[nodiscard]] bool active() const { return overlay_ != nullptr; }

   private:
    void ShowFrameLocked(size_t index);

    lv_display_t* display_{};
    lv_obj_t* overlay_{};
    std::array<HallTransitionFrame, 2U> frames_{};
    std::array<lv_image_dsc_t, 2U> descriptors_{};
    HallTransitionRect card_{};
    uint32_t width_{};
    uint32_t height_{};
};

}  // namespace micropixel::platform::lvgl::square_common
