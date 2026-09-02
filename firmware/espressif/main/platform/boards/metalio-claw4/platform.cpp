#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "device/contracts/graphics.hpp"
#include "draw/lv_draw_buf_private.h"
#include "esp_async_color_convert.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#if CONFIG_MICROPIXEL_SYSTEM_SHELL_SNAPSHOT
#include "src/draw/snapshot/lv_snapshot.h"
#endif
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/metalio-claw4/battery_peripheral.hpp"
#include "platform/boards/metalio-claw4/board_config.hpp"
#include "platform/boards/metalio-claw4/board_io.hpp"
#include "platform/boards/metalio-claw4/display/display_pipeline.hpp"
#include "platform/boards/metalio-claw4/display/screen_capture.hpp"
#include "platform/boards/metalio-claw4/gpio_peripheral.hpp"
#include "platform/boards/metalio-claw4/haptic_actuator.hpp"
#include "platform/boards/metalio-claw4/i2s_audio_sink.hpp"
#include "platform/boards/metalio-claw4/platform_state.hpp"
#include "platform/boards/metalio-claw4/presentation.hpp"
#include "platform/boards/metalio-claw4/sensor_peripheral.hpp"
#include "platform/boards/metalio-claw4/tca9555_power_key.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/input/gt911_input.hpp"
#include "platform/lvgl/display/jpeg_cover_decoder.hpp"
#include "platform/lvgl/display/png_cover_decoder.hpp"
#include "platform/lvgl/display/system_transition_compositor.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/host_pointer_router.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/platform.hpp"
#include "platform/random/system_random.hpp"
#include "platform/wifi/esp_hosted_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "work/background_executor.hpp"
#include "work/task_policy.hpp"

#if !defined(CONFIG_PM_ENABLE)
#error "Metalio-Claw4 LVGL idle pause requires CONFIG_PM_ENABLE"
#endif

