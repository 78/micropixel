// SPDX-License-Identifier: Apache-2.0
#include "platform/platform.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/m5stack-tab5/audio_config.hpp"
#include "platform/boards/m5stack-tab5/battery_peripheral.hpp"
#include "platform/boards/m5stack-tab5/board_config.hpp"
#include "platform/boards/m5stack-tab5/board_hardware.hpp"
#include "platform/boards/m5stack-tab5/display_hardware.hpp"
#include "platform/boards/m5stack-tab5/inertial_axis.hpp"
#include "platform/boards/m5stack-tab5/power_controller.hpp"
#include "platform/boards/m5stack-tab5/presentation.hpp"
#include "platform/boards/m5stack-tab5/rtc_clock.hpp"
#include "platform/boards/m5stack-tab5/sd_card.hpp"
#include "platform/boards/m5stack-tab5/tab5_state.hpp"
#include "platform/drivers/sensors/bmi270.hpp"
#include "platform/gpio/esp_gpio_peripheral.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/memory/ext_ram_bss.hpp"
#include "platform/memory/internal_ram.hpp"
#include "platform/sensors/polled_inertial_sensor_peripheral.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/wifi/esp_hosted_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"

namespace micropixel::platform {
namespace {

namespace board_detail = m5stack_tab5;
namespace board = m5stack_tab5::board;

constexpr char kTag[] = "m5stack_tab5";

class M5StackTab5Board final : public Board {
   public:
    M5StackTab5Board()
        : state_(TaskState(input_state_)),
          graphics_context_{.engine = &state_.guest_graphics, .hooks = state_.ui.GraphicsHooks()},
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          power_(hardware_),
          audio_output_(board::AudioConfig(hardware_.Expander()), board::CodecConfig()),
          inertial_(board::kInertialAddress),
          acceleration_source_(inertial_, drivers::Bmi270::Kind::kAcceleration),
          angular_velocity_source_(inertial_, drivers::Bmi270::Kind::kAngularVelocity),
          acceleration_(acceleration_source_, board_detail::Tab5InertialAxis::kAccelerationMapping),
          angular_velocity_(angular_velocity_source_, board_detail::Tab5InertialAxis::kAngularVelocityMapping),
          inertial_sensors_(acceleration_, angular_velocity_,
                            {.log_tag = "tab5_imu",
                             .model = "BMI270",
                             .acceleration_timer_name = "tab5_accel",
                             .angular_velocity_timer_name = "tab5_gyro"}),
          presentation_(state_, hardware_, kTag),
          system_ui_(state_.ui, presentation_) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        ESP_RETURN_ON_FALSE(memory::IsInternalObject(*this), ESP_ERR_INVALID_STATE, kTag,
                            "Board control objects must reside in internal RAM");
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_LOGI(kTag, "initializing M5Stack Tab5 (ILI9881C + GT911)");

        ESP_RETURN_ON_ERROR(hardware_.Initialize(), kTag, "initialize board hardware failed");
        ESP_RETURN_ON_ERROR(state_.i2c_executor.Initialize(), kTag, "start shared I2C executor failed");
        // The RTC seeds the system clock before the UI and the network stack
        // come up; a missing chip only costs the time source, never the boot.
        rtc_clock_.Initialize(hardware_.I2cBus(), state_.i2c_executor);
        ESP_RETURN_ON_ERROR(hardware_.ResetPanelAndTouch(), kTag, "reset panel and touch failed");
        ESP_RETURN_ON_ERROR(board_detail::InitializeDisplayHardware(state_), kTag,
                            "initialize ILI9881C display failed");
        ESP_RETURN_ON_ERROR(board_detail::RegisterLvglDisplay(state_), kTag, "register LVGL display failed");
        ESP_RETURN_ON_ERROR(state_.touch_input.Initialize(hardware_.I2cBus(), state_.i2c_executor), kTag,
                            "bind GT911 touch failed");
        // The IMU is optional: it must not stop the Host when it is missing.
        inertial_sensors_.Initialize(hardware_.I2cBus(), state_.i2c_executor);
        // The expansion header lines are optional as well: a rejected line table
        // costs the Guest GPIO service, never the boot.
        const esp_err_t gpio_status = gpio_.Initialize();
        if (gpio_status != ESP_OK) {
            ESP_LOGW(kTag, "expansion GPIO unavailable for this boot: %s", esp_err_to_name(gpio_status));
        }
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, &state_.framebuffers,
                                                             board_detail::Tab5DirectScanoutProfile()),
                            kTag, "initialize RGB565 Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        const esp_err_t ui_status = state_.ui.InitializeLocked(state_.display);
        esp_lv_adapter_unlock();
        ESP_RETURN_ON_ERROR(ui_status, kTag, "initialize 720x1280 Host UI failed");

