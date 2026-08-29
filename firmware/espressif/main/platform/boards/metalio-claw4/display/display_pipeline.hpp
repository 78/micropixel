#pragma once

#include <array>
#include <cstdint>

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "platform/boards/metalio-claw4/board_io.hpp"
#include "platform/lvgl/display/display_pipeline.hpp"

namespace micropixel::platform::metalio_claw4 {

class MetalioClaw4DisplayPipeline final : public lvgl::DisplayPipeline {
   public:
    MetalioClaw4DisplayPipeline(BoardIo& board_io, uint32_t width, uint32_t height)
        : board_io_(board_io), geometry_{width, height, 3U} {}

    void BindLvgl(lv_display_t* display);
    void RebindPanel();

    [[nodiscard]] lvgl::DisplayGeometry Geometry() const override { return geometry_; }
    [[nodiscard]] lvgl::DisplayCapabilities Capabilities() const override;
    [[nodiscard]] lvgl::DirectFramebufferAccess* DirectFramebuffers() override { return &framebuffers_; }
    [[nodiscard]] esp_err_t Suspend() override;
    [[nodiscard]] esp_err_t Resume() override;
    [[nodiscard]] esp_err_t SetBrightness(uint32_t per_ten_thousand) override;

    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return board_io_.Panel(); }
    [[nodiscard]] esp_lcd_panel_io_handle_t PanelIo() const { return board_io_.PanelIo(); }

   private:
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
    lv_display_t* display_{};
};

}  // namespace micropixel::platform::metalio_claw4
