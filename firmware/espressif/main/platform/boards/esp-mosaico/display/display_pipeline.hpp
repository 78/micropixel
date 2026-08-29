#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_async_color_convert.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "platform/lvgl/display/display_pipeline.hpp"

namespace micropixel::platform::esp_mosaico {

// Owns the CO5300 transport, panel, LVGL presentation hook and the canonical
// RGB565 shadow of panel GRAM. Other Mosaico components consume only the
// read-only presentation views exposed here; they never submit panel traffic.
class MosaicoDisplayPipeline final : public lvgl::DisplayPipeline {
   public:
    MosaicoDisplayPipeline(uint32_t width, uint32_t height) : geometry_{width, height, 2U} {}

    [[nodiscard]] esp_err_t InitializePanel();
    [[nodiscard]] esp_err_t BindLvgl(lv_display_t* display);

    [[nodiscard]] lvgl::DisplayGeometry Geometry() const override { return geometry_; }
    [[nodiscard]] lvgl::DisplayCapabilities Capabilities() const override;
    [[nodiscard]] lvgl::DirectFramebufferAccess* DirectFramebuffers() override { return nullptr; }
    [[nodiscard]] esp_err_t Suspend() override;
    [[nodiscard]] esp_err_t Resume() override;
    [[nodiscard]] esp_err_t SetBrightness(uint32_t per_ten_thousand) override;

    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return panel_; }
    [[nodiscard]] esp_lcd_panel_io_handle_t PanelIo() const { return panel_io_; }
    [[nodiscard]] uint8_t* DisplayedShadow() const { return displayed_shadow_; }
    [[nodiscard]] const bool* DisplayedShadowReady() const { return &displayed_shadow_valid_; }
    [[nodiscard]] async_color_convert_handle_t ShadowCopyDma2d() const { return shadow_copy_dma2d_; }

   private:
    static void Flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);
    static void RoundArea(lv_area_t* area, void* context);
    [[nodiscard]] bool CaptureDisplayedShadow(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);

    static inline MosaicoDisplayPipeline* flush_owner_{};
    lvgl::DisplayGeometry geometry_{};
    esp_lcd_panel_handle_t panel_{};
    esp_lcd_panel_io_handle_t panel_io_{};
    lv_display_t* display_{};
    async_color_convert_handle_t shadow_copy_dma2d_{};
    lv_display_flush_cb_t adapter_flush_cb_{};
    uint8_t* displayed_shadow_{};
    std::array<bool, 480U> displayed_shadow_rows_{};
    uint32_t panel_submit_count_{};
    uint32_t shadow_copy_count_{};
    uint64_t panel_submit_pixels_{};
    uint64_t shadow_copy_pixels_{};
    bool displayed_shadow_valid_{};
};

}  // namespace micropixel::platform::esp_mosaico
