#include "platform/platform.hpp"

#include <cstdint>
#include <cstring>
#include <optional>

#include "draw/lv_draw_buf_private.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "lvgl.h"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/audio/es8311_i2s_audio_sink.hpp"
#include "platform/boards/ksdiy-p4c5/board_config.hpp"
#include "platform/boards/ksdiy-p4c5/platform_state.hpp"
#include "platform/boards/ksdiy-p4c5/presentation.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/graphics/dma2d_copy_engine.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/memory/ext_ram_bss.hpp"
#include "platform/memory/graphics_buffer_alignment.hpp"
#include "platform/memory/internal_ram.hpp"
#include "platform/wifi/esp_hosted_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "work/background_executor.hpp"
#include "work/task_policy.hpp"

#if !defined(CONFIG_PM_ENABLE)
#error "KSDIY P4C5 LVGL idle pause requires CONFIG_PM_ENABLE"
#endif

namespace micropixel::platform {
namespace {

namespace board_detail = ksdiy_p4c5::detail;
namespace board = ksdiy_p4c5::board;

void* AllocateImageBuffer(size_t size, lv_color_format_t) {
    // DMA cache invalidation must not overlap a neighbouring allocation.
    constexpr size_t kAlignment = memory::kGraphicsBufferAlignment;
    if (size > SIZE_MAX - (kAlignment - 1U)) {
        return nullptr;
    }
    const size_t allocation_bytes = (size + kAlignment - 1U) / kAlignment * kAlignment;
    return heap_caps_aligned_alloc(kAlignment, allocation_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void FreeImageBuffer(void* buffer) { heap_caps_free(buffer); }

graphics::Dma2dCopyEngine& LvglBufferCopyEngine() {
    // LVGL's draw-buffer copy callback has no context parameter, so this
    // engine is process-lifetime state driven only from the LVGL task.
    static graphics::Dma2dCopyEngine engine{};
    return engine;
}

void CopyImageBuffer(lv_draw_buf_t* destination, const lv_area_t* destination_area, const lv_draw_buf_t* source,
                     const lv_area_t* source_area) {
    if (destination == nullptr || source == nullptr || destination->header.cf != source->header.cf) {
        return;
    }
    const int32_t line_width =
        destination_area == nullptr ? destination->header.w : lv_area_get_width(destination_area);
    const int32_t source_width = source_area == nullptr ? source->header.w : lv_area_get_width(source_area);
    const int32_t line_count =
        destination_area == nullptr ? destination->header.h : lv_area_get_height(destination_area);
    const int32_t source_line_count = source_area == nullptr ? source->header.h : lv_area_get_height(source_area);
    if (line_width <= 0 || line_count <= 0 || line_width != source_width || line_count != source_line_count) {
        return;
    }

    if ((destination_area == nullptr || source_area == nullptr) && LV_COLOR_FORMAT_IS_INDEXED(destination->header.cf)) {
        std::memcpy(destination->data, source->data,
                    LV_COLOR_INDEXED_PALETTE_SIZE(destination->header.cf) * sizeof(lv_color32_t));
    }

    auto* destination_data =
        static_cast<uint8_t*>(lv_draw_buf_goto_xy(destination, destination_area == nullptr ? 0 : destination_area->x1,
                                                  destination_area == nullptr ? 0 : destination_area->y1));
    const auto* source_data = static_cast<const uint8_t*>(lv_draw_buf_goto_xy(
        source, source_area == nullptr ? 0 : source_area->x1, source_area == nullptr ? 0 : source_area->y1));
    if (destination_data == nullptr || source_data == nullptr) {
        return;
    }

    graphics::Dma2dCopyEngine& dma2d = LvglBufferCopyEngine();
    const uint32_t bits_per_pixel = lv_color_format_get_bpp(static_cast<lv_color_format_t>(destination->header.cf));
    if (dma2d.Ready() && bits_per_pixel == 24U && destination->data != source->data &&
        destination->header.stride % 3U == 0U && source->header.stride % 3U == 0U) {
        // Panel framebuffers handed to LVGL may not carry a data_size; the
        // tight stride * height bound is exact for them.
        const auto buffer_bytes = [](const lv_draw_buf_t* buffer) {
            return buffer->data_size != 0U ? buffer->data_size : buffer->header.stride * buffer->header.h;
        };
        const graphics::Dma2dCopyBlock copy{
            .source =
                {
                    .pixels = source->data,
                    .size = buffer_bytes(source),
                    .width = source->header.w,
                    .height = source->header.h,
                    .stride = source->header.stride,
                    .format = graphics::SurfacePixelFormat::kBgr888,
                },
            .source_rect = {.x = source_area == nullptr ? 0 : source_area->x1,
                            .y = source_area == nullptr ? 0 : source_area->y1,
                            .width = line_width,
                            .height = line_count},
            .destination =
                {
                    .pixels = destination->data,
                    .size = buffer_bytes(destination),
                    .width = destination->header.w,
                    .height = destination->header.h,
                    .stride = destination->header.stride,
                    .format = graphics::SurfacePixelFormat::kBgr888,
                },
            .destination_rect = {.x = destination_area == nullptr ? 0 : destination_area->x1,
                                 .y = destination_area == nullptr ? 0 : destination_area->y1,
                                 .width = line_width,
                                 .height = line_count},
        };
        if (dma2d.Copy(copy)) {
            return;
        }
    }

    const uint32_t line_bytes = (static_cast<uint32_t>(line_width) * bits_per_pixel + 7U) >> 3U;
    for (int32_t line = 0; line < line_count; ++line) {
        std::memcpy(destination_data, source_data, line_bytes);
        destination_data += destination->header.stride;
        source_data += source->header.stride;
    }
}

void* AlignImageBuffer(void* buffer, lv_color_format_t) { return buffer; }

uint32_t ImageWidthToStride(uint32_t width, lv_color_format_t color_format) {
    return LV_DRAW_BUF_STRIDE(width, color_format);
}

esp_err_t InitializeLvgl(board_detail::KsdiyP4c5BoardState& state) {
    esp_lv_adapter_config_t adapter_config{};
    adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_config.stack_in_psram = false;
    adapter_config.task_priority = task_policy::kDisplayPriority;
    adapter_config.task_core_id = board_detail::kLvglTaskCore;
    adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_MONOTONIC;
    // A sub-tick timeout rounds down to zero and can busy-loop this
    // high-priority task until the task watchdog fires.
    adapter_config.task_min_delay_ms = board_detail::kLvglTaskMinDelayMs;
    adapter_config.task_max_delay_ms = board_detail::kLvglMaximumWaitMs;
    adapter_config.auto_sleep.enable = true;
    adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_PAUSE;
    adapter_config.auto_sleep.idle_timeout_ms = board_detail::kLvglIdleTimeoutMs;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), board_detail::kTag, "initialize LVGL adapter failed");
    const esp_err_t copy_status = LvglBufferCopyEngine().Initialize();
    if (copy_status != ESP_OK) {
        ESP_LOGW(board_detail::kTag, "LVGL draw-buffer DMA2D copy unavailable; using CPU fallback: %s",
                 esp_err_to_name(copy_status));
    }
    lv_draw_buf_handlers_init(lv_draw_buf_get_image_handlers(), AllocateImageBuffer, FreeImageBuffer, CopyImageBuffer,
                              AlignImageBuffer, nullptr, nullptr, ImageWidthToStride);
    // Cover Flow scales and dims whole Hall cards, which LVGL renders through a
    // full-size ARGB8888 layer per card (~288 KiB). The 1 MiB LVGL pool cannot
    // reliably supply that contiguously, so layers come from PSRAM as well.
    lv_draw_buf_handlers_init(lv_draw_buf_get_handlers(), AllocateImageBuffer, FreeImageBuffer, CopyImageBuffer,
                              AlignImageBuffer, nullptr, nullptr, ImageWidthToStride);
    lv_draw_buf_get_handlers()->buf_copy_cb = CopyImageBuffer;

    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.display_pipeline.Panel();
    // MIPI-DSI needs no DBI IO handle; the BSP keeps it private.
    display_config.panel_io = nullptr;
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI;
    display_config.profile.rotation = board_detail::kRotation;
    // The adapter takes the physical panel size and swaps it for rotation.
    display_config.profile.hor_res = board::kPanelWidth;
    display_config.profile.ver_res = board::kPanelHeight;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#ifdef CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.enable_ppa_accel = true;
    display_config.profile.require_double_buffer = true;
    display_config.tear_avoid_mode = board_detail::kTearAvoidMode;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    state.display_pipeline.BindLvgl(state.display);
    ESP_RETURN_ON_ERROR(state.guest_graphics.Initialize(state.display, state.display_pipeline.DirectFramebuffers(),
                                                        state.display_pipeline.DirectScanout()),
                        board_detail::kTag, "initialize shared Guest graphics failed");
    lv_timer_set_period(lv_display_get_refr_timer(state.display), board_detail::kRefreshPeriodMs);

    // The adapter task has not started yet, so the UI is built without the lock.
    ESP_RETURN_ON_ERROR(state.ui.InitializeLocked(state.display), board_detail::kTag, "initialize System UI failed");
    ESP_RETURN_ON_ERROR(state.touch_input.Start(state.display), board_detail::kTag, "start ST7123 touch failed");
    lvgl::RequestDisplayRefresh(state.display);
    return esp_lv_adapter_start();
}

audio::I2sCodecAudioConfig AudioConfig() {
    return {
        .name = "KSDIY P4C5 ES8311/I2S",
        .log_tag = "ksdiy_audio",
        .i2c_port = board::kI2cPort,
        .i2s_port = board::kAudioI2sPort,
        .master_clock = board::kAudioMasterClock,
        .bit_clock = board::kAudioBitClock,
        .word_select = board::kAudioWordSelect,
        .data_out = board::kAudioDataOut,
        .amplifier_enable = board::kAudioAmplifierEnable,
        .codec_i2c_address = board::kAudioCodecI2cAddress,
        .sample_rate = board::kAudioSampleRate,
        .i2c_clock_hz = 100000U,
        .amplifier_preroll_ms = 24U,
        .dma_descriptor_count = 6U,
        .dma_frame_count = 240U,
        .amplifier_active_low = false,
        // The codec rails come up with the PMIC at boot.
        .probe_before_attach = true,
    };
}

class KsdiyP4c5Board final : public Board {
   public:
    KsdiyP4c5Board()
        : state_(TaskState(i2c_executor_, touch_input_)),
          graphics_context_{
              .engine = &state_.guest_graphics,
              .hooks = state_.ui.GraphicsHooks(),
          },
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          presentation_(state_),
          system_ui_(state_.ui, presentation_) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        ESP_RETURN_ON_FALSE(memory::IsInternalObject(*this), ESP_ERR_INVALID_STATE, board_detail::kTag,
                            "Board control objects must reside in internal RAM");
        state_.audio_engine = &context.AudioEngine();
        ESP_LOGI(board_detail::kTag, "initializing KSDIY P4C5: %dx%d %s", board_detail::kWidth, board_detail::kHeight,
                 board_detail::kTearAvoidModeName);

