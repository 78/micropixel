#include "platform/platform.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst92xx.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/esp-mosaico/battery_peripheral.hpp"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "platform/boards/esp-mosaico/display/brightness_controller.hpp"
#include "platform/boards/esp-mosaico/display/panel_transition_compositor.hpp"
#include "platform/boards/esp-mosaico/function_button.hpp"
#include "platform/boards/esp-mosaico/haptic_actuator.hpp"
#include "platform/boards/esp-mosaico/i2s_audio_sink.hpp"
#include "platform/boards/esp-mosaico/platform_state.hpp"
#include "platform/boards/esp-mosaico/power_controller.hpp"
#include "platform/boards/esp-mosaico/presentation.hpp"
#include "platform/boards/esp-mosaico/sensor_peripheral.hpp"
#include "platform/boards/esp-mosaico/status_led.hpp"
#include "platform/boards/esp-mosaico/usb_cdc_console.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/gpio/esp_gpio_peripheral.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/lvgl/display/system_transition_timeline.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/host_pointer_router.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/random/system_random.hpp"
#include "platform/transports/tinyusb_cdc_local_control.hpp"
#include "platform/wifi/native_wifi_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "src/display/lv_display_private.h"
#include "work/background_executor.hpp"

namespace micropixel::platform {
namespace {

namespace board_detail = esp_mosaico::detail;

esp_err_t InitializeDisplay(board_detail::MosaicoBoardState& state) {
    ESP_RETURN_ON_ERROR(state.display_pipeline.InitializePanel(), board_detail::kTag,
                        "initialize Mosaico display pipeline failed");
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_config{};
        adapter_config.task_stack_size = 10240U;
        adapter_config.task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
        adapter_config.task_core_id = board_detail::kLvglTaskCore;
        adapter_config.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
#ifdef ESP_LV_ADAPTER_HAS_TICK_MODE
        adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_PERIODIC;
#endif
        adapter_config.task_min_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS;
        adapter_config.task_max_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS;
        adapter_config.stack_in_psram = true;
        adapter_config.auto_sleep.enable = false;
        adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
        adapter_config.auto_sleep.idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS;
        ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), board_detail::kTag, "initialize LVGL adapter failed");
    }
    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.display_pipeline.Panel();
    display_config.panel_io = state.display_pipeline.PanelIo();
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_config.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.profile.hor_res = board_detail::kWidth;
    display_config.profile.ver_res = board_detail::kHeight;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#ifdef CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.require_double_buffer = true;
    display_config.profile.enable_ppa_accel = board_detail::kEnableLvglAdapterPpaAccel;
    display_config.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(state.display_pipeline.BindLvgl(state.display), board_detail::kTag,
                        "bind Mosaico display pipeline to LVGL failed");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), board_detail::kTag, "start LVGL adapter failed");
    return ESP_OK;
}

esp_err_t InitializeSharedI2c(board_detail::MosaicoBoardState& state) {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.sda_io_num = esp_mosaico::board::kI2cData;
    bus_config.scl_io_num = esp_mosaico::board::kI2cClock;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &state.i2c_bus), board_detail::kTag,
                        "initialize shared Mosaico I2C bus failed");
    return ESP_OK;
}

esp_err_t InitializeTouch(board_detail::MosaicoBoardState& state) {
    if (state.i2c_bus == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST92XX_ADDRESS;
    io_config.scl_speed_hz = 400000U;
    io_config.control_phase_bytes = 1U;
    io_config.dc_bit_offset = 0U;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.disable_control_phase = true;
    io_config.transaction_timeout_ms = 100U;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(state.i2c_bus, &io_config, &state.touch_io), board_detail::kTag,
                        "create CST92xx panel IO failed");

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = board_detail::kWidth - 1;
    touch_config.y_max = board_detail::kHeight - 1;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = esp_mosaico::board::kTouchInterrupt;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst92xx(state.touch_io, &touch_config, &state.touch), board_detail::kTag,
                        "initialize CST92xx failed");
    ESP_LOGI(board_detail::kTag, "CST92xx ready: interrupt GPIO%d shared I2C0",
             static_cast<int>(esp_mosaico::board::kTouchInterrupt));
    return ESP_OK;
}

