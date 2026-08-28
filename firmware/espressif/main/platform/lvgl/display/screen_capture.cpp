#include "platform/lvgl/display/screen_capture.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "src/draw/snapshot/lv_snapshot.h"

namespace micropixel::platform::lvgl {
namespace {

constexpr char kTag[] = "micropixel_capture";
constexpr char kCaptureCommand[] = "MICROPIXEL_CAPTURE";
constexpr char kTouchCommand[] = "MICROPIXEL_TOUCH";
constexpr uint32_t kJpegQuality = 85U;

struct RawCapture final {
    uint8_t* pixels{};
    size_t capacity{};
    uint32_t bytes{};
    jpeg_enc_input_format_t format{};
};

void Release(RawCapture& capture) {
    heap_caps_free(capture.pixels);
    capture = {};
}

bool AllocateInput(uint32_t bytes, RawCapture& capture) {
    jpeg_encode_memory_alloc_cfg_t config{
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    capture.pixels = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(bytes, &config, &capture.capacity));
    capture.bytes = bytes;
    return capture.pixels != nullptr && capture.capacity >= bytes;
}

bool CaptureLogical(lv_display_t* display, uint32_t width, uint32_t height, RawCapture& capture) {
    if (width > UINT32_MAX / 3U || height > UINT32_MAX / (width * 3U)) {
        return false;
    }
    const uint32_t stride = width * 3U;
    const uint32_t bytes = stride * height;
    if (!AllocateInput(bytes, capture)) {
        return false;
    }
    lv_draw_buf_t snapshot{};
    bool success = lv_draw_buf_init(&snapshot, width, height, LV_COLOR_FORMAT_RGB888, stride, capture.pixels,
                                    capture.capacity) == LV_RESULT_OK;
    if (success && esp_lv_adapter_lock(-1) == ESP_OK) {
        success = lv_snapshot_take_to_draw_buf(lv_display_get_screen_active(display), LV_COLOR_FORMAT_RGB888,
                                               &snapshot) == LV_RESULT_OK;
        esp_lv_adapter_unlock();
    } else {
        success = false;
    }
    capture.format = JPEG_ENCODE_IN_FORMAT_RGB888;
    if (!success) {
        Release(capture);
    }
    return success;
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
            capture.format = bytes_per_pixel == 3U ? JPEG_ENCODE_IN_FORMAT_RGB888 : JPEG_ENCODE_IN_FORMAT_RGB565;
        }
        esp_lv_adapter_unlock();
    }
    if (!success) {
        Release(capture);
    }
    return success;
}

bool EncodeJpeg(const RawCapture& capture, uint32_t width, uint32_t height, uint8_t*& jpeg_bytes, uint32_t& jpeg_size) {
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
        encode_config.src_type = capture.format;
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
}

bool TransmitJpeg(const uint8_t* jpeg_bytes, uint32_t jpeg_size, uint32_t sequence, uint32_t width, uint32_t height,
                  ScreenCaptureDevelopment::Source source, transports::DevelopmentLocalControlTransport& transport) {
    const char* source_name = source == ScreenCaptureDevelopment::Source::kLogical ? "LOGICAL" : "DISPLAY";
    char begin[128]{};
    const int begin_length = std::snprintf(
        begin, sizeof(begin), "\nMICROPIXEL_CAPTURE_BEGIN %" PRIu32 " %" PRIu32 " %" PRIu32 " JPEG %" PRIu32 " %s\n",
        sequence, width, height, jpeg_size, source_name);
    char end[80]{};
    const int end_length = std::snprintf(end, sizeof(end), "\nMICROPIXEL_CAPTURE_END %" PRIu32 "\n", sequence);
    if (begin_length <= 0 || static_cast<size_t>(begin_length) >= sizeof(begin) || end_length <= 0 ||
        static_cast<size_t>(end_length) >= sizeof(end)) {
        return false;
    }

    transport.LockOutput();
    const bool success = transport.WriteAll(begin, static_cast<size_t>(begin_length)) &&
                         transport.WriteAll(jpeg_bytes, jpeg_size) &&
                         transport.WriteAll(end, static_cast<size_t>(end_length));
    transport.FlushOutput(3000U);
    transport.UnlockOutput();
    return success;
}

}  // namespace

