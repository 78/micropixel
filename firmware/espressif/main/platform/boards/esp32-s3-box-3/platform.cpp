#include "platform/platform.hpp"

#include <cstdint>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "lvgl.h"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/esp32-s3-box-3/board_config.hpp"
#include "platform/boards/esp32-s3-box-3/board_hardware.hpp"
#include "platform/boards/esp32-s3-box-3/display_hardware.hpp"
#include "platform/boards/esp32-s3-box-3/host_mute_control.hpp"
#include "platform/boards/esp32-s3-box-3/i2s_audio_sink.hpp"
#include "platform/boards/esp32-s3-box-3/platform_state.hpp"
#include "platform/boards/esp32-s3-box-3/presentation.hpp"
#include "platform/boards/esp32-s3-box-3/sensor_peripheral.hpp"
#include "platform/boards/esp32-s3-common/lvgl_display.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/gpio/esp_gpio_peripheral.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/wifi/native_wifi_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"

namespace micropixel::platform {
namespace {

namespace board_detail = esp32_s3_box_3::detail;

esp_err_t InitializeDisplay(esp32_s3_box_3::BoardHardware& hardware, board_detail::Box3BoardState& state) {
    ESP_RETURN_ON_ERROR(board_detail::InitializeDisplayHardware(hardware, state), board_detail::kTag,
                        "ESP-BOX-3 display initialization failed");
#if CONFIG_MICROPIXEL_BOX3_LVGL_DOUBLE_BUFFER
    constexpr bool kDoubleBuffer = true;
#else
    constexpr bool kDoubleBuffer = false;
#endif
    return esp32_s3_common::RegisterLvglDisplay(state, kDoubleBuffer);
}

esp_err_t InitializeTouch(esp32_s3_box_3::BoardHardware& hardware, board_detail::Box3BoardState& state) {
    ESP_RETURN_ON_ERROR(board_detail::InitializeTouchHardware(hardware, state), board_detail::kTag,
                        "ESP-BOX-3 touch initialization failed");
    ESP_RETURN_ON_ERROR(state.i2c_executor.Initialize(), board_detail::kTag, "start touch I2C executor failed");
    ESP_RETURN_ON_ERROR(state.touch_input.Initialize(state.touch, state.i2c_executor), board_detail::kTag,
                        "bind ESP-BOX-3 touch input failed");
    return ESP_OK;
}

class Esp32S3Box3Board final : public Board {
   public:
    Esp32S3Box3Board()
        : graphics_context_{.engine = &state_.guest_graphics, .hooks = state_.ui.GraphicsHooks()},
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          presentation_(
              state_, board_detail::kTag,
              [](void* context, int percent) {
                  return static_cast<esp32_s3_box_3::BoardHardware*>(context)->SetBrightness(percent);
              },
              &hardware_),
          system_ui_(state_.ui, presentation_) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_LOGI(board_detail::kTag, "initializing ESP32-S3-BOX-3 P4 with native board support");
        ESP_RETURN_ON_ERROR(hardware_.Initialize(), board_detail::kTag, "initialize board hardware failed");
        ESP_RETURN_ON_ERROR(InitializeDisplay(hardware_, state_), board_detail::kTag, "initialize display failed");
        ESP_RETURN_ON_ERROR(state_.display_shadow.Initialize(state_.display), board_detail::kTag,
                            "initialize PSRAM displayed shadow failed");
        ESP_RETURN_ON_ERROR(InitializeTouch(hardware_, state_), board_detail::kTag, "initialize touch failed");
        sensors_.Initialize(hardware_.I2cBus(), state_.i2c_executor);
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, nullptr), board_detail::kTag,
                            "initialize RGB565 Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        const esp_err_t status = state_.ui.InitializeLocked(state_.display);
        esp_lv_adapter_unlock();
        ESP_RETURN_ON_ERROR(status, board_detail::kTag, "initialize 320x240 Host UI failed");

