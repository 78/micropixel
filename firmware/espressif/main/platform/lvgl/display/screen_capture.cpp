#include "platform/lvgl/display/screen_capture.hpp"

#include <cstddef>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "soc/soc_caps.h"
#if SOC_JPEG_ENCODE_SUPPORTED
#include "driver/jpeg_encode.h"
#endif

namespace micropixel::platform::lvgl {
namespace {

constexpr char kTag[] = "micropixel_capture";
constexpr uint32_t kJpegQuality = 85U;

struct RawCapture final {
    uint8_t* pixels{};
    size_t capacity{};
    uint32_t bytes{};
    DisplayCapturePixelFormat format{DisplayCapturePixelFormat::kRgb565};
};

void Release(RawCapture& capture) {
    heap_caps_free(capture.pixels);
    capture = {};
}

void ReleaseJpeg(uint8_t* data) { heap_caps_free(data); }

bool AllocateInput(uint32_t bytes, RawCapture& capture) {
#if SOC_JPEG_ENCODE_SUPPORTED
    jpeg_encode_memory_alloc_cfg_t config{
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    capture.pixels = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(bytes, &config, &capture.capacity));
#else
    capture.capacity = bytes;
    capture.pixels =
        static_cast<uint8_t*>(heap_caps_aligned_calloc(16U, bytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    capture.bytes = bytes;
    return capture.pixels != nullptr && capture.capacity >= bytes;
}

bool CaptureDisplayBuffer(lv_display_t* display, uint32_t width, uint32_t height,
                          const DisplayCaptureSource& configured_source, RawCapture& capture) {
    if (width == 0U || height == 0U) {
        return false;
    }
    bool success = false;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        const uint8_t* source = configured_source.pixels;
        uint32_t source_stride = configured_source.stride;
        uint32_t bytes_per_pixel = configured_source.format == DisplayCapturePixelFormat::kRgb888 ? 3U : 2U;
        bool source_ready = source != nullptr && (configured_source.ready == nullptr || *configured_source.ready);
        if (!source_ready) {
            const lv_draw_buf_t* active = lv_display_get_buf_active(display);
            source_ready =
                active != nullptr && active->data != nullptr && active->header.w == width && active->header.h == height;
            if (source_ready && active->header.cf == LV_COLOR_FORMAT_RGB565) {
                bytes_per_pixel = 2U;
            } else if (source_ready && active->header.cf == LV_COLOR_FORMAT_RGB888) {
                bytes_per_pixel = 3U;
            } else {
                source_ready = false;
            }
            if (source_ready) {
                source = static_cast<const uint8_t*>(active->data);
                source_stride = active->header.stride;
            }
        }
        const bool dimensions_valid =
            width <= UINT32_MAX / bytes_per_pixel && height <= UINT32_MAX / (width * bytes_per_pixel);
        const uint32_t compact_stride = dimensions_valid ? width * bytes_per_pixel : 0U;
        const uint32_t bytes = dimensions_valid ? compact_stride * height : 0U;
        success = source_ready && source_stride >= compact_stride && AllocateInput(bytes, capture);
        if (success) {
            for (uint32_t row = 0U; row < height; ++row) {
                std::memcpy(capture.pixels + static_cast<size_t>(row) * compact_stride,
                            source + static_cast<size_t>(row) * source_stride, compact_stride);
            }
            capture.format =
                bytes_per_pixel == 3U ? DisplayCapturePixelFormat::kRgb888 : DisplayCapturePixelFormat::kRgb565;
        }
        esp_lv_adapter_unlock();
    }
    if (!success) {
        Release(capture);
    }
    return success;
}

bool EncodeJpeg(const RawCapture& capture, uint32_t width, uint32_t height, uint8_t*& jpeg_bytes, uint32_t& jpeg_size) {
#if SOC_JPEG_ENCODE_SUPPORTED
    jpeg_encode_memory_alloc_cfg_t output_config{
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t output_capacity = 0U;
    jpeg_bytes = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(capture.bytes, &output_config, &output_capacity));
    if (jpeg_bytes == nullptr || output_capacity > UINT32_MAX) {
        heap_caps_free(jpeg_bytes);
        jpeg_bytes = nullptr;
        return false;
    }

    jpeg_encode_engine_cfg_t engine_config{};
    engine_config.timeout_ms = 500U;
    jpeg_encoder_handle_t encoder = nullptr;
    esp_err_t status = jpeg_new_encoder_engine(&engine_config, &encoder);
    if (status == ESP_OK) {
        jpeg_encode_cfg_t encode_config{};
        encode_config.height = height;
        encode_config.width = width;
        encode_config.src_type = capture.format == DisplayCapturePixelFormat::kRgb888 ? JPEG_ENCODE_IN_FORMAT_RGB888
                                                                                      : JPEG_ENCODE_IN_FORMAT_RGB565;
        encode_config.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
        encode_config.image_quality = kJpegQuality;
        status = jpeg_encoder_process(encoder, &encode_config, capture.pixels, capture.bytes, jpeg_bytes,
                                      static_cast<uint32_t>(output_capacity), &jpeg_size);
    }
    if (encoder != nullptr) {
        const esp_err_t release_status = jpeg_del_encoder_engine(encoder);
        if (status == ESP_OK) {
            status = release_status;
        }
    }
    if (status != ESP_OK || jpeg_size == 0U) {
        ESP_LOGE(kTag, "hardware JPEG encode failed: %s", esp_err_to_name(status));
        heap_caps_free(jpeg_bytes);
        jpeg_bytes = nullptr;
        jpeg_size = 0U;
        return false;
    }
    return true;
#else
    if (width > INT32_MAX || height > INT32_MAX || capture.bytes > INT32_MAX) {
        return false;
    }
    const uint32_t output_capacity = capture.bytes;
    jpeg_bytes =
        static_cast<uint8_t*>(heap_caps_aligned_calloc(16U, output_capacity, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg_bytes == nullptr) {
        return false;
    }
    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = static_cast<int>(width);
    config.height = static_cast<int>(height);
    config.src_type =
        capture.format == DisplayCapturePixelFormat::kRgb888 ? JPEG_PIXEL_FORMAT_RGB888 : JPEG_PIXEL_FORMAT_RGB565_LE;
    config.subsampling = JPEG_SUBSAMPLE_420;
    config.quality = kJpegQuality;
    config.task_enable = false;
    jpeg_enc_handle_t encoder = nullptr;
    jpeg_error_t status = jpeg_enc_open(&config, &encoder);
    int encoded_size = 0;
    if (status == JPEG_ERR_OK) {
        status = jpeg_enc_process(encoder, capture.pixels, static_cast<int>(capture.bytes), jpeg_bytes,
                                  static_cast<int>(output_capacity), &encoded_size);
    }
    if (encoder != nullptr) {
        const jpeg_error_t close_status = jpeg_enc_close(encoder);
        if (status == JPEG_ERR_OK) {
            status = close_status;
        }
    }
    if (status != JPEG_ERR_OK || encoded_size <= 0) {
        ESP_LOGE(kTag, "software JPEG encode failed: status=%d", static_cast<int>(status));
        heap_caps_free(jpeg_bytes);
        jpeg_bytes = nullptr;
        return false;
    }
    jpeg_size = static_cast<uint32_t>(encoded_size);
    return true;
#endif
}

}  // namespace

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg(lv_display_t* display, uint32_t width,
                                                                                uint32_t height,
                                                                                DisplayCaptureSource display_source) {
    if (display == nullptr || width == 0U || height == 0U) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }

    RawCapture capture{};
    if (!CaptureDisplayBuffer(display, width, height, display_source, capture)) {
        ESP_LOGE(kTag, "display-buffer screen capture failed");
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }

    uint8_t* jpeg_bytes = nullptr;
    uint32_t jpeg_size = 0U;
    const bool encoded = EncodeJpeg(capture, width, height, jpeg_bytes, jpeg_size);
    Release(capture);
    if (!encoded) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    return host_ui::ScreenCapture(jpeg_bytes, jpeg_size, width, height, ReleaseJpeg);
}

}  // namespace micropixel::platform::lvgl
