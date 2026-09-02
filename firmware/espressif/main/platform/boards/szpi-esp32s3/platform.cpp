#include "platform/platform.hpp"

#include <algorithm>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/esp32-s3-common/landscape_320_state.hpp"
#include "platform/boards/esp32-s3-common/lvgl_display.hpp"
#include "platform/boards/esp32-s3-common/presentation.hpp"
#include "platform/boards/szpi-esp32s3/board_config.hpp"
#include "platform/boards/szpi-esp32s3/board_hardware.hpp"
#include "platform/boards/szpi-esp32s3/display_hardware.hpp"
#include "platform/boards/szpi-esp32s3/i2s_audio_sink.hpp"
#include "platform/boards/szpi-esp32s3/sensor_peripheral.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/gpio/esp_gpio_peripheral.hpp"
#include "platform/input/gpio_key_input.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/wifi/native_wifi_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"

namespace micropixel::platform {
namespace {

namespace common = esp32_s3_common;
namespace board_detail = szpi_esp32s3;
constexpr char kTag[] = "szpi_esp32s3";

class SzpiEsp32S3Board final : public Board {
   public:
    SzpiEsp32S3Board()
        : graphics_context_{.engine = &state_.guest_graphics, .hooks = state_.ui.GraphicsHooks()},
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          audio_output_(hardware_),
          presentation_(
              state_, kTag,
              [](void* context, int percent) {
                  const auto safe_percent = static_cast<uint8_t>(std::clamp(percent, 0, 100));
                  return static_cast<board_detail::BoardHardware*>(context)->SetBrightness(
                      controllers::VisibleBrightnessPercent(safe_percent));
              },
              &hardware_),
          system_ui_(state_.ui, presentation_),
          gpio_(board_detail::board::kApplicationGpioLines) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_LOGI(kTag, "initializing LCKFB SZPI ESP32-S3 preview");
        ESP_RETURN_ON_ERROR(board_detail::InitializeDisplayHardware(hardware_, state_), kTag,
                            "initialize ST7789 display failed");
#if CONFIG_MICROPIXEL_SZPI_S3_LVGL_DOUBLE_BUFFER
        constexpr bool kDoubleBuffer = true;
#else
        constexpr bool kDoubleBuffer = false;
#endif
        ESP_RETURN_ON_ERROR(common::RegisterLvglDisplay(state_, kDoubleBuffer), kTag, "register LVGL display failed");
        ESP_RETURN_ON_ERROR(state_.display_shadow.Initialize(state_.display), kTag,
                            "initialize PSRAM displayed shadow failed");
        ESP_RETURN_ON_ERROR(board_detail::InitializeTouch(hardware_, state_), kTag, "initialize FT6336 touch failed");
        ESP_RETURN_ON_ERROR(state_.i2c_executor.Initialize(), kTag, "start shared I2C executor failed");
        ESP_RETURN_ON_ERROR(state_.touch_input.Initialize(state_.touch, state_.i2c_executor), kTag,
                            "bind FT6336 touch input failed");
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, nullptr), kTag,
                            "initialize RGB565 Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        const esp_err_t ui_status = state_.ui.InitializeLocked(state_.display);
        esp_lv_adapter_unlock();
        ESP_RETURN_ON_ERROR(ui_status, kTag, "initialize 320x240 Host UI failed");
        ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), kTag, "start LVGL adapter failed");
        ESP_RETURN_ON_ERROR(state_.touch_input.Start(state_.display), kTag, "start polled FT6336 input failed");

        sensors_.Initialize(hardware_.I2cBus(), state_.i2c_executor);

        const esp_err_t audio_status = audio_output_.Configure(hardware_.I2cBus(), state_.i2c_executor);
        if (audio_status != ESP_OK) {
            ESP_LOGW(kTag, "audio unavailable for this boot: %s", esp_err_to_name(audio_status));
        }
        const esp_err_t gpio_status = gpio_.Initialize();
        if (gpio_status != ESP_OK) {
            ESP_LOGW(kTag, "expansion GPIO unavailable for this boot: %s", esp_err_to_name(gpio_status));
        }
        ESP_RETURN_ON_ERROR(boot_button_.Initialize(state_.ui.Input()), kTag, "initialize BOOT key failed");
        ESP_RETURN_ON_ERROR(state_.development_display.Start(state_.display, state_.touch_input, state_.local_control,
                                                             common::kWidth, common::kHeight,
                                                             {.pixels = state_.display_shadow.Pixels(),
                                                              .stride = state_.display_shadow.Stride(),
                                                              .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                                              .ready = state_.display_shadow.Ready()}),
                            kTag, "start USB development control failed");
        ESP_RETURN_ON_ERROR(hardware_.SetBrightness(80), kTag, "set startup brightness failed");

        BoardRegistration registration{{
            .board = "LCKFB SZPI ESP32-S3",
            .host_chip = "ESP32-S3",
            .wifi_coprocessor = "Native ESP32-S3",
            .touch_controller = state_.touch_name,
            .display =
                {
                    .driver = state_.panel_name,
                    .interface = "SPI 80 MHz",
                    .pixel_format = "RGB565 (big-endian wire)",
                    .width_pixels = common::kWidth,
                    .height_pixels = common::kHeight,
                },
            .graphics_acceleration = "CPU only; SPI DMA transport",
        }};
        registration.SetInput(state_.ui.Input());
        registration.SetGraphics(graphics_);
        if (audio_status == ESP_OK) {
            registration.SetAudioOutput(audio_output_, 16000U);
        }
        registration.SetWifi(wifi_);
        registration.SetLocalControl(state_.local_control);
        registration.SetSystemUi(system_ui_);
        bool registered = true;
        if (sensors_.acceleration_available()) {
            registered = registration.AddSensor(sensors_, board_detail::SensorPeripheral::kAcceleration,
                                                "Built-in QMI8658 accelerometer") &&
                         registered;
        }
        if (sensors_.angular_velocity_available()) {
            registered = registration.AddSensor(sensors_, board_detail::SensorPeripheral::kAngularVelocity,
                                                "Built-in QMI8658 gyroscope") &&
                         registered;
        }
        if (gpio_status == ESP_OK) {
            for (const auto& line : board_detail::board::kExpansionGpioLines) {
                registered = registration.AddGpio(gpio_, line.channel, line.name) && registered;
            }
        }
        ESP_LOGI(kTag, "ready: ST7789 + FT6336, native Wi-Fi, audio=%s", audio_status == ESP_OK ? "ES8311" : "off");
        return registered && context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    common::Landscape320State state_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    board_detail::BoardHardware hardware_{};
    board_detail::I2sAudioSink audio_output_;
    board_detail::SensorPeripheral sensors_{};
    common::Landscape320Presentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
    gpio::EspGpioPeripheral gpio_;
    input::GpioKeyInput boot_button_{{.pin = board_detail::board::kBootButton,
                                      .code = device::KeyCode::kConfirm,
                                      .log_tag = "szpi_button",
                                      .task_name = "szpi_button",
                                      .task_core = 0}};
    wifi::NativeWifiRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
};

}  // namespace

Board& ConfiguredBoard() {
    static SzpiEsp32S3Board board;
    return board;
}

}  // namespace micropixel::platform
