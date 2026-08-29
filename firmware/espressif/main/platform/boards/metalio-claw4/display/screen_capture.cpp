#include "platform/boards/metalio-claw4/display/screen_capture.hpp"

#include <cstddef>
#include <cstdint>

#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "platform/input/gt911_input.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/transports/usb_serial_jtag_local_control.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_capture";

struct DevelopmentCapture final {
    transports::UsbSerialJtagLocalControl transport{};
    transports::DevelopmentDisplayControl display_control{};
};

DevelopmentCapture& DevelopmentInstance() {
    static DevelopmentCapture instance;
    return instance;
}

void ReleaseCaptureBuffer(uint8_t* data) { heap_caps_free(data); }

uint8_t* DisplayedFrameBuffer(lv_display_t* display, esp_lcd_panel_handle_t panel) {
    constexpr uint32_t kFramebufferCount = 2U;
    void* panel_frame_buffers[kFramebufferCount]{};
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, kFramebufferCount, &panel_frame_buffers[0],
                                           &panel_frame_buffers[1]) != ESP_OK) {
        return nullptr;
    }
    auto* free_frame_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display));
    if (free_frame_buffer == panel_frame_buffers[0]) {
        return static_cast<uint8_t*>(panel_frame_buffers[1]);
    }
    if (free_frame_buffer == panel_frame_buffers[1]) {
        return static_cast<uint8_t*>(panel_frame_buffers[0]);
    }
    return nullptr;
}

}  // namespace

esp_err_t InitializeScreenCapture(lv_display_t* display, input::Gt911Input& touch_input, uint32_t width,
                                  uint32_t height) {
    auto& development = DevelopmentInstance();
    return development.display_control.Start(display, touch_input, development.transport, width, height);
}

device::LocalControl& UsbLocalControl() { return DevelopmentInstance().transport; }

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg(lv_display_t* display,
                                                                                esp_lcd_panel_handle_t panel,
                                                                                uint32_t width, uint32_t height) {
    if (display == nullptr || panel == nullptr || width == 0U || height == 0U || height > UINT32_MAX / width / 3U) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    const uint32_t frame_bytes = width * height * 3U;
    jpeg_encode_memory_alloc_cfg_t output_memory_config{
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t output_capacity = 0U;
    auto* jpeg_bytes =
        static_cast<uint8_t*>(jpeg_alloc_encoder_mem(frame_bytes, &output_memory_config, &output_capacity));
    if (jpeg_bytes == nullptr || output_capacity > UINT32_MAX) {
        heap_caps_free(jpeg_bytes);
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }

    // Dummy draw freezes the physical framebuffers while LVGL continues to
    // render into its private buffer. If a compositor already owns dummy draw,
    // preserve that ownership after the capture.
    const bool dummy_draw_was_enabled = esp_lv_adapter_dummy_draw_get_free_buf_preserve(display) != nullptr;
    esp_err_t status = dummy_draw_was_enabled ? ESP_OK : esp_lv_adapter_set_dummy_draw(display, true);
    uint8_t* displayed_frame = status == ESP_OK ? DisplayedFrameBuffer(display, panel) : nullptr;

    jpeg_encoder_handle_t encoder = nullptr;
    if (displayed_frame != nullptr) {
        jpeg_encode_engine_cfg_t engine_config{};
        engine_config.timeout_ms = 200;
        status = jpeg_new_encoder_engine(&engine_config, &encoder);
    } else {
        status = ESP_FAIL;
    }

    uint32_t jpeg_size = 0U;
    if (status == ESP_OK) {
        jpeg_encode_cfg_t encode_config{};
        encode_config.height = height;
        encode_config.width = width;
        encode_config.src_type = JPEG_ENCODE_IN_FORMAT_RGB888;
        encode_config.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
        encode_config.image_quality = 80U;
        status = jpeg_encoder_process(encoder, &encode_config, displayed_frame, frame_bytes, jpeg_bytes,
                                      static_cast<uint32_t>(output_capacity), &jpeg_size);
    }
    if (encoder != nullptr) {
        const esp_err_t release_status = jpeg_del_encoder_engine(encoder);
        if (status == ESP_OK) {
            status = release_status;
        }
    }
    if (!dummy_draw_was_enabled) {
        const esp_err_t restore_status = esp_lv_adapter_set_dummy_draw(display, false);
        if (status == ESP_OK) {
            status = restore_status;
        }
    }
    if (status != ESP_OK || jpeg_size == 0U) {
        ESP_LOGE(kTag, "Remote Control JPEG capture failed: %s", esp_err_to_name(status));
        heap_caps_free(jpeg_bytes);
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    return host_ui::ScreenCapture(jpeg_bytes, jpeg_size, width, height, ReleaseCaptureBuffer);
}

}  // namespace micropixel::platform::metalio_claw4
