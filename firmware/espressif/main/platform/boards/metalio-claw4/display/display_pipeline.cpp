#include "platform/boards/metalio-claw4/display/display_pipeline.hpp"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lv_adapter.h"

namespace micropixel::platform::metalio_claw4 {

void MetalioClaw4DisplayPipeline::DpiFramebuffers::Bind(lv_display_t* display, esp_lcd_panel_handle_t panel) {
    display_ = display;
    panel_ = panel;
    buffers_ = {};
    (void)Resolve();
}

bool MetalioClaw4DisplayPipeline::DpiFramebuffers::Resolve() {
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

bool MetalioClaw4DisplayPipeline::DpiFramebuffers::Ready() const {
    return display_ != nullptr && panel_ != nullptr && buffers_[0] != nullptr && buffers_[1] != nullptr;
}

uint8_t* MetalioClaw4DisplayPipeline::DpiFramebuffers::AcquireFree() {
    if (!Ready() && !Resolve()) {
        return nullptr;
    }
    return static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
}

uint8_t* MetalioClaw4DisplayPipeline::DpiFramebuffers::Displayed() {
    uint8_t* free = AcquireFree();
    if (free == buffers_[0]) {
        return buffers_[1];
    }
    return free == buffers_[1] ? buffers_[0] : nullptr;
}

bool MetalioClaw4DisplayPipeline::DpiFramebuffers::Contains(const uint8_t* buffer) const {
    return buffer != nullptr && (buffer == buffers_[0] || buffer == buffers_[1]);
}

esp_err_t MetalioClaw4DisplayPipeline::DpiFramebuffers::Submit(uint8_t* buffer) {
    return Ready() && Contains(buffer) ? esp_lv_adapter_dummy_draw_flush_buf(display_, buffer) : ESP_ERR_INVALID_ARG;
}

void MetalioClaw4DisplayPipeline::BindLvgl(lv_display_t* display) {
    display_ = display;
    framebuffers_.Bind(display_, hardware_.Panel());
}

void MetalioClaw4DisplayPipeline::RebindPanel() { framebuffers_.Bind(display_, hardware_.Panel()); }

lvgl::DisplayCapabilities MetalioClaw4DisplayPipeline::Capabilities() const {
    return {.partial_flush = true,
            .tearing_effect_sync = true,
            .direct_framebuffers = true,
            .ppa = true,
            .dma2d = true,
            .hardware_jpeg = true};
}

esp_err_t MetalioClaw4DisplayPipeline::Suspend() {
    framebuffers_.Bind(nullptr, nullptr);
    return hardware_.SuspendDisplay();
}

esp_err_t MetalioClaw4DisplayPipeline::Resume() {
    const esp_err_t status = hardware_.ResumeDisplay();
    if (status == ESP_OK) {
        RebindPanel();
    }
    return status;
}

esp_err_t MetalioClaw4DisplayPipeline::SetBrightness(uint32_t per_ten_thousand) {
    return hardware_.SetBacklightOutputPerTenThousand(per_ten_thousand);
}

}  // namespace micropixel::platform::metalio_claw4
