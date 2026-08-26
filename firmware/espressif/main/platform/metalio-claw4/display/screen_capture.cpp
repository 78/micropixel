#include "platform/metalio-claw4/display/screen_capture.hpp"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/jpeg_encode.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_private/log_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "platform/metalio-claw4/input/gt911_input.hpp"
#include "png.h"
#include "src/draw/snapshot/lv_snapshot.h"
#include "task_policy.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_capture";
constexpr uint32_t kTaskStackSize = 8U * 1024U;
constexpr BaseType_t kTaskCore = 0;
constexpr char kCaptureCommand[] = "MICROPIXEL_CAPTURE";
constexpr char kTouchCommand[] = "MICROPIXEL_TOUCH";
constexpr char kLocalControlPrefix[] = "MPX1 ";
constexpr size_t kLocalControlResponseCapacity = 1024U;

struct PngUsbStream final {
    size_t bytes{};
    bool failed{};
};

bool UsbWriteAll(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (size > 0U) {
        // The ESP-IDF no-split TX ring cannot accept an item approaching the
        // size of the whole ring. libpng commonly hands us 8 KiB IDAT chunks,
        // so feed the 4 KiB USB driver ring in bounded records.
        size_t chunk = size > 1024U ? 1024U : size;
        int written = usb_serial_jtag_write_bytes(bytes, chunk, pdMS_TO_TICKS(1000));
        if (written <= 0) {
            return false;
        }
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

void PngUsbWrite(png_structp png, png_bytep data, png_size_t size) {
    auto* stream = static_cast<PngUsbStream*>(png_get_io_ptr(png));
    if (stream == nullptr || !UsbWriteAll(data, size)) {
        if (stream != nullptr) {
            stream->failed = true;
        }
        png_error(png, "USB Serial/JTAG write failed");
    }
    stream->bytes += size;
}

void PngUsbFlush(png_structp) { (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000)); }

void PngCaptureError(png_structp png, png_const_charp) { png_longjmp(png, 1); }

void PngCaptureWarning(png_structp, png_const_charp) {}

png_voidp PngPsramAlloc(png_structp, png_alloc_size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void PngPsramFree(png_structp, png_voidp memory) { heap_caps_free(memory); }

bool TransmitSnapshotPng(const lv_draw_buf_t& snapshot, uint32_t sequence, size_t& png_bytes) {
    png_structp png = png_create_write_struct_2(PNG_LIBPNG_VER_STRING, nullptr, PngCaptureError, PngCaptureWarning,
                                                nullptr, PngPsramAlloc, PngPsramFree);
    if (png == nullptr) {
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        return false;
    }

    PngUsbStream stream{};
    bool success = false;
    volatile bool log_locked = false;
    if (setjmp(png_jmpbuf(png)) == 0) {
        png_set_write_fn(png, &stream, PngUsbWrite, PngUsbFlush);
        png_set_IHDR(png, info, snapshot.header.w, snapshot.header.h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_compression_level(png, 3);
        // LVGL's little-endian RGB888 storage is B,G,R; PNG rows are R,G,B.
        png_set_bgr(png);

        char begin[96]{};
        int begin_length =
            std::snprintf(begin, sizeof(begin), "\nMICROPIXEL_CAPTURE_BEGIN %" PRIu32 " %" PRIu32 " %" PRIu32 "\n",
                          sequence, static_cast<uint32_t>(snapshot.header.w), static_cast<uint32_t>(snapshot.header.h));

        esp_log_impl_lock();
        log_locked = true;
        (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
        bool began = begin_length > 0 && static_cast<size_t>(begin_length) < sizeof(begin) &&
                     UsbWriteAll(begin, static_cast<size_t>(begin_length));
        if (began) {
            png_write_info(png, info);
            const auto* pixels = static_cast<const uint8_t*>(snapshot.data);
            for (uint32_t row = 0U; row < snapshot.header.h; ++row) {
                png_write_row(png, const_cast<png_bytep>(pixels + static_cast<size_t>(row) * snapshot.header.stride));
            }
            png_write_end(png, info);
        }
        char end[80]{};
        int end_length = std::snprintf(end, sizeof(end), "\nMICROPIXEL_CAPTURE_END %" PRIu32 "\n", sequence);
        bool ended = end_length > 0 && static_cast<size_t>(end_length) < sizeof(end) &&
                     UsbWriteAll(end, static_cast<size_t>(end_length));
        (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(3000));
        esp_log_impl_unlock();
        log_locked = false;
        success = began && ended && !stream.failed;
    } else if (log_locked) {
        esp_log_impl_unlock();
        log_locked = false;
    }

    png_bytes = stream.bytes;
    png_destroy_write_struct(&png, &info);
    return success;
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

class ScreenCapture final : public device::LocalControlBackend {
   public:
    [[nodiscard]] esp_err_t Start(lv_display_t* display, Gt911Input& touch_input, uint32_t width, uint32_t height) {
        if (display == nullptr || width == 0U || height == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
        display_ = display;
        touch_input_ = &touch_input;
        width_ = width;
        height_ = height;
        if (!usb_serial_jtag_is_driver_installed()) {
            usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
            usb_config.rx_buffer_size = 8192U;
            usb_config.tx_buffer_size = 4096U;
            esp_err_t status = usb_serial_jtag_driver_install(&usb_config);
            if (status != ESP_OK) {
                return status;
            }
        }
        if (xTaskCreatePinnedToCore(TaskEntry, "micropixel_capture", kTaskStackSize, this,
                                    task_policy::kCapturePriority, &task_, kTaskCore) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    void Bind(device::LocalControlCommandSink command_sink, device::LocalControlResponseSource response_source,
              void* context) override {
        command_context_.store(context, std::memory_order_release);
        response_source_.store(response_source, std::memory_order_release);
        command_sink_.store(command_sink, std::memory_order_release);
    }

    void Unbind(void* context) override {
        if (command_context_.load(std::memory_order_acquire) != context) {
            return;
        }
        command_sink_.store(nullptr, std::memory_order_release);
        response_source_.store(nullptr, std::memory_order_release);
        command_context_.store(nullptr, std::memory_order_release);
    }

   private:
    static void TaskEntry(void* context) { static_cast<ScreenCapture*>(context)->Run(); }

    void Run() {
        ESP_LOGI(kTag, "screen capture ready; commands=%s, %s <phase> <id> <x> <y> <pressure>", kCaptureCommand,
                 kTouchCommand);
        for (;;) {
            std::array<uint8_t, 512U> bytes{};
            int received = usb_serial_jtag_read_bytes(bytes.data(), bytes.size(), pdMS_TO_TICKS(20U));
            if (received <= 0) {
                DrainControlResponses();
                continue;
            }
            for (int index = 0; index < received; ++index) {
                const uint8_t byte = bytes[static_cast<size_t>(index)];
                if (byte == '\r') {
                    continue;
                }
                if (byte == '\n') {
                    command_[command_length_] = '\0';
                    ProcessCommand();
                    command_length_ = 0U;
                    continue;
                }
                if (byte >= 0x20U && byte <= 0x7eU && command_length_ + 1U < sizeof(command_)) {
                    command_[command_length_++] = static_cast<char>(byte);
                } else {
                    command_length_ = 0U;
                }
            }
            DrainControlResponses();
        }
    }

    void ProcessCommand() {
        if (std::strncmp(command_, kLocalControlPrefix, sizeof(kLocalControlPrefix) - 1U) == 0) {
            device::LocalControlCommandSink sink = command_sink_.load(std::memory_order_acquire);
            void* context = command_context_.load(std::memory_order_acquire);
            if (sink != nullptr && context != nullptr) {
                sink(context, command_);
            }
            return;
        }
        if (std::strcmp(command_, kCaptureCommand) == 0) {
            CaptureAndTransmit();
            return;
        }

        char phase_name[8]{};
        uint32_t id = 0U;
        uint32_t x = 0U;
        uint32_t y = 0U;
        uint32_t pressure = 0U;
        int consumed = 0;
        const int fields =
            std::sscanf(command_, "MICROPIXEL_TOUCH %7s %" SCNu32 " %" SCNu32 " %" SCNu32 " %" SCNu32 " %n", phase_name,
                        &id, &x, &y, &pressure, &consumed);
        if (fields != 5 || command_[consumed] != '\0' || x >= width_ || y >= height_ || pressure > 1000U ||
            touch_input_ == nullptr) {
            ESP_LOGW(kTag, "invalid capture command: %s", command_);
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

        device::TouchSample sample{};
        sample.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
        sample.id = id;
        sample.x = static_cast<int32_t>(x);
        sample.y = static_cast<int32_t>(y);
        /* Capture injection follows the selected GT911 backend capabilities. */
        sample.pressure_per_mille = static_cast<uint16_t>(pressure);
        sample.phase = phase;
        touch_input_->InjectTouchForCapture(sample);
        ESP_LOGI(kTag, "injected touch phase=%s id=%" PRIu32 " x=%" PRIu32 " y=%" PRIu32, phase_name, id, x, y);
    }

    void DrainControlResponses() {
        device::LocalControlResponseSource source = response_source_.load(std::memory_order_acquire);
        void* context = command_context_.load(std::memory_order_acquire);
        if (source == nullptr || context == nullptr) {
            return;
        }
        std::array<char, kLocalControlResponseCapacity> response{};
        while (source(context, response.data(), response.size())) {
            const size_t length = ::strnlen(response.data(), response.size());
            if (length == 0U || length == response.size()) {
                continue;
            }
            esp_log_impl_lock();
            (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000U));
            const char newline = '\n';
            const bool written = UsbWriteAll(&newline, sizeof(newline)) && UsbWriteAll(response.data(), length) &&
                                 UsbWriteAll(&newline, sizeof(newline));
            (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000U));
            esp_log_impl_unlock();
            if (!written) {
                ESP_LOGW(kTag, "local control response write failed");
                return;
            }
            response = {};
        }
    }

    void CaptureAndTransmit() {
        const uint32_t stride = width_ * 3U;
        const size_t snapshot_bytes = static_cast<size_t>(stride) * height_;
        auto* pixels =
            static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, snapshot_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pixels == nullptr) {
            ESP_LOGE(kTag, "screen capture could not allocate %zu PSRAM bytes", snapshot_bytes);
            return;
        }

        lv_draw_buf_t snapshot{};
        bool captured = lv_draw_buf_init(&snapshot, width_, height_, LV_COLOR_FORMAT_RGB888, stride, pixels,
                                         snapshot_bytes) == LV_RESULT_OK;
        if (captured && esp_lv_adapter_lock(-1) == ESP_OK) {
            captured = lv_snapshot_take_to_draw_buf(lv_display_get_screen_active(display_), LV_COLOR_FORMAT_RGB888,
                                                    &snapshot) == LV_RESULT_OK;
            esp_lv_adapter_unlock();
        } else {
            captured = false;
        }

        if (!captured) {
            ESP_LOGE(kTag, "LVGL screen capture failed");
            heap_caps_free(pixels);
            return;
        }
        lv_draw_buf_invalidate_cache(&snapshot, nullptr);

        uint32_t sequence = ++sequence_;
        size_t png_bytes = 0U;
        bool transmitted = TransmitSnapshotPng(snapshot, sequence, png_bytes);
        heap_caps_free(pixels);
        if (transmitted) {
            ESP_LOGI(kTag, "screen capture #%" PRIu32 " complete: bytes=%zu", sequence, png_bytes);
        } else {
            ESP_LOGE(kTag, "screen capture #%" PRIu32 " PNG/USB transfer failed", sequence);
        }
    }

    lv_display_t* display_{};
    Gt911Input* touch_input_{};
    TaskHandle_t task_{};
    std::atomic<device::LocalControlCommandSink> command_sink_{};
    std::atomic<device::LocalControlResponseSource> response_source_{};
    std::atomic<void*> command_context_{};
    char command_[4608]{};
    size_t command_length_{};
    uint32_t sequence_{};
    uint32_t width_{};
    uint32_t height_{};
};

ScreenCapture& Instance() {
    static ScreenCapture instance;
    return instance;
}

}  // namespace

esp_err_t InitializeScreenCapture(lv_display_t* display, Gt911Input& touch_input, uint32_t width, uint32_t height) {
    return Instance().Start(display, touch_input, width, height);
}

device::LocalControlBackend& UsbLocalControlBackend() { return Instance(); }

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