        ESP_RETURN_ON_ERROR(state_.board_io.Initialize(), board_detail::kTag,
                            "initialize KSDIY P4C5 BSP panel and touch failed");
        ESP_RETURN_ON_ERROR(i2c_executor_.Initialize(), board_detail::kTag, "start shared I2C executor failed");
        ESP_RETURN_ON_ERROR(touch_input_.Initialize(state_.board_io.Touch(), i2c_executor_, true), board_detail::kTag,
                            "bind ST7123 input failed");
        if (state_.board_io.PmicAvailable()) {
            state_.battery.Initialize(state_.board_io.Pmic(), i2c_executor_);
        }
        state_.sensors.Initialize(state_.board_io.I2cBus(), i2c_executor_);
        ESP_RETURN_ON_ERROR(InitializeLvgl(state_), board_detail::kTag, "initialize LVGL display pipeline failed");
        ESP_RETURN_ON_ERROR(state_.display_pipeline.SetBrightness(controllers::kBrightnessControlScale),
                            board_detail::kTag, "turn backlight on failed");

        const esp_err_t audio_status = audio_output_.Configure(state_.board_io.I2cBus(), i2c_executor_);
        if (audio_status != ESP_OK) {
            ESP_LOGW(board_detail::kTag, "ES8311 audio unavailable: %s", esp_err_to_name(audio_status));
        }
        // USB development screenshots take the same path as Remote Control.
        ESP_RETURN_ON_ERROR(state_.development_display.Start(touch_input_, state_.local_control, board_detail::kWidth,
                                                             board_detail::kHeight,
                                                             transports::DevelopmentCaptureHook::For(presentation_)),
                            board_detail::kTag, "start USB screen capture/local control failed");

