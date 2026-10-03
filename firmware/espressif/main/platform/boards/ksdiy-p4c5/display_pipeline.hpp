#pragma once

#include <array>
#include <cstdint>

#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "platform/boards/ksdiy-p4c5/board_config.hpp"
#include "platform/boards/ksdiy-p4c5/board_io.hpp"
#include "platform/lvgl/display/display_pipeline.hpp"

namespace micropixel::platform::ksdiy_p4c5 {

// Portrait scans LVGL's two direct-mode RGB888 framebuffers out unchanged, so
// Guest frames can be PPA-converted straight into a free DPI framebuffer.
// Landscape renders through the LVGL adapter's PPA rotation; its framebuffers
// are rotated relative to LVGL, so it exposes none and keeps Guest frames on
// the composited App Surface path.
class DisplayPipeline final : public lvgl::DisplayPipeline {
   public:
    explicit DisplayPipeline(BoardIo& board_io)
        : board_io_(board_io),
          geometry_{static_cast<uint32_t>(board::kDisplayWidth), static_cast<uint32_t>(board::kDisplayHeight), 3U} {}

    void BindLvgl(lv_display_t* display);

    [[nodiscard]] lvgl::DisplayGeometry Geometry() const override { return geometry_; }
    [[nodiscard]] lvgl::DisplayCapabilities Capabilities() const override;
    [[nodiscard]] lvgl::DirectFramebufferAccess* DirectFramebuffers() override {
        return board::kLandscape ? nullptr : &framebuffers_;
    }
    [[nodiscard]] lvgl::DirectScanoutProfile DirectScanout() const override;
    [[nodiscard]] esp_err_t Suspend() override { return board_io_.SetDisplayEnabled(false); }
    [[nodiscard]] esp_err_t Resume() override { return board_io_.SetDisplayEnabled(true); }
    [[nodiscard]] esp_err_t SetBrightness(uint32_t per_ten_thousand) override {
        return board_io_.SetBacklightOutputPerTenThousand(per_ten_thousand);
    }

    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return board_io_.Panel(); }

   private:
    static constexpr uint16_t kDirectScanoutMaxFps = 60U;

    class DpiFramebuffers final : public lvgl::DirectFramebufferAccess {
       public:
        void Bind(lv_display_t* display, esp_lcd_panel_handle_t panel);
        [[nodiscard]] bool Ready() const override;
        [[nodiscard]] uint32_t Count() const override { return buffers_.size(); }
        [[nodiscard]] uint8_t* AcquireFree() override;
        [[nodiscard]] uint8_t* Displayed() override;
        [[nodiscard]] bool Contains(const uint8_t* buffer) const override;
        [[nodiscard]] esp_err_t Submit(uint8_t* buffer) override;

       private:
        [[nodiscard]] bool Resolve();

        lv_display_t* display_{};
        esp_lcd_panel_handle_t panel_{};
        std::array<uint8_t*, 2U> buffers_{};
    };

    BoardIo& board_io_;
    lvgl::DisplayGeometry geometry_{};
    DpiFramebuffers framebuffers_{};
};

}  // namespace micropixel::platform::ksdiy_p4c5
