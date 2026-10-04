// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/display/dpi_framebuffers.hpp"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lv_adapter.h"

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr uint32_t kFramebufferCount = 2U;
// Panel transfer bound for one full frame: the vendor video timing drives the
// native 720x1280 scanout from a 60 MHz DPI pixel clock.
constexpr uint16_t kDirectScanoutMaxFps = 60U;

}  // namespace

void Tab5DpiFramebuffers::Bind(lv_display_t* display, esp_lcd_panel_handle_t panel) {
    display_ = display;
    panel_ = panel;
    buffers_ = {};
    (void)Resolve();
}

bool Tab5DpiFramebuffers::Resolve() {
    if (display_ == nullptr || panel_ == nullptr) {
        buffers_ = {};
        return false;
    }
    void* first = nullptr;
    void* second = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel_, kFramebufferCount, &first, &second) != ESP_OK) {
        buffers_ = {};
        return false;
    }
    buffers_[0] = static_cast<uint8_t*>(first);
    buffers_[1] = static_cast<uint8_t*>(second);
    return buffers_[0] != nullptr && buffers_[1] != nullptr;
}

bool Tab5DpiFramebuffers::Ready() const {
    return display_ != nullptr && panel_ != nullptr && buffers_[0] != nullptr && buffers_[1] != nullptr;
}

uint8_t* Tab5DpiFramebuffers::AcquireFree() {
    if (!Ready() && !Resolve()) {
        return nullptr;
    }
    return static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
}

uint8_t* Tab5DpiFramebuffers::Displayed() {
    uint8_t* free_buffer = AcquireFree();
    if (free_buffer == buffers_[0]) {
        return buffers_[1];
    }
    return free_buffer == buffers_[1] ? buffers_[0] : nullptr;
}

bool Tab5DpiFramebuffers::Contains(const uint8_t* buffer) const {
    return buffer != nullptr && (buffer == buffers_[0] || buffer == buffers_[1]);
}

esp_err_t Tab5DpiFramebuffers::Submit(uint8_t* buffer) {
    return Ready() && Contains(buffer) ? esp_lv_adapter_dummy_draw_flush_buf(display_, buffer) : ESP_ERR_INVALID_ARG;
}

lvgl::DirectScanoutProfile Tab5DirectScanoutProfile() {
    return {.mode = lvgl::DirectScanoutProfile::Mode::kFramebufferRgb565,
            .rgb565_byte_swapped = false,
            .max_full_frame_fps = kDirectScanoutMaxFps};
}

}  // namespace micropixel::platform::m5stack_tab5
