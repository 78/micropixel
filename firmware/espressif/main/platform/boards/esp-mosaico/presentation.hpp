#pragma once

#include "device/contracts/graphics.hpp"
#include "host/ui/lvgl/square_common/square_presentation.hpp"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/esp-mosaico/platform_state.hpp"

namespace micropixel::platform::esp_mosaico::detail {

class MosaicoPresentation final : public host_ui::lvgl::square_common::SquarePresentation,
                                  public host_ui::lvgl::square_common::DisplayTransition,
                                  public host_ui::lvgl::square_common::ScreenCapture,
                                  public host_ui::lvgl::square_common::BrightnessControl,
                                  public host_ui::lvgl::square_common::VolumeControl {
   public:
    explicit MosaicoPresentation(MosaicoBoardState& state) : state_(state) {}
    void BindAudioEngine(audio::AudioEngine& audio) { audio_ = &audio; }

    [[nodiscard]] host_ui::lvgl::square_common::DisplayTransition* Transition() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::ScreenCapture* Capture() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::BrightnessControl* Brightness() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::VolumeControl* Volume() override { return this; }

    [[nodiscard]] bool RetainNativeCover(const host_ui::HallCoverModel& source,
                                         host_ui::HallCoverModel& prepared) override;
    [[nodiscard]] bool EnterTransitionPending() const override { return false; }
    [[nodiscard]] bool BackgroundAvailable() const override;
    [[nodiscard]] bool PrepareBackgroundLocked(lv_obj_t* root) override;
    [[nodiscard]] bool UpdateBackgroundRegionLocked(lv_obj_t* object,
                                                    host_ui::lvgl::square_common::HallPresentationRect rect) override;
    [[nodiscard]] bool AnimateToHall(host_ui::lvgl::square_common::HallPresentationRect rect, uint32_t duration_ms,
                                     uint64_t trigger_timestamp_us) override;
    void CancelEnterTransition() override {}
    [[nodiscard]] bool AnimateToGuest(lv_obj_t* hall_root, lv_obj_t* guest_frame,
                                      const host_ui::lvgl::square_common::HallTransitionPresentation& presentation,
                                      uint32_t transition_duration_ms) override;
    [[nodiscard]] std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame(
        const host_ui::lvgl::square_common::HallTransitionPresentation& presentation, uint64_t trigger_timestamp_us,
        uint32_t transition_duration_ms) override;
    [[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() override;
    void ReleaseGuestSnapshot() override;
    [[nodiscard]] bool SynchronizeGuestReveal() const override { return false; }
    [[nodiscard]] bool PrepareHallBackgroundLocked();
    void ApplyBrightness(uint8_t percent) override;
    void ApplyVolume(uint8_t percent) override;

   private:
    MosaicoBoardState& state_;
    audio::AudioEngine* audio_{};
};

}  // namespace micropixel::platform::esp_mosaico::detail