        BoardRegistration& registration = registration_.emplace(device::BoardInfo{
            .board = "KSDIY ESP32-P4C5 4.3\"",
            .host_chip = "ESP32-P4",
            .firmware_target = board::kLandscape ? "ksdiy-p4c5-landscape" : "ksdiy-p4c5",
            .wifi_coprocessor = "ESP32-C5",
            .touch_controller = "ST7123",
            .display =
                {
                    .driver = "ST7102",
                    .interface = "MIPI-DSI / 2 lanes",
                    .pixel_format = "RGB888",
                    .width_pixels = static_cast<uint32_t>(board_detail::kWidth),
                    .height_pixels = static_cast<uint32_t>(board_detail::kHeight),
                    .refresh_rate_hz = board::kDisplayRefreshRateHz,
                },
            .graphics_acceleration = "PPA + DMA2D",
        });
        registration.SetGraphics(graphics_);
        registration.SetInput(state_.ui.Input());
        if (audio_status == ESP_OK) {
            registration.SetAudioOutput(audio_output_, audio_output_.SampleRate());
        }
        if (state_.board_io.PmicAvailable()) {
            registration.SetBattery(state_.battery);
        }
        bool registered = true;
        if (state_.sensors.acceleration_available()) {
            registered = registration.AddSensor(state_.sensors, ksdiy_p4c5::SensorPeripheral::kAcceleration,
                                                "Built-in LSM6DS3TR-C accelerometer") &&
                         registered;
        }
        if (state_.sensors.angular_velocity_available()) {
            registered = registration.AddSensor(state_.sensors, ksdiy_p4c5::SensorPeripheral::kAngularVelocity,
                                                "Built-in LSM6DS3TR-C gyroscope") &&
                         registered;
        }
        registration.SetWifi(wifi_);
        registration.SetLocalControl(state_.local_control);
        registration.SetSystemUi(system_ui_);
        ESP_LOGI(board_detail::kTag, "KSDIY P4C5 ready: ST7102 %dx%d RGB888, ST7123 IRQ GPIO%d, audio=%s, battery=%s",
                 board_detail::kWidth, board_detail::kHeight, static_cast<int>(board::kTouchInterrupt),
                 audio_status == ESP_OK ? "ES8311" : "off", state_.board_io.PmicAvailable() ? "AXP2101" : "off");
        return registered && context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        state_.guest_graphics.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    static board_detail::KsdiyP4c5BoardState& TaskState(buses::I2cExecutor& executor, input::EspLcdTouchInput& touch) {
        static MICROPIXEL_EXT_RAM_BSS board_detail::KsdiyP4c5BoardState state(executor, touch);
        return state;
    }

    buses::I2cExecutor i2c_executor_{};
    input::EspLcdTouchInput touch_input_{board_detail::kWidth, board_detail::kHeight, device::kMaxTouchPoints};
    board_detail::KsdiyP4c5BoardState& state_;
    std::optional<BoardRegistration> registration_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    audio::Es8311I2sAudioSink audio_output_{AudioConfig(), {.amplifier_voltage = 5.0F, .codec_dac_voltage = 3.3F}};
    wifi::EspHostedRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
    board_detail::KsdiyP4c5Presentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
};

}  // namespace

Board& ConfiguredBoard() {
    static KsdiyP4c5Board board;
    return board;
}

}  // namespace micropixel::platform