std::expected<void, device::PowerError> RestoreDisplayAfterSleep(board_detail::MosaicoBoardState& state) {
    const esp_err_t resume_status = state.display_pipeline.Resume();
    if (resume_status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "display resume failed after sleep: %s", esp_err_to_name(resume_status));
        return std::unexpected(device::PowerError::kDisplayRestore);
    }
    const esp_err_t adapter_status =
        esp_lv_adapter_sleep_recover(state.display, state.display_pipeline.Panel(), state.display_pipeline.PanelIo());
    if (adapter_status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "LVGL recovery failed after sleep: %s", esp_err_to_name(adapter_status));
        return std::unexpected(device::PowerError::kDisplayRestore);
    }
    return {};
}

std::expected<void, device::PowerError> EnterLowPowerImpl(board_detail::MosaicoBoardState& state,
                                                          esp_mosaico::PowerController& power) {
    esp_err_t status = esp_lv_adapter_sleep_prepare();
    if (status != ESP_OK) {
        ESP_LOGE(board_detail::kTag, "LVGL sleep preparation failed: %s", esp_err_to_name(status));
        return std::unexpected(device::PowerError::kDisplayPrepare);
    }
    status = state.display_pipeline.Suspend();
    if (status != ESP_OK) {
        (void)RestoreDisplayAfterSleep(state);
        return std::unexpected(device::PowerError::kDisplayShutdown);
    }

    const auto power_press_pending = [&power] { return power.PowerPressOccurredAfterRequest() || power.IsPressed(); };
    if (power_press_pending()) {
        ESP_LOGI(board_detail::kTag, "light sleep canceled by a concurrent POWER press");
        power.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
        return RestoreDisplayAfterSleep(state);
    }

    for (uint32_t attempt = 1U; attempt <= esp_mosaico::board::kLightSleepEntryAttempts; ++attempt) {
        status = power.PrepareForLightSleep();
        if (status != ESP_OK) {
            if (power_press_pending()) {
                power.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
                return RestoreDisplayAfterSleep(state);
            }
            const auto restore = RestoreDisplayAfterSleep(state);
            if (!restore.has_value()) {
                return restore;
            }
            return std::unexpected(device::PowerError::kWakeSource);
        }
        if (power_press_pending()) {
            power.FinishLightSleep();
            power.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
            return RestoreDisplayAfterSleep(state);
        }

        const int64_t started_us = esp_timer_get_time();
        status = esp_light_sleep_start();
        const uint64_t duration_us = static_cast<uint64_t>(esp_timer_get_time() - started_us);
        power.FinishLightSleep();
        if (status == ESP_OK) {
            ESP_LOGI(board_detail::kTag, "light sleep returned after %" PRIu64 " ms", duration_us / 1000U);
            power.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
            break;
        }
        ESP_LOGW(board_detail::kTag, "light sleep attempt %" PRIu32 " rejected: %s", attempt, esp_err_to_name(status));
        if (status != ESP_ERR_SLEEP_REJECT || attempt == esp_mosaico::board::kLightSleepEntryAttempts) {
            break;
        }
    }

    const auto restore = RestoreDisplayAfterSleep(state);
    if (!restore.has_value()) {
        return restore;
    }
    return status == ESP_OK ? std::expected<void, device::PowerError>{}
                            : std::unexpected(device::PowerError::kSleepRejected);
}

