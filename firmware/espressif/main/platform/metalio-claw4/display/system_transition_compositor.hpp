#pragma once

#include <array>
#include <cstdint>

#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

namespace micropixel::platform::metalio_claw4 {

struct SystemTransitionRect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

enum class SystemTransitionDirection : uint8_t {
    kToHall,
    kToGuest,
};

// Board-private full-screen transition compositor. Resizes use the ESP32-P4
// PPA SRM engine, while exact Hall-background copies prefer DMA2D. LVGL only
// renders the static Hall background once before dummy-draw mode takes over.
class SystemTransitionCompositor final {
   public:
    SystemTransitionCompositor() = default;
    SystemTransitionCompositor(const SystemTransitionCompositor&) = delete;
    SystemTransitionCompositor& operator=(const SystemTransitionCompositor&) = delete;
    ~SystemTransitionCompositor();

    [[nodiscard]] esp_err_t Initialize(lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t width,
                                       uint32_t height);
    [[nodiscard]] bool HasBackground() const { return background_pixels_ != nullptr; }
    [[nodiscard]] bool PrepareBackgroundLocked(lv_obj_t* root);
    // Refresh only the working animation background. The preserved Hall
    // baseline remains unchanged so a running-card view cannot become the
    // starting point for a later Guest-to-Hall transition.
    [[nodiscard]] bool RefreshBackgroundLocked(lv_obj_t* root);
    [[nodiscard]] bool ResetBackgroundToBaseline();
    [[nodiscard]] bool UpdateBackgroundRegionLocked(lv_obj_t* root, const SystemTransitionRect& region);
    [[nodiscard]] bool UpdateBackgroundPixels(const uint8_t* pixels, const SystemTransitionRect& region);
    [[nodiscard]] bool CaptureDisplayedToHalf(uint8_t* half, uint32_t half_allocation_bytes,
                                              const SystemTransitionRect& card, uint64_t trigger_timestamp_us,
                                              uint32_t& elapsed_us);
    [[nodiscard]] bool ScaleFullscreenToHalf(const uint8_t* fullscreen, uint8_t* half, uint32_t half_allocation_bytes);
    [[nodiscard]] bool ScaleHalfToCard(const uint8_t* half, uint8_t* card, uint32_t card_allocation_bytes);
    [[nodiscard]] bool Animate(const uint8_t* half_guest, const SystemTransitionRect& card,
                               SystemTransitionDirection direction, uint32_t duration_ms,
                               uint64_t trigger_timestamp_us = 0U);
    [[nodiscard]] bool BeginStatusLayerTransition(bool entering, uint32_t scrim_rgb, uint8_t scrim_opacity,
                                                  uint64_t trigger_timestamp_us);
    [[nodiscard]] bool AnimateStatusLayerLocked(lv_obj_t* dialog, int32_t visible_y, int32_t hidden_y, bool entering,
                                                uint32_t duration_ms, uint64_t trigger_timestamp_us);
    [[nodiscard]] bool FinishStatusLayerTransition(bool keep_buffers);
    void CancelStatusLayerTransition();
    void CancelPreparedToHall();
    void ClearBackground();
    void Release();

   private:
    [[nodiscard]] bool CaptureBackgroundLocked(lv_obj_t* root, bool preserve_as_baseline);
    [[nodiscard]] bool BlitRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                  uint32_t source_x, uint32_t source_y, uint32_t source_block_width,
                                  uint32_t source_block_height, uint8_t* destination, uint32_t destination_width,
                                  uint32_t destination_height, uint32_t destination_allocation_bytes,
                                  uint32_t destination_x, uint32_t destination_y, float scale);
    [[nodiscard]] bool ScaleRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                   uint8_t* destination, uint32_t destination_width, uint32_t destination_height,
                                   uint32_t destination_allocation_bytes, uint32_t destination_x,
                                   uint32_t destination_y, float scale);
    [[nodiscard]] bool CopyRgb888(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                                  uint32_t source_x, uint32_t source_y, uint8_t* destination,
                                  uint32_t destination_width, uint32_t destination_height, uint32_t destination_x,
                                  uint32_t destination_y, uint32_t copy_width, uint32_t copy_height);
    [[nodiscard]] bool ComposeBackground(uint8_t* frame_buffer);
    [[nodiscard]] bool ComposeBackgroundRegion(uint8_t* frame_buffer, const SystemTransitionRect& region);
    [[nodiscard]] bool RestoreGuestDifference(uint8_t* frame_buffer, const SystemTransitionRect& old_region,
                                              const SystemTransitionRect& new_region);
    [[nodiscard]] SystemTransitionRect GuestRect(const SystemTransitionRect& card, uint32_t scale_units) const;
    [[nodiscard]] bool ComposeGuest(uint8_t* frame_buffer, const uint8_t* half_guest, const SystemTransitionRect& card,
                                    uint32_t scale_units, SystemTransitionRect& composed_region);
    [[nodiscard]] bool SubmitFrame(uint8_t* frame_buffer);
    [[nodiscard]] uint8_t* DisplayedFrameBuffer() const;
    [[nodiscard]] bool CaptureStatusDialogLocked(lv_obj_t* dialog);
    [[nodiscard]] bool ComposeStatusScrimRegion(uint8_t* frame_buffer, const SystemTransitionRect& region);
    [[nodiscard]] bool CopyStatusScrimRegion(uint8_t* frame_buffer, const SystemTransitionRect& region);
    [[nodiscard]] bool ComposeStatusDialog(uint8_t* frame_buffer, const SystemTransitionRect& region);
    [[nodiscard]] SystemTransitionRect VisibleStatusDialogRect(int32_t x, int32_t y) const;
    void ClearStatusLayerBuffers();

    static constexpr uint32_t kBytesPerPixel = 3U;
    static constexpr uint32_t kPpaScaleDenominator = 16U;
    static constexpr uint32_t kCardWidth = 202U;
    static constexpr uint32_t kHalfWidth = 360U;

    lv_display_t* display_{};
    esp_lcd_panel_handle_t panel_{};
    ppa_client_handle_t srm_client_{};
    ppa_client_handle_t blend_client_{};
    async_color_convert_handle_t dma2d_client_{};
    uint8_t* background_pixels_{};
    uint8_t* baseline_background_pixels_{};
    uint32_t width_{};
    uint32_t height_{};
    uint32_t frame_bytes_{};
    struct StatusFrameState final {
        uint8_t* buffer{};
        SystemTransitionRect dialog_region{};
        bool scrim_ready{};
        bool has_dialog{};
    };
    std::array<StatusFrameState, 3U> status_frame_states_{};
    uint8_t* status_background_pixels_{};
    uint8_t* status_scrim_background_pixels_{};
    uint8_t* status_scrim_alpha_pixels_{};
    uint8_t* status_dialog_pixels_{};
    uint32_t status_dialog_width_{};
    uint32_t status_dialog_height_{};
    uint32_t status_dialog_allocation_bytes_{};
    uint32_t status_scrim_rgb_{};
    int32_t status_dialog_x_{};
    int32_t status_dialog_ext_draw_size_{};
    bool status_transition_dummy_active_{};
    bool status_layer_buffers_ready_{};
    bool prepared_to_hall_{};
};

}  // namespace micropixel::platform::metalio_claw4
