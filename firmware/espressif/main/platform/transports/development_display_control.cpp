#include "platform/transports/development_display_control.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::platform::transports {
namespace {

constexpr char kTag[] = "development_display";
constexpr char kCaptureCommand[] = "MICROPIXEL_CAPTURE";
constexpr char kTouchCommand[] = "MICROPIXEL_TOUCH";

bool TransmitJpeg(const uint8_t* jpeg_bytes, uint32_t jpeg_size, uint32_t sequence, uint32_t width, uint32_t height,
                  DevelopmentLocalControlTransport& transport) {
    char begin[128]{};
    const int begin_length = std::snprintf(
        begin, sizeof(begin), "\nMICROPIXEL_CAPTURE_BEGIN %" PRIu32 " %" PRIu32 " %" PRIu32 " JPEG %" PRIu32 " %s\n",
        sequence, width, height, jpeg_size, "DISPLAY");
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

esp_err_t DevelopmentDisplayControl::Start(lv_display_t* display, device::Input& input,
                                           DevelopmentLocalControlTransport& transport, uint32_t width, uint32_t height,
                                           lvgl::DisplayCaptureSource display_source) {
    if (display == nullptr || width == 0U || height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    display_ = display;
    input_ = &input;
    transport_ = &transport;
    width_ = width;
    height_ = height;
    display_source_ = display_source;
    ESP_LOGI(kTag, "development commands=%s, %s <phase> <id> <x> <y> <pressure>", kCaptureCommand, kTouchCommand);
    return transport.Start(ReceiveCommand, this);
}

void DevelopmentDisplayControl::ReceiveCommand(void* context, const char* command) {
    static_cast<DevelopmentDisplayControl*>(context)->ProcessCommand(command);
}

void DevelopmentDisplayControl::ProcessCommand(const char* command) {
    if (std::strcmp(command, kCaptureCommand) == 0) {
        CaptureAndTransmit();
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

void DevelopmentDisplayControl::CaptureAndTransmit() {
    auto capture_result = lvgl::CaptureScreenJpeg(display_, width_, height_, display_source_);
    if (!capture_result.has_value()) {
        ESP_LOGE(kTag, "display-buffer screen capture failed");
        return;
    }

    host_ui::ScreenCapture capture = std::move(capture_result.value());
    if (!capture.valid() || capture.size() > UINT32_MAX) {
        ESP_LOGE(kTag, "invalid display-buffer JPEG capture");
        return;
    }
    const uint32_t jpeg_size = static_cast<uint32_t>(capture.size());
    const uint32_t capture_width = capture.width();
    const uint32_t capture_height = capture.height();
    const host_ui::ScreenCaptureRelease release = capture.releaser();
    uint8_t* jpeg_bytes = capture.Detach();
    const uint32_t sequence = ++sequence_;
    const bool transmitted = TransmitJpeg(jpeg_bytes, jpeg_size, sequence, capture_width, capture_height, *transport_);
    release(jpeg_bytes);
    if (transmitted) {
        ESP_LOGI(kTag, "display-buffer JPEG capture #%" PRIu32 " complete: bytes=%" PRIu32, sequence, jpeg_size);
    } else {
        ESP_LOGE(kTag, "JPEG capture #%" PRIu32 " USB transfer failed", sequence);
    }
}

}  // namespace micropixel::platform::transports