esp_err_t ScreenCaptureDevelopment::Start(lv_display_t* display, device::InputBackend& input,
                                          transports::DevelopmentLocalControlTransport& transport, uint32_t width,
                                          uint32_t height, DisplayCaptureSource display_source) {
    if (display == nullptr || width == 0U || height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    display_ = display;
    input_ = &input;
    transport_ = &transport;
    width_ = width;
    height_ = height;
    display_source_ = display_source;
    ESP_LOGI(kTag, "development commands=%s [LOGICAL|DISPLAY], %s <phase> <id> <x> <y> <pressure>", kCaptureCommand,
             kTouchCommand);
    return transport.Start(ReceiveDevelopmentCommand, this);
}

void ScreenCaptureDevelopment::ReceiveDevelopmentCommand(void* context, const char* command) {
    static_cast<ScreenCaptureDevelopment*>(context)->ProcessCommand(command);
}

void ScreenCaptureDevelopment::ProcessCommand(const char* command) {
    if (std::strcmp(command, kCaptureCommand) == 0 || std::strcmp(command, "MICROPIXEL_CAPTURE LOGICAL") == 0) {
        CaptureAndTransmit(Source::kLogical);
        return;
    }
    if (std::strcmp(command, "MICROPIXEL_CAPTURE DISPLAY") == 0) {
        CaptureAndTransmit(Source::kDisplayBuffer);
        return;
    }

    char phase_name[8]{};
    uint32_t id = 0U;
    uint32_t x = 0U;
    uint32_t y = 0U;
    uint32_t pressure = 0U;
    int consumed = 0;
    const int fields = std::sscanf(command, "MICROPIXEL_TOUCH %7s %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %n",
                                   phase_name, &id, &x, &y, &pressure, &consumed);
    if (fields != 5 || command[consumed] != '\0' || x >= width_ || y >= height_ || pressure > 1000U ||
        input_ == nullptr) {
        ESP_LOGW(kTag, "invalid development command: %s", command);
        return;
    }

    device::TouchPhase phase{};
    if (std::strcmp(phase_name, "DOWN") == 0) {
        phase = device::TouchPhase::kDown;
    } else if (std::strcmp(phase_name, "MOVE") == 0) {
        phase = device::TouchPhase::kMove;
    } else if (std::strcmp(phase_name, "UP") == 0) {
        phase = device::TouchPhase::kUp;
    } else if (std::strcmp(phase_name, "CANCEL") == 0) {
        phase = device::TouchPhase::kCancel;
    } else {
        ESP_LOGW(kTag, "invalid touch phase: %s", phase_name);
        return;
    }

    const device::TouchSample sample{
        .timestamp_us = static_cast<uint64_t>(esp_timer_get_time()),
        .id = id,
        .x = static_cast<int32_t>(x),
        .y = static_cast<int32_t>(y),
        .pressure_per_mille = static_cast<uint16_t>(pressure),
        .phase = phase,
    };
    if (!input_->InjectTouch(sample)) {
        ESP_LOGW(kTag, "touch injection rejected: phase=%s id=%" PRIu32, phase_name, id);
    }
}

void ScreenCaptureDevelopment::CaptureAndTransmit(Source source) {
    RawCapture capture{};
    const bool captured = source == Source::kLogical
                              ? CaptureLogical(display_, width_, height_, capture)
                              : CaptureDisplayBuffer(display_, width_, height_, display_source_, capture);
    if (!captured) {
        ESP_LOGE(kTag, "%s screen capture failed", source == Source::kLogical ? "logical" : "display-buffer");
        return;
    }

    uint8_t* jpeg_bytes = nullptr;
    uint32_t jpeg_size = 0U;
    const bool encoded = EncodeJpeg(capture, width_, height_, jpeg_bytes, jpeg_size);
    Release(capture);
    if (!encoded) {
        return;
    }
    const uint32_t sequence = ++sequence_;
    const bool transmitted = TransmitJpeg(jpeg_bytes, jpeg_size, sequence, width_, height_, source, *transport_);
    heap_caps_free(jpeg_bytes);
    if (transmitted) {
        ESP_LOGI(kTag, "%s JPEG capture #%" PRIu32 " complete: bytes=%" PRIu32,
                 source == Source::kLogical ? "logical" : "display-buffer", sequence, jpeg_size);
    } else {
        ESP_LOGE(kTag, "JPEG capture #%" PRIu32 " USB transfer failed", sequence);
    }
}

}  // namespace micropixel::platform::lvgl
