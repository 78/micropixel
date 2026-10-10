#include "platform/boards/ksdiy-p4c5/display_pipeline.hpp"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lv_adapter.h"

namespace micropixel::platform::ksdiy_p4c5 {

void DisplayPipeline::DpiFramebuffers::Bind(lv_display_t* display, esp_lcd_panel_handle_t panel) {
    display_ = display;
    panel_ = panel;
    buffers_ = {};
    (void)Resolve();
}

bool DisplayPipeline::DpiFramebuffers::Resolve() {
    if (display_ == nullptr || panel_ == nullptr) {
        buffers_ = {};
        return false;
    }
    void* first = nullptr;
    void* second = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel_, 2U, &first, &second) != ESP_OK) {
        buffers_ = {};
        return false;
    }
    buffers_[0] = static_cast<uint8_t*>(first);
    buffers_[1] = static_cast<uint8_t*>(second);
    return buffers_[0] != nullptr && buffers_[1] != nullptr;
}

bool DisplayPipeline::DpiFramebuffers::Ready() const {
    return display_ != nullptr && panel_ != nullptr && buffers_[0] != nullptr && buffers_[1] != nullptr;
}

uint8_t* DisplayPipeline::DpiFramebuffers::AcquireFree() {
    if (!Ready() && !Resolve()) {
        return nullptr;
    }
    return static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
}

uint8_t* DisplayPipeline::DpiFramebuffers::Displayed() {
    uint8_t* free = AcquireFree();
    if (free == buffers_[0]) {
        return buffers_[1];
    }
    return free == buffers_[1] ? buffers_[0] : nullptr;
}

bool DisplayPipeline::DpiFramebuffers::Contains(const uint8_t* buffer) const {
    return buffer != nullptr && (buffer == buffers_[0] || buffer == buffers_[1]);
}

esp_err_t DisplayPipeline::DpiFramebuffers::Submit(uint8_t* buffer) {
    return Ready() && Contains(buffer) ? esp_lv_adapter_dummy_draw_flush_buf(display_, buffer) : ESP_ERR_INVALID_ARG;
}

void DisplayPipeline::BindLvgl(lv_display_t* display) {
    if (!board::kLandscape) {
        framebuffers_.Bind(display, board_io_.Panel());
    }
}

lvgl::DisplayCapabilities DisplayPipeline::Capabilities() const {
    return {.partial_flush = true,
            .tearing_effect_sync = true,
            .direct_framebuffers = !board::kLandscape,
            .ppa = true,
            .dma2d = true,
            .hardware_jpeg = true};
}

lvgl::DirectScanoutProfile DisplayPipeline::DirectScanout() const {
    if (board::kLandscape) {
        return {};
    }
    return {.mode = lvgl::DirectScanoutProfile::Mode::kFramebufferRgb888,
            .rgb565_byte_swapped = false,
            .max_full_frame_fps = kDirectScanoutMaxFps};
}

}  // namespace micropixel::platform::ksdiy_p4c5
