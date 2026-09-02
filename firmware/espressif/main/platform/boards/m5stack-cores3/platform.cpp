#include "platform/platform.hpp"

#include <algorithm>
#include <expected>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/esp32-s3-common/landscape_320_state.hpp"
#include "platform/boards/esp32-s3-common/lvgl_display.hpp"
#include "platform/boards/esp32-s3-common/presentation.hpp"
#include "platform/boards/m5stack-cores3/battery_peripheral.hpp"
#include "platform/boards/m5stack-cores3/board_hardware.hpp"
#include "platform/boards/m5stack-cores3/display_hardware.hpp"
#include "platform/boards/m5stack-cores3/i2s_audio_sink.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/drivers/sensors/bmi270.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/sensors/polled_inertial_sensor_peripheral.hpp"
#include "platform/wifi/native_wifi_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"

namespace micropixel::platform {
namespace {

namespace common = esp32_s3_common;
namespace board_detail = m5stack_cores3;
constexpr char kTag[] = "m5stack_cores3";

class M5StackCoreS3Board final : public Board, public device::Power {
   public:
    M5StackCoreS3Board()
        : graphics_context_{.engine = &state_.guest_graphics, .hooks = state_.ui.GraphicsHooks()},
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          acceleration_(inertial_, drivers::Bmi270::Kind::kAcceleration),
          angular_velocity_(inertial_, drivers::Bmi270::Kind::kAngularVelocity),
          inertial_sensors_(acceleration_, angular_velocity_,
                            {.log_tag = "cores3_imu",
                             .model = "BMI270",
                             .acceleration_timer_name = "cores3_accel",
                             .angular_velocity_timer_name = "cores3_gyro"}),
          presentation_(
              state_, kTag,
              [](void* context, int percent) {
                  const auto safe_percent = static_cast<uint8_t>(std::clamp(percent, 0, 100));
                  return static_cast<board_detail::BoardHardware*>(context)->SetBrightness(
                      controllers::VisibleBrightnessPercent(safe_percent));
              },
              &hardware_),
          system_ui_(state_.ui, presentation_) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_LOGI(kTag, "initializing M5Stack CoreS3 preview");
        ESP_RETURN_ON_ERROR(board_detail::InitializeDisplayHardware(hardware_, state_), kTag,
                            "initialize ILI9342C display failed");
#if CONFIG_MICROPIXEL_CORES3_LVGL_DOUBLE_BUFFER
        constexpr bool kDoubleBuffer = true;
#else
        constexpr bool kDoubleBuffer = false;
#endif
        ESP_RETURN_ON_ERROR(common::RegisterLvglDisplay(state_, kDoubleBuffer), kTag, "register LVGL display failed");
        ESP_RETURN_ON_ERROR(state_.display_shadow.Initialize(state_.display), kTag,
                            "initialize PSRAM displayed shadow failed");
        const esp_err_t touch_status = board_detail::InitializeTouch(hardware_, state_);
        const bool touch_available = touch_status == ESP_OK;
        if (!touch_available) {
            state_.touch_name = "Unavailable";
            ESP_LOGW(kTag, "FT6336U unavailable after recovery; continuing without touch: %s",
                     esp_err_to_name(touch_status));
        }
        ESP_RETURN_ON_ERROR(state_.i2c_executor.Initialize(), kTag, "start shared I2C executor failed");
        hardware_.BindExecutor(state_.i2c_executor);
        if (touch_available) {
            ESP_RETURN_ON_ERROR(state_.touch_input.Initialize(state_.touch, state_.i2c_executor), kTag,
                                "bind FT6336U touch input failed");
        }
        inertial_sensors_.Initialize(hardware_.I2cBus(), state_.i2c_executor);
        battery_.Initialize(hardware_, state_.i2c_executor);
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, nullptr), kTag,
                            "initialize RGB565 Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        const esp_err_t ui_status = state_.ui.InitializeLocked(state_.display);
        esp_lv_adapter_unlock();
        ESP_RETURN_ON_ERROR(ui_status, kTag, "initialize 320x240 Host UI failed");
        ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), kTag, "start LVGL adapter failed");
        if (touch_available) {
            ESP_RETURN_ON_ERROR(state_.touch_input.Start(state_.display), kTag, "start polled FT6336U input failed");
        }

        const esp_err_t audio_status = audio_output_.Configure(hardware_.I2cBus(), state_.i2c_executor);
        if (audio_status != ESP_OK) {
            ESP_LOGW(kTag, "audio unavailable for this boot: %s", esp_err_to_name(audio_status));
        }
        ESP_RETURN_ON_ERROR(state_.development_display.Start(state_.display, state_.touch_input, state_.local_control,
                                                             common::kWidth, common::kHeight,
                                                             {.pixels = state_.display_shadow.Pixels(),
                                                              .stride = state_.display_shadow.Stride(),
                                                              .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                                              .ready = state_.display_shadow.Ready()}),
                            kTag, "start USB development control failed");
        ESP_RETURN_ON_ERROR(hardware_.SetBrightness(80), kTag, "set startup brightness failed");

        BoardRegistration registration{{
            .board = "M5Stack CoreS3",
            .host_chip = "ESP32-S3",
            .wifi_coprocessor = "Native ESP32-S3",
            .touch_controller = state_.touch_name,
            .display =
                {
                    .driver = state_.panel_name,
                    .interface = "SPI 40 MHz",
                    .pixel_format = "RGB565 (big-endian wire)",
                    .width_pixels = common::kWidth,
                    .height_pixels = common::kHeight,
                },
            .graphics_acceleration = "CPU only; SPI DMA transport",
        }};
        if (touch_available) {
            registration.SetInput(state_.ui.Input());
        }
        registration.SetGraphics(graphics_);
        if (audio_status == ESP_OK) {
            registration.SetAudioOutput(audio_output_, 16000U, &hardware_);
        }
        registration.SetBattery(battery_);
        registration.SetPower(*this);
        registration.SetWifi(wifi_);
        registration.SetLocalControl(state_.local_control);
        registration.SetSystemUi(system_ui_);
        bool registered = true;
        if (inertial_sensors_.acceleration_available()) {
            registered =
                registration.AddSensor(inertial_sensors_, sensors::PolledInertialSensorPeripheral::kAcceleration,
                                       "Built-in BMI270 accelerometer") &&
                registered;
        }
        if (inertial_sensors_.angular_velocity_available()) {
            registered =
                registration.AddSensor(inertial_sensors_, sensors::PolledInertialSensorPeripheral::kAngularVelocity,
                                       "Built-in BMI270 gyroscope") &&
                registered;
        }
        ESP_LOGI(kTag, "ready: ILI9342C + %s, native Wi-Fi, audio=%s",
                 touch_available ? "FT6336U" : "touch unavailable", audio_status == ESP_OK ? "AW88298" : "off");
        return registered && context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

    void SetPowerButtonSink(device::PowerButtonSink, void*) override {}
    void SetPowerOffButtonSink(device::PowerOffButtonSink, void*) override {}
    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return std::unexpected(device::PowerError::kUnavailable);
    }
    [[noreturn]] void PowerOff() override {
        while (true) {
            const esp_err_t status = hardware_.RequestPowerOff();
            if (status != ESP_OK) {
                ESP_LOGE(kTag, "AXP2101 power-off request failed: %s", esp_err_to_name(status));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

   private:
    common::Landscape320State state_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    board_detail::BoardHardware hardware_{};
    board_detail::I2sAudioSink audio_output_{};
    board_detail::BatteryPeripheral battery_{};
    drivers::Bmi270 inertial_{};
    drivers::Bmi270Vector acceleration_;
    drivers::Bmi270Vector angular_velocity_;
    sensors::PolledInertialSensorPeripheral inertial_sensors_;
    common::Landscape320Presentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
    wifi::NativeWifiRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
};

}  // namespace

Board& ConfiguredBoard() {
    static M5StackCoreS3Board board;
    return board;
}

}  // namespace micropixel::platform