        // Finish every LVGL allocation and display callback registration before
        // the worker can refresh the display.  Starting it in InitializeDisplay()
        // races DisplayShadow::Initialize(), GuestGraphicsEngine::Initialize(),
        // and System Shell construction against LVGL's non-thread-safe TLSF
        // allocator, which can corrupt the PSRAM pool during cold boot.
        ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), board_detail::kTag, "start LVGL adapter failed");
        ESP_RETURN_ON_ERROR(state_.touch_input.Start(state_.display), board_detail::kTag,
                            "start interrupt-driven BOX-3 touch failed");
        const esp_err_t audio_config_status = audio_output_.Configure(hardware_.I2cBus(), state_.i2c_executor);
        if (audio_config_status != ESP_OK) {
            ESP_LOGW(board_detail::kTag, "BOX-3 audio unavailable for this boot: %s",
                     esp_err_to_name(audio_config_status));
        }
        const esp_err_t mute_status = mute_control_.Initialize(context.AudioEngine());
        if (mute_status != ESP_OK) {
            ESP_LOGW(board_detail::kTag, "BOX-3 Host mute monitor unavailable for this boot: %s",
                     esp_err_to_name(mute_status));
        }
        const esp_err_t gpio_status = gpio_.Initialize();
        if (gpio_status != ESP_OK) {
            ESP_LOGW(board_detail::kTag, "BOX-3 Pmod GPIO unavailable for this boot: %s", esp_err_to_name(gpio_status));
        }
        ESP_RETURN_ON_ERROR(state_.development_display.Start(state_.display, state_.touch_input, state_.local_control,
                                                             board_detail::kWidth, board_detail::kHeight,
                                                             {.pixels = state_.display_shadow.Pixels(),
                                                              .stride = state_.display_shadow.Stride(),
                                                              .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                                              .ready = state_.display_shadow.Ready()}),
                            board_detail::kTag, "start USB Serial/JTAG development control failed");
        ESP_RETURN_ON_ERROR(hardware_.SetBrightness(80), board_detail::kTag, "set startup brightness failed");

        ESP_LOGI(board_detail::kTag, "BOX-3 P4 ready: panel=%s touch=%s 320x240 RGB565 SPI=40MHz Wi-Fi=native audio=%s",
                 state_.panel_name, state_.touch_name, audio_config_status == ESP_OK ? "ES8311" : "unavailable");
        BoardRegistration registration{{
            .board = "ESP32-S3-BOX-3 (P4)",
            .host_chip = "ESP32-S3",
            .wifi_coprocessor = "Native ESP32-S3",
            .touch_controller = state_.touch_name,
            .display =
                {
                    .driver = state_.panel_name,
                    .interface = "SPI 40 MHz",
                    .pixel_format = "RGB565 (big-endian wire)",
                    .width_pixels = board_detail::kWidth,
                    .height_pixels = board_detail::kHeight,
                },
            .graphics_acceleration = "CPU only; SPI DMA transport",
        }};
        registration.SetInput(state_.ui.Input());
        registration.SetGraphics(graphics_);
        if (audio_config_status == ESP_OK) {
            registration.SetAudioOutput(audio_output_, 16000U);
        }
        registration.SetWifi(wifi_);
        registration.SetLocalControl(state_.local_control);
        registration.SetSystemUi(system_ui_);
        bool registered = true;
        if (sensors_.acceleration_available()) {
            registered = registration.AddSensor(sensors_, esp32_s3_box_3::SensorPeripheral::kAcceleration,
                                                "Built-in ICM-42607-P accelerometer") &&
                         registered;
        }
        if (sensors_.angular_velocity_available()) {
            registered = registration.AddSensor(sensors_, esp32_s3_box_3::SensorPeripheral::kAngularVelocity,
                                                "Built-in ICM-42607-P gyroscope") &&
                         registered;
        }
        if (gpio_status == ESP_OK) {
            for (const auto& line : esp32_s3_box_3::board::kPmodGpioLines) {
                registered = registration.AddGpio(gpio_, line.channel, line.name) && registered;
            }
        }
        return registered && context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    esp32_s3_box_3::BoardHardware hardware_{};
    board_detail::Box3BoardState state_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    esp32_s3_box_3::I2sAudioSink audio_output_{};
    esp32_s3_box_3::HostMuteControl mute_control_{};
    esp32_s3_box_3::SensorPeripheral sensors_{};
    gpio::EspGpioPeripheral gpio_{esp32_s3_box_3::board::kApplicationGpioLines};
    board_detail::Box3Presentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
    wifi::NativeWifiRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
};

}  // namespace

Board& ConfiguredBoard() {
    static Esp32S3Box3Board board;
    return board;
}

}  // namespace micropixel::platform