        ESP_RETURN_ON_ERROR(state_.touch_input.Start(state_.display), kTag, "start GT911 input failed");
        ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), kTag, "start LVGL adapter failed");
        ESP_RETURN_ON_ERROR(hardware_.SetBrightness(80), kTag, "set startup brightness failed");

        // Audio is optional: a missing codec must not stop the Host.
        const esp_err_t audio_status = audio_output_.Configure(hardware_.I2cBus(), state_.i2c_executor);
        if (audio_status != ESP_OK) {
            ESP_LOGW(kTag, "audio unavailable for this boot: %s", esp_err_to_name(audio_status));
        }
        // The battery gauge is optional too: without an INA226 the peripheral
        // reports an unavailable snapshot and the status bar hides the icon.
        battery_.Initialize(hardware_.I2cBus(), hardware_.Expander(), state_.i2c_executor);
        // The card socket is optional: without a card the App Store keeps
        // installing into the NOR app_store partition.
        sd_card_.Initialize();
        // Starting the development bridge also starts the USB local-control
        // transport; MPX1 install/control frames share the same byte stream.
        ESP_RETURN_ON_ERROR(state_.development_display.Start(state_.touch_input, state_.local_control,
                                                             static_cast<uint32_t>(board_detail::kWidth),
                                                             static_cast<uint32_t>(board_detail::kHeight),
                                                             transports::DevelopmentCaptureHook::For(presentation_)),
                            kTag, "start USB local control failed");

        // Fixed-size process-lifetime registration lives in PSRAM BSS. Keeping
        // it on the stack would exceed the repository's 1536-byte frame limit.
        static MICROPIXEL_EXT_RAM_BSS BoardRegistration registration{{
            .board = "M5Stack Tab5",
            .host_chip = "ESP32-P4",
            .firmware_target = "m5stack-tab5",
            .wifi_coprocessor = "ESP32-C6",
            .touch_controller = "GT911",
            .display =
                {
                    .driver = "ILI9881C",
                    .interface = "MIPI-DSI 2-lane 730 Mbps",
                    .pixel_format = "RGB565",
                    .width_pixels = static_cast<uint32_t>(board::kDisplayWidth),
                    .height_pixels = static_cast<uint32_t>(board::kDisplayHeight),
                    .refresh_rate_hz = 60U,
                },
            .graphics_acceleration = "PPA + DMA2D",
        }};
        registration.SetGraphics(graphics_);
        registration.SetInput(state_.ui.Input());
        registration.SetSystemUi(system_ui_);
        registration.SetPower(power_);
        registration.SetLocalControl(state_.local_control);
        if (audio_status == ESP_OK) {
            registration.SetAudioOutput(audio_output_, audio_output_.SampleRate());
        }
        registration.SetBattery(battery_);
        // Wi-Fi runs on the on-board ESP32-C6 over ESP-Hosted SDIO. A missing or
        // mismatched coprocessor firmware only costs the network: initialization
        // fails and the System UI reports Wi-Fi as unavailable.
        registration.SetWifi(wifi_);
        // The card is the user's medium, so the platform may never format it on
        // its own: a foreign card is reported as "needs formatting" and the
        // System settings ask first.
        if (sd_card_.storage() != nullptr) {
            registration.SetAppStorage(*sd_card_.storage(), 0U, /*removable=*/true);
        }
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
        // M5-Bus and ExtPort1 reach the Guest through the existing GPIO service;
        // the line list and its strapping notes live in board_config.hpp.
        if (gpio_status == ESP_OK) {
            for (const auto& line : board::kApplicationGpioLineNames) {
                registered = registration.AddGpio(gpio_, line.channel, line.name) && registered;
            }
        }
        ESP_LOGI(kTag, "ready: ILI9881C + GT911, 720x1280 portrait%s%s%s", audio_status == ESP_OK ? ", ES8388" : "",
                 inertial_sensors_.acceleration_available() ? ", BMI270" : "",
                 gpio_status == ESP_OK ? ", M5-Bus/ExtPort1 GPIO" : "");
        return registered && context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        state_.guest_graphics.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    static board_detail::Tab5State& TaskState(board_detail::Tab5InputState& input) {
        static MICROPIXEL_EXT_RAM_BSS board_detail::Tab5State state(input);
        return state;
    }

    board_detail::Tab5InputState input_state_{};
    board_detail::Tab5State& state_;
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    board_detail::BoardHardware hardware_{};
    board_detail::Tab5PowerController power_;
    audio::Es8388I2sAudioSink audio_output_;
    board_detail::BatteryPeripheral battery_{};
    board_detail::RtcClock rtc_clock_{};
    board_detail::SdCard sd_card_{};
    wifi::EspHostedRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
    drivers::Bmi270 inertial_;
    drivers::Bmi270Vector acceleration_source_;
    drivers::Bmi270Vector angular_velocity_source_;
    board_detail::Tab5InertialAxis acceleration_;
    board_detail::Tab5InertialAxis angular_velocity_;
    sensors::PolledInertialSensorPeripheral inertial_sensors_;
    gpio::EspGpioPeripheral gpio_{board::kApplicationGpioLines};
    board_detail::Tab5Presentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
};

}  // namespace

Board& ConfiguredBoard() {
    static M5StackTab5Board board;
    return board;
}

}  // namespace micropixel::platform
