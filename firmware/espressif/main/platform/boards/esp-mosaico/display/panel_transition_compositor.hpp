#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_err.h"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/lvgl/display/display_pipeline.hpp"
#include "platform/lvgl/display/ppa_srm_blitter.hpp"

namespace micropixel::platform::esp_mosaico {

struct PanelTransitionRect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

enum class PanelTransitionDirection : uint8_t {
    kToHall,
    kToGuest,
};

// CO5300 keeps the displayed image in panel GRAM, so it cannot exchange a
// framebuffer like the P4 RGB pipeline. Retained sources stay in canonical
// RGB565. PPA first composes each changed region in canonical byte order, then
// performs a separate 1:1 hardware byte-pack pass for the CO5300 wire order.
// Keeping byte packing out of the scaling pass is required: PPA byte_swap acts
// on input pixels, so enabling it while scaling interpolates misread colours.
class PanelTransitionCompositor final : public host_ui::lvgl::square_common::StatusLayerTransition {
   public:
    PanelTransitionCompositor() = default;
    PanelTransitionCompositor(const PanelTransitionCompositor&) = delete;
    PanelTransitionCompositor& operator=(const PanelTransitionCompositor&) = delete;
    ~PanelTransitionCompositor();

    [[nodiscard]] esp_err_t Initialize(lv_display_t* display, uint32_t width, uint32_t height,
                                       lvgl::SystemTransitionProfile profile, async_color_convert_handle_t dma2d_client,
                                       uint8_t* displayed_source, const bool* displayed_source_ready);
    [[nodiscard]] bool HasBackground() const { return background_ != nullptr; }
    [[nodiscard]] bool CaptureDisplayedBackground(const uint8_t* fullscreen);
    [[nodiscard]] bool UpdateBackgroundRgb888(const uint8_t* pixels, const PanelTransitionRect& region);
    [[nodiscard]] bool CaptureDisplayedToIntermediate(const uint8_t* fullscreen, uint8_t* intermediate,
                                                      uint32_t intermediate_allocation_bytes,
                                                      const PanelTransitionRect& card, uint64_t trigger_timestamp_us,
                                                      uint32_t& elapsed_us);
    [[nodiscard]] bool ScaleIntermediateToCoverRgb888(const uint8_t* intermediate, uint8_t* cover,
                                                      uint32_t cover_allocation_bytes);
    [[nodiscard]] bool Animate(const uint8_t* intermediate, const PanelTransitionRect& card,
                               PanelTransitionDirection direction, uint32_t duration_ms = 140U,
                               uint64_t trigger_timestamp_us = 0U);
    [[nodiscard]] bool BeginStatusLayerTransition(bool entering, uint32_t scrim_rgb, uint8_t scrim_opacity,
                                                  uint64_t trigger_timestamp_us) override;
    [[nodiscard]] bool AnimateStatusLayerLocked(lv_obj_t* dialog, int32_t visible_y, int32_t hidden_y, bool entering,
                                                uint32_t duration_ms, uint64_t trigger_timestamp_us) override;
    [[nodiscard]] bool FinishStatusLayerTransition(bool keep_buffers) override;
    void CancelStatusLayerTransition() override;
    void CancelPreparedToHall();
    void ReleaseBackground();

   private:
    [[nodiscard]] PanelTransitionRect GuestRect(const PanelTransitionRect& card, uint32_t scale_units) const;
    [[nodiscard]] PanelTransitionRect AlignedUnion(const PanelTransitionRect& left,
                                                   const PanelTransitionRect& right) const;
    [[nodiscard]] bool ComposeStage(const uint8_t* intermediate, const PanelTransitionRect& target,
                                    const PanelTransitionRect& update, uint32_t scale_units, bool overlay_guest);
    [[nodiscard]] bool BlitStage(const PanelTransitionRect& update);
    [[nodiscard]] bool PpaCopy(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                               const PanelTransitionRect& source_region, ppa_srm_color_mode_t source_mode,
                               uint8_t* destination, uint32_t destination_width, uint32_t destination_height,
                               uint32_t destination_allocation_bytes, uint32_t destination_x, uint32_t destination_y,
                               ppa_srm_color_mode_t destination_mode, float scale);
    [[nodiscard]] bool Finish(bool success);
    [[nodiscard]] bool CaptureStatusDialogLocked(lv_obj_t* dialog);
    [[nodiscard]] PanelTransitionRect VisibleStatusDialogRect(int32_t x, int32_t y) const;
    [[nodiscard]] bool ComposeStatusScrim();
    [[nodiscard]] bool ComposeStatusStage(const PanelTransitionRect& update, const PanelTransitionRect& dialog_region,
                                          int32_t dialog_y, bool show_dialog, bool restore_background);
    [[nodiscard]] bool SyncDisplayedSource(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                           const PanelTransitionRect& destination);
    void ClearStatusLayerBuffers();
    void Release();

    lv_display_t* display_{};
    lvgl::PpaSrmBlitter srm_blitter_{};
    async_color_convert_handle_t dma2d_client_{};
    ppa_client_handle_t blend_client_{};
    uint8_t* displayed_source_{};
    const bool* displayed_source_ready_{};
    uint8_t* background_{};
    uint8_t* native_stage_{};
    uint8_t* wire_stage_{};
    uint32_t width_{};
    uint32_t height_{};
    uint32_t frame_allocation_bytes_{};
    lvgl::SystemTransitionProfile profile_{};
    uint8_t* status_background_{};
    uint8_t* status_scrim_{};
    uint8_t* status_scrim_alpha_{};
    uint8_t* status_dialog_pixels_{};
    uint32_t status_dialog_width_{};
    uint32_t status_dialog_height_{};
    uint32_t status_dialog_allocation_bytes_{};
    int32_t status_dialog_x_{};
    int32_t status_dialog_ext_draw_size_{};
    uint32_t status_scrim_rgb_{};
    bool status_transition_dummy_active_{};
    bool status_layer_buffers_ready_{};
    bool prepared_to_hall_{};
};

}  // namespace micropixel::platform::esp_mosaico