class EspMosaicoBoard final : public Board, public device::Power {
   public:
    EspMosaicoBoard()
        : graphics_context_{
              .engine = &state_.guest_graphics,
              .hooks = state_.ui.GraphicsHooks(),
          },
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          presentation_(state_),
          system_ui_(state_.ui, presentation_) {
        state_.ui.BindThemeChanged(
            [](void* context) {
                static_cast<board_detail::MosaicoBoardState*>(context)->panel_transition.ReleaseBackground();
            },
            &state_);
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_RETURN_ON_ERROR(InitializeUsbCdcConsole(), board_detail::kTag, "initialize Type-C USB CDC console failed");
        ESP_LOGI(board_detail::kTag, "initializing ESP-Mosaico with CO5300 and CST92xx drivers");
        ESP_RETURN_ON_ERROR(power_.Initialize(), board_detail::kTag, "initialize Mosaico power control failed");
        ESP_RETURN_ON_ERROR(status_led_.Initialize(), board_detail::kTag, "initialize Mosaico status LED failed");
        ESP_RETURN_ON_ERROR(status_led_.Set(true), board_detail::kTag, "turn on Mosaico startup status LED failed");
        ESP_RETURN_ON_ERROR(InitializeSharedI2c(state_), board_detail::kTag,
                            "initialize shared Mosaico I2C bus failed");
        ESP_RETURN_ON_ERROR(InitializeTouch(state_), board_detail::kTag, "initialize Mosaico touch failed");
        ESP_RETURN_ON_ERROR(state_.i2c_executor.Initialize(), board_detail::kTag,
                            "start shared Mosaico I2C executor failed");
        ESP_RETURN_ON_ERROR(state_.touch_input.Initialize(state_.touch, state_.i2c_executor), board_detail::kTag,
                            "bind CST92xx input failed");
        ESP_RETURN_ON_ERROR(sensors_.BeginInitialize(state_.i2c_bus, state_.i2c_executor), board_detail::kTag,
                            "start Mosaico sensor discovery failed");
        ESP_RETURN_ON_ERROR(InitializeDisplay(state_), board_detail::kTag, "initialize Mosaico display failed");
#if CONFIG_MICROPIXEL_MOSAICO_SOFTWARE_RENDERING
        ESP_LOGI(board_detail::kTag, "S31 graphics experiment: PPA/DMA2D disabled; hardware transitions unavailable");
#else
        ESP_RETURN_ON_ERROR(
            state_.panel_transition.Initialize(
                state_.display, board_detail::kWidth, board_detail::kHeight,
                host_ui::lvgl::square_common::TransitionProfile(board_detail::ui_profile::kSystemUiProfile.square),
                state_.display_pipeline.ShadowCopyDma2d(), state_.display_pipeline.DisplayedShadow(),
                state_.display_pipeline.DisplayedShadowReady()),
            board_detail::kTag, "initialize CO5300 direct transition compositor failed");
#endif
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, nullptr), board_detail::kTag,
                            "initialize shared Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        const esp_err_t host_pointer_status = state_.ui.InitializeLocked(state_.display);
        if (host_pointer_status != ESP_OK) {
            esp_lv_adapter_unlock();
            return host_pointer_status;
        }

        esp_lv_adapter_unlock();

