// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "platform/lvgl/display/display_pipeline.hpp"

namespace micropixel::platform::m5stack_tab5 {

// The two DPI framebuffers the ILI9881C panel scans out of, lent to the
// graphics engine so Guest frames can bypass LVGL (Graphics 1.5 Direct Surface
// and the App Surface scanout). Same mechanism as the claw4 pipeline: the free
// buffer comes from the LVGL adapter's dummy draw and the flip goes through
// esp_lv_adapter_dummy_draw_flush_buf, so LVGL and the presenter never own the
// panel at the same time.
//
// The engine keeps this object for the lifetime of the process, so it lives in
// the board state; Bind once after the display is registered and again after a
// panel re-creation.
class Tab5DpiFramebuffers final : public lvgl::DirectFramebufferAccess {
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

// The ILI9881C consumes 16 bpp RGB565 over its two DSI lanes, so the presenter
// copies or PPA-converts Guest frames into the free framebuffer and flips it
// instead of letting LVGL composite every frame.
[[nodiscard]] lvgl::DirectScanoutProfile Tab5DirectScanoutProfile();

}  // namespace micropixel::platform::m5stack_tab5
