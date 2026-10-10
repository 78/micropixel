#pragma once

#include "host/ui/lvgl/square_common/square_presentation.hpp"
#include "platform/boards/ksdiy-p4c5/platform_state.hpp"

namespace micropixel::platform::ksdiy_p4c5::detail {

// Basic presentation roles. Hall transitions are not provided: the shared
// transition compositor only scales square frames.
class KsdiyP4c5Presentation final : public host_ui::lvgl::square_common::SquarePresentation,
                                    public host_ui::lvgl::square_common::ScreenCapture,
                                    public host_ui::lvgl::square_common::BrightnessControl,
                                    public host_ui::lvgl::square_common::VolumeControl {
   public:
    explicit KsdiyP4c5Presentation(KsdiyP4c5BoardState& state) : state_(state) {}

    // A landscape frame only exists rotated inside the DPI framebuffers, so
    // landscape builds expose no screenshot.
    [[nodiscard]] host_ui::lvgl::square_common::ScreenCapture* Capture() override {
        if constexpr (board::kLandscape) {
            return nullptr;
        } else {
            return this;
        }
    }
    [[nodiscard]] host_ui::lvgl::square_common::BrightnessControl* Brightness() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::VolumeControl* Volume() override { return this; }

    [[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() override;
    void ApplyBrightness(uint8_t percent) override;
    void ApplyVolume(uint8_t percent) override;

   private:
    KsdiyP4c5BoardState& state_;
};

}  // namespace micropixel::platform::ksdiy_p4c5::detail