        ESP_RETURN_ON_ERROR(sensors_.FinishInitialize(), board_detail::kTag, "finish Mosaico sensor discovery failed");
        ESP_RETURN_ON_ERROR(audio_output_.Configure(state_.i2c_bus, state_.i2c_executor), board_detail::kTag,
                            "configure Mosaico ES8311 audio hardware failed");
        battery_.Initialize(state_.i2c_bus, state_.i2c_executor);
        ESP_RETURN_ON_ERROR(haptics_.Initialize(), board_detail::kTag, "initialize Mosaico vibration motor failed");
        ESP_RETURN_ON_ERROR(gpio_.Initialize(), board_detail::kTag, "initialize Mosaico application GPIO failed");
        ESP_RETURN_ON_ERROR(function_button_.Initialize(state_.ui.Input()), board_detail::kTag,
                            "initialize Mosaico function button failed");
        ESP_RETURN_ON_ERROR(state_.touch_input.Start(state_.display), board_detail::kTag,
                            "start interrupt-driven touch failed");
        ESP_RETURN_ON_ERROR(state_.development_display.Start(state_.display, state_.touch_input, state_.local_control,
                                                             board_detail::kWidth, board_detail::kHeight,
                                                             {.pixels = state_.display_pipeline.DisplayedShadow(),
                                                              .stride = board_detail::kWidth * 2U,
                                                              .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                                              .ready = state_.display_pipeline.DisplayedShadowReady()}),
                            board_detail::kTag, "start USB screen capture/local control failed");
        ESP_LOGI(board_detail::kTag,
                 "Mosaico HMI ready: 480x480 QSPI display, interrupt-driven touch, shared Guest renderer");
        BoardRegistration registration{{
            .board = "ESP-Mosaico V1.0",
            .host_chip = "ESP32-S31",
            .wifi_coprocessor = "Native ESP32-S31",
            .touch_controller = "CST92xx",
            .display =
                {
                    .driver = "CO5300",
                    .interface = "QSPI",
                    .pixel_format = "RGB565",
                    .width_pixels = static_cast<uint32_t>(board_detail::kWidth),
                    .height_pixels = static_cast<uint32_t>(board_detail::kHeight),
                    .refresh_rate_hz = 60U,
                    // H0216F002AMT004-1 does not specify a simple circular
                    // corner radius. Its mechanical drawing defines a rounded
                    // Active Area whose largest axis-aligned safe rectangle is
                    // inset by about 22 px. Keep a 2 px assembly/rounding guard.
                    .safe_area =
                        {
                            .top_pixels = 24U,
                            .right_pixels = 24U,
                            .bottom_pixels = 24U,
                            .left_pixels = 24U,
                        },
                },
#if CONFIG_MICROPIXEL_MOSAICO_SOFTWARE_RENDERING
            .graphics_acceleration = "CPU only",
#else
            .graphics_acceleration = "PPA + DMA2D",
#endif
        }};
        registration.SetGraphics(graphics_);
        registration.SetInput(state_.ui.Input());
        registration.SetAudioOutput(audio_output_, 16000U, &power_);
        registration.SetBattery(battery_);
        registration.SetWifi(wifi_);
        registration.SetPower(*this);
        registration.SetLocalControl(state_.local_control);
        registration.SetSystemUi(system_ui_);
        bool registered = true;
        if (sensors_.acceleration_available()) {
            registered = registration.AddSensor(sensors_, esp_mosaico::SensorPeripheral::kAcceleration,
                                                "BMI270 accelerometer") &&
                         registered;
        }
        if (sensors_.angular_velocity_available()) {
            registered =
                registration.AddSensor(sensors_, esp_mosaico::SensorPeripheral::kAngularVelocity, "BMI270 gyroscope") &&
                registered;
        }
        if (sensors_.magnetic_field2_available()) {
            registered = registration.AddSensor(sensors_, esp_mosaico::SensorPeripheral::kMagneticField2,
                                                "BMM150 magnetometer #2") &&
                         registered;
        }
        if (sensors_.magnetic_field3_available()) {
            registered = registration.AddSensor(sensors_, esp_mosaico::SensorPeripheral::kMagneticField3,
                                                "BMM150 magnetometer #3") &&
                         registered;
        }
        registered =
            registration.AddGpio(status_led_, esp_mosaico::StatusLed::kChannel, "Orange status LED") && registered;
        for (device::PeripheralChannelId line : esp_mosaico::board::kApplicationGpioLines) {
            char name[16]{};
            (void)std::snprintf(name, sizeof(name), "GPIO%u", static_cast<unsigned>(line));
            registered = registration.AddGpio(gpio_, line, name) && registered;
        }
        registered =
            registration.AddHaptics(haptics_, haptics::TimedHapticsPeripheral::kChannel, "Built-in vibration motor") &&
            registered;
        if (!registered || !context.Publish(registration)) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_RETURN_ON_ERROR(status_led_.Set(false), board_detail::kTag, "turn off Mosaico startup status LED failed");
        return ESP_OK;
    }
    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

    void SetPowerButtonSink(device::PowerButtonSink sink, void* context) override {
        power_.SetPowerButtonSink(sink, context);
    }

    void SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context) override {
        power_.SetPowerOffButtonSink(sink, context);
    }

    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return EnterLowPowerImpl(state_, power_);
    }

    [[noreturn]] void PowerOff() override { power_.PowerOff(); }

   private:
    board_detail::MosaicoBoardState state_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    esp_mosaico::I2sAudioSink audio_output_{};
    board_detail::MosaicoPresentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
    esp_mosaico::BatteryPeripheral battery_{};
    esp_mosaico::SensorPeripheral sensors_{};
    gpio::EspGpioPeripheral gpio_{esp_mosaico::board::kApplicationGpioLines};
    esp_mosaico::FunctionButton function_button_{};
    esp_mosaico::StatusLed status_led_{};
    wifi::NativeWifiRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
    esp_mosaico::PowerController power_{};
    haptics::TimedHapticsPeripheral haptics_{esp_mosaico::ConfiguredHapticActuator(), 0U};
};

}  // namespace

Board& ConfiguredBoard() {
    static EspMosaicoBoard board;
    return board;
}

}  // namespace micropixel::platform