namespace micropixel::platform {
namespace {

namespace board_detail = metalio_claw4::detail;

void* AllocateImageBuffer(size_t size, lv_color_format_t) {
    return heap_caps_aligned_alloc(64U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void FreeImageBuffer(void* buffer) { heap_caps_free(buffer); }

async_color_convert_handle_t& LvglBufferCopyDma2dClient() {
    // LVGL's draw-buffer copy callback has no context parameter. This client is
    // therefore process-lifetime state, just like LVGL's global handlers.
    static async_color_convert_handle_t client{};
    return client;
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

    async_color_convert_handle_t dma2d = LvglBufferCopyDma2dClient();
    const uint32_t bits_per_pixel = lv_color_format_get_bpp(static_cast<lv_color_format_t>(destination->header.cf));
    if (dma2d != nullptr && bits_per_pixel == 24U && destination->data != source->data &&
        destination->header.stride % 3U == 0U && source->header.stride % 3U == 0U) {
        async_color_convert_request_t copy{};
        copy.src_buffer = source->data;
        copy.src_stride = source->header.stride / 3U;
        copy.src_height = source->header.h;
        copy.src_x = source_area == nullptr ? 0U : static_cast<uint32_t>(source_area->x1);
        copy.src_y = source_area == nullptr ? 0U : static_cast<uint32_t>(source_area->y1);
        copy.dst_buffer = destination->data;
        copy.dst_stride = destination->header.stride / 3U;
        copy.dst_height = destination->header.h;
        copy.dst_x = destination_area == nullptr ? 0U : static_cast<uint32_t>(destination_area->x1);
        copy.dst_y = destination_area == nullptr ? 0U : static_cast<uint32_t>(destination_area->y1);
        copy.copy_width = static_cast<uint32_t>(line_width);
        copy.copy_height = static_cast<uint32_t>(line_count);
        copy.src_color_format = ESP_COLOR_FOURCC_BGR24;
        copy.dst_color_format = ESP_COLOR_FOURCC_BGR24;
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY
        const int64_t copy_started_us = esp_timer_get_time();
#endif
        if (esp_color_convert_blocking(dma2d, &copy, -1) == ESP_OK) {
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY
            esp_lv_adapter_display_telemetry_record_framebuffer_dma2d(
                static_cast<uint64_t>(line_width) * line_count,
                static_cast<uint64_t>(esp_timer_get_time() - copy_started_us));
#endif
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

constexpr const char* GraphicsRendererName() { return "retained-objects"; }

esp_err_t InitializeLvgl(board_detail::MetalioClaw4BoardState& state) {
    esp_lv_adapter_config_t adapter_config{};
    adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_config.stack_in_psram = false;
    adapter_config.task_priority = task_policy::kDisplayPriority;
    adapter_config.task_core_id = board_detail::kLvglTaskCore;
    adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_MONOTONIC;
    // A sub-tick timeout is rounded down to zero by pdMS_TO_TICKS().  LVGL's
    // animation timer can then keep this high-priority task in a busy loop and
    // starve IDLE1 until the task watchdog fires.
    adapter_config.task_min_delay_ms = board_detail::kLvglTaskMinDelayMs;
    adapter_config.task_max_delay_ms = board_detail::kLvglMaximumWaitMs;
    adapter_config.auto_sleep.enable = true;
    adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_PAUSE;
    adapter_config.auto_sleep.idle_timeout_ms = board_detail::kLvglIdleTimeoutMs;
    esp_err_t status = esp_lv_adapter_init(&adapter_config);
    if (status != ESP_OK) {
        return status;
    }
    async_color_convert_config_t copy_config{};
    copy_config.backlog = 1U;
    copy_config.dma_burst_size = 128U;
    status = esp_async_color_convert_install_dma2d(&copy_config, &LvglBufferCopyDma2dClient());
    if (status != ESP_OK) {
        LvglBufferCopyDma2dClient() = nullptr;
        ESP_LOGW(board_detail::kTag, "LVGL draw-buffer DMA2D copy unavailable; using CPU fallback: %s",
                 esp_err_to_name(status));
    }
    lv_draw_buf_handlers_init(lv_draw_buf_get_image_handlers(), AllocateImageBuffer, FreeImageBuffer, CopyImageBuffer,
                              AlignImageBuffer, nullptr, nullptr, ImageWidthToStride);
    lv_draw_buf_get_handlers()->buf_copy_cb = CopyImageBuffer;

    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.display_pipeline.Panel();
    display_config.panel_io = state.display_pipeline.PanelIo();
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI;
    display_config.profile.hor_res = board_detail::kWidth;
    display_config.profile.ver_res = board_detail::kHeight;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#ifdef CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.enable_ppa_accel = board_detail::kEnablePpaAccel;
    display_config.profile.require_double_buffer = true;
    display_config.tear_avoid_mode = board_detail::kTearAvoidMode;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    state.display_pipeline.BindLvgl(state.display);
    status = state.guest_graphics.Initialize(state.display, state.display_pipeline.DirectFramebuffers());
    if (status != ESP_OK) {
        return status;
    }
    status = state.system_transition.Initialize(
        state.display, state.display_pipeline.DirectFramebuffers(), board_detail::kWidth, board_detail::kHeight,
        host_ui::lvgl::square_common::TransitionProfile(board_detail::ui_profile::kSystemUiProfile.square));
    if (status != ESP_OK) {
        return status;
    }
    lv_timer_set_period(lv_display_get_refr_timer(state.display), board_detail::kRefreshPeriodMs);

    status = state.ui.InitializeLocked(state.display);
    if (status != ESP_OK) {
        return status;
    }

    status = state.touch_input.Start(state.display);
    if (status != ESP_OK) {
        return status;
    }
    lvgl::RequestDisplayRefresh(state.display);
    status = esp_lv_adapter_start();
    if (status == ESP_OK) {
        status = metalio_claw4::InitializeScreenCapture(state.display, state.touch_input, board_detail::kWidth,
                                                        board_detail::kHeight);
    }
    return status;
}

}  // namespace

namespace {

esp_err_t InitializePlatformImpl(board_detail::MetalioClaw4BoardState& state) {
    ESP_LOGI(board_detail::kTag, "initializing Metalio-Claw4 LVGL display pipeline");
    esp_err_t status = state.board_io.Initialize();
    if (status == ESP_OK) {
        status = state.i2c_executor.Initialize();
    }
    if (status == ESP_OK) {
        state.battery.Initialize(state.board_io.I2cBus(), state.board_io.IoExpander(), state.i2c_executor);
        state.power_key.SetPowerInputChangeSink(
            [](void* context) {
                static_cast<metalio_claw4::BatteryPeripheral*>(context)->NotifyExternalPowerChanged();
            },
            &state.battery);
        status = state.power_key.Initialize(state.board_io.IoExpander(), state.i2c_executor);
    }
    if (status == ESP_OK) {
        state.sensors.Initialize(state.board_io.I2cBus(), state.i2c_executor);
        status = state.gpio.Initialize();
    }
    if (status == ESP_OK) {
        status = state.haptics.Initialize();
    }
    if (status == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        status = state.touch_input.Initialize(state.board_io.I2cBus(), state.i2c_executor);
    }
    if (status == ESP_OK) {
        status = InitializeLvgl(state);
    }
    if (status == ESP_OK) {
        status = state.display_pipeline.SetBrightness(controllers::kBrightnessControlScale);
    }
    if (status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "Metalio-Claw4 LVGL display pipeline failed: %s", esp_err_to_name(status));
        return status;
    }

    ESP_LOGI(board_detail::kTag,
             "Metalio-Claw4 LVGL display pipeline ready: %dx%d RGB888, %s, %lu ms idle refresh, "
             "GT911 IRQ GPIO%d, power-key=TCA9555-P0.5/IRQ-GPIO%d, graphics=%s, ppa=%s, lvgl-core=%d priority=%u",
             board_detail::kWidth, board_detail::kHeight, board_detail::kTearAvoidModeName,
             static_cast<unsigned long>(board_detail::kRefreshPeriodMs),
             static_cast<int>(metalio_claw4::board::kTouchInterrupt),
             static_cast<int>(metalio_claw4::board::kIoExpanderInterrupt), GraphicsRendererName(),
             board_detail::kEnablePpaAccel ? "on" : "off", board_detail::kLvglTaskCore,
             static_cast<unsigned>(task_policy::kDisplayPriority));
    return ESP_OK;
}

std::expected<void, device::PowerError> RestoreDisplayAfterSleep(board_detail::MetalioClaw4BoardState& state) {
    const esp_err_t resume_status = state.display_pipeline.Resume();
    if (resume_status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "display reinitialization failed after sleep: %s", esp_err_to_name(resume_status));
        return std::unexpected(device::PowerError::kDisplayRestore);
    }

    state.guest_graphics.RebindFramebuffers(state.display_pipeline.DirectFramebuffers());
    state.system_transition.RebindFramebuffers(state.display_pipeline.DirectFramebuffers());
    const esp_err_t status =
        esp_lv_adapter_sleep_recover(state.display, state.board_io.Panel(), state.board_io.PanelIo());
    if (status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "LVGL display recovery failed after sleep: %s", esp_err_to_name(status));
        return std::unexpected(device::PowerError::kDisplayRestore);
    }
    return {};
}

std::expected<void, device::PowerError> EnterLowPowerImpl(board_detail::MetalioClaw4BoardState& state) {
    esp_err_t status = esp_lv_adapter_sleep_prepare();
    if (status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "LVGL sleep preparation failed: %s", esp_err_to_name(status));
        return std::unexpected(device::PowerError::kDisplayPrepare);
    }

    status = state.display_pipeline.Suspend();
    if (status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "display shutdown failed: %s", esp_err_to_name(status));
        (void)RestoreDisplayAfterSleep(state);
        return std::unexpected(device::PowerError::kDisplayShutdown);
    }

    const auto power_press_pending = [&state] {
        return state.power_key.PowerPressOccurredAfterRequest() || state.power_key.IsPressed();
    };
    const auto cancel_sleep_entry = [&state]() -> std::expected<void, device::PowerError> {
        ESP_LOGI(board_detail::kTag, "light sleep canceled by a power press during the entry transition");
        state.power_key.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
        return RestoreDisplayAfterSleep(state);
    };

    if (power_press_pending()) {
        return cancel_sleep_entry();
    }

    for (uint32_t attempt = 1U; attempt <= board_detail::kLightSleepEntryAttempts; ++attempt) {
        status = state.power_key.PrepareForLightSleep();
        if (status != ESP_OK) {
            if (power_press_pending()) {
                return cancel_sleep_entry();
            }
            ESP_LOGE(board_detail::kTag, "power-key wake source preparation failed: %s", esp_err_to_name(status));
            const auto restore = RestoreDisplayAfterSleep(state);
            if (!restore.has_value()) {
                return restore;
            }
            return std::unexpected(device::PowerError::kWakeSource);
        }
        if (power_press_pending()) {
            return cancel_sleep_entry();
        }

        ESP_LOGI(board_detail::kTag, "entering light sleep; power key is the wake source (attempt=%" PRIu32 ")",
                 attempt);
        const int64_t sleep_started_us = esp_timer_get_time();
        status = esp_light_sleep_start();
        const uint64_t sleep_duration_us = static_cast<uint64_t>(esp_timer_get_time() - sleep_started_us);
        if (status == ESP_OK) {
            const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
            const uint64_t gpio_wakeup_status = esp_sleep_get_gpio_wakeup_status();
            ESP_LOGI(board_detail::kTag,
                     "light sleep returned: status=ESP_OK duration=%" PRIu64 " ms causes=0x%08" PRIx32
                     " gpio=0x%016" PRIx64,
                     sleep_duration_us / 1000U, wake_causes, gpio_wakeup_status);
            state.power_key.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
            break;
        }

        ESP_LOGW(board_detail::kTag,
                 "light sleep entry rejected: attempt=%" PRIu32 " status=%s duration=%" PRIu64 " ms", attempt,
                 status == ESP_ERR_SLEEP_REJECT ? "ESP_ERR_SLEEP_REJECT" : esp_err_to_name(status),
                 sleep_duration_us / 1000U);
        if (power_press_pending()) {
            return cancel_sleep_entry();
        }
        if (status != ESP_ERR_SLEEP_REJECT || attempt == board_detail::kLightSleepEntryAttempts) {
            break;
        }
        ESP_LOGI(board_detail::kTag, "retrying light sleep after clearing a transient wake condition");
    }

    const auto restore = RestoreDisplayAfterSleep(state);
    if (!restore.has_value()) {
        return restore;
    }
    if (status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "light sleep rejected: %s", esp_err_to_name(status));
        return std::unexpected(device::PowerError::kSleepRejected);
    }
    ESP_LOGI(board_detail::kTag, "light sleep cycle complete");
    return {};
}

class MetalioClaw4Board final : public Board, public device::Power {
   public:
    MetalioClaw4Board()
        : graphics_context_{
              .engine = &state_.guest_graphics,
              .hooks = state_.ui.GraphicsHooks(),
          },
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          presentation_(state_),
          system_ui_(state_.ui, presentation_) {
        state_.ui.BindThemeChanged(
            [](void* context) {
                static_cast<board_detail::MetalioClaw4BoardState*>(context)->system_transition.ClearBackground();
            },
            &state_);
        state_.ui.BindBeforeLaunchPresentation(
            [](void* context) {
                static_cast<board_detail::MetalioClaw4BoardState*>(context)->touch_input.ClearSmokeUi();
            },
            &state_);
        state_.ui.BindBeforeRootRelease(
            [](void* context) {
                static_cast<board_detail::MetalioClaw4BoardState*>(context)->touch_input.ClearSmokeUi();
            },
            &state_);
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        state_.audio_engine = &context.AudioEngine();
        const esp_err_t initialize_error = InitializePlatformImpl(state_);
        if (initialize_error != ESP_OK) {
            return initialize_error;
        }
        audio_output_.Configure(state_.board_io.IoExpander(), state_.i2c_executor);
        BoardRegistration registration{{
            .board = "Metalio-Claw4",
            .host_chip = "ESP32-P4",
            .firmware_target = "metalio-claw4",
            .wifi_coprocessor = "ESP32-C5",
            .touch_controller = "GT911",
            .display =
                {
                    .driver = "NV3051F",
                    .interface = "MIPI-DSI / 2 lanes",
                    .pixel_format = "RGB888",
                    .width_pixels = static_cast<uint32_t>(board_detail::kWidth),
                    .height_pixels = static_cast<uint32_t>(board_detail::kHeight),
                    .refresh_rate_hz = 60U,
                },
            .graphics_acceleration = "PPA + DMA2D",
        }};
        registration.SetGraphics(graphics_);
        registration.SetInput(state_.ui.Input());
        registration.SetAudioOutput(audio_output_, 16000U);
        registration.SetBattery(state_.battery);
        registration.SetWifi(wifi_);
        registration.SetPower(*this);
        registration.SetLocalControl(metalio_claw4::UsbLocalControl());
        registration.SetSystemUi(system_ui_);
        bool registered = true;
        if (state_.sensors.acceleration_available()) {
            registered = registration.AddSensor(state_.sensors, metalio_claw4::SensorPeripheral::kAcceleration,
                                                "Built-in accelerometer") &&
                         registered;
        }
        if (state_.sensors.magnetic_field_available()) {
            registered = registration.AddSensor(state_.sensors, metalio_claw4::SensorPeripheral::kMagneticField,
                                                "Built-in magnetometer") &&
                         registered;
        }
        for (device::PeripheralChannelId line : metalio_claw4::GpioPeripheral::kApplicationLines) {
            char name[16]{};
            (void)std::snprintf(name, sizeof(name), "P%u", static_cast<unsigned>(line));
            registered = registration.AddGpio(state_.gpio, line, name) && registered;
        }
        registered = registration.AddHaptics(state_.haptics, haptics::TimedHapticsPeripheral::kChannel,
                                             "Built-in vibration motor") &&
                     registered;
        return registered && context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

    void SetPowerButtonSink(device::PowerButtonSink sink, void* context) override {
        state_.power_key.SetKeyPressSink(sink, context);
    }

    void SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context) override {
        state_.power_key.SetPowerOffSink(sink, context);
    }

    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override { return EnterLowPowerImpl(state_); }

    [[noreturn]] void PowerOff() override { state_.power_key.PowerOff(); }

   private:
    board_detail::MetalioClaw4BoardState state_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    metalio_claw4::I2sAudioSink audio_output_{};
    wifi::EspHostedRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
    board_detail::MetalioClaw4Presentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
};

}  // namespace

Board& ConfiguredBoard() {
    static MetalioClaw4Board board;
    return board;
}

}  // namespace micropixel::platform
