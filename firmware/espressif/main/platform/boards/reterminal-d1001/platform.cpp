// SPDX-License-Identifier: Apache-2.0
#include "platform/platform.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/audio/es8311_i2s_audio_sink.hpp"
#include "platform/boards/reterminal-d1001/audio_config.hpp"
#include "platform/boards/reterminal-d1001/board_config.hpp"
#include "platform/boards/reterminal-d1001/board_hardware.hpp"
#include "platform/boards/reterminal-d1001/d1001_state.hpp"
#include "platform/boards/reterminal-d1001/display_hardware.hpp"
#include "platform/boards/reterminal-d1001/power_controller.hpp"
#include "platform/boards/reterminal-d1001/power_key.hpp"
#include "platform/boards/reterminal-d1001/presentation.hpp"
#include "platform/boards/reterminal-d1001/touch_hardware.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/memory/ext_ram_bss.hpp"
#include "platform/memory/internal_ram.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/wifi/esp_hosted_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "sdkconfig.h"

// The 24 MiB app_store starts at 8 MiB and crosses the 16 MiB physical flash
// boundary. Without 32-bit cache mapping, BundleFS blocks above that boundary
// cannot be mapped even though the partition table is valid.
#if CONFIG_ESPTOOLPY_FLASHSIZE_32MB && !CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH
#error "D1001 32 MiB flash layout requires CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH"
#endif

namespace micropixel::platform {
namespace {

namespace board = reterminal_d1001::board;

constexpr char kTag[] = "reterminal_d1001";

class ReTerminalD1001Board final : public Board {
   public:
    ReTerminalD1001Board()
        : state_(TaskState(input_state_)),
          graphics_context_{.engine = &state_.guest_graphics, .hooks = state_.ui.GraphicsHooks()},
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          power_(hardware_),
          power_key_(power_),
          presentation_(state_, hardware_, kTag),
          system_ui_(state_.ui, presentation_) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        ESP_RETURN_ON_FALSE(memory::IsInternalObject(*this), ESP_ERR_INVALID_STATE, kTag,
                            "Board control objects must reside in internal RAM");
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_LOGI(kTag, "initializing Seeed reTerminal D1001 (JD9365 + GSL3670)");

        ESP_RETURN_ON_ERROR(hardware_.Initialize(), kTag, "initialize board hardware failed");
        ESP_RETURN_ON_ERROR(state_.system_i2c_executor.Initialize(), kTag, "start system I2C executor failed");
        ESP_RETURN_ON_ERROR(state_.touch_i2c_executor.Initialize(), kTag, "start touch I2C executor failed");
        ESP_RETURN_ON_ERROR(hardware_.ResetPanelAndTouch(), kTag, "reset panel and touch failed");
        ESP_RETURN_ON_ERROR(InitializeDisplayHardware(state_), kTag, "initialize JD9365 display failed");
        ESP_RETURN_ON_ERROR(RegisterLvglDisplay(state_), kTag, "register LVGL display failed");

        // Touch is optional at runtime even when built in: initialization or
        // the first probe read may fail, and that must not keep the display,
        // backlight or USB transport down. The driver itself comes from the
        // external seeed/esp_lcd_touch_gsl3670 component.
        esp_err_t touch_status = ESP_ERR_NOT_SUPPORTED;
#if CONFIG_MICROPIXEL_D1001_ENABLE_GSL3670
        touch_status = InitializeTouchHardware(state_, hardware_);
        if (touch_status == ESP_OK) {
            touch_status = state_.touch_input.Initialize(state_.touch, state_.touch_i2c_executor);
        }
        if (touch_status != ESP_OK) {
            ESP_LOGW(kTag, "GSL3670 touch unavailable: %s", esp_err_to_name(touch_status));
            state_.touch = nullptr;
        }
#else
        ESP_LOGI(kTag, "GSL3670 touch disabled by configuration");
#endif

        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, &state_.framebuffers,
                                                             reterminal_d1001::D1001DirectScanoutProfile()),
                            kTag, "initialize RGB565 Guest graphics failed");
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        const esp_err_t ui_status = state_.ui.InitializeLocked(state_.display);
        esp_lv_adapter_unlock();
        ESP_RETURN_ON_ERROR(ui_status, kTag, "initialize 800x1280 Host UI failed");

        if (touch_status == ESP_OK) {
            // A probe read failure must not keep LVGL and the backlight down.
            // Drop touch and keep booting with an unavailable input backend.
            const esp_err_t input_status = state_.touch_input.Start(state_.display);
            if (input_status != ESP_OK) {
                ESP_LOGW(kTag, "GSL3670 input start failed: %s", esp_err_to_name(input_status));
                state_.touch = nullptr;
                touch_status = input_status;
            }
        }
        ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), kTag, "start LVGL adapter failed");
        ESP_RETURN_ON_ERROR(hardware_.SetBrightness(80), kTag, "set startup brightness failed");

        // Audio is optional: a missing codec must not stop the Host.
        const esp_err_t audio_status = audio_output_.Configure(hardware_.I2cBus(), state_.system_i2c_executor);
        if (audio_status != ESP_OK) {
            ESP_LOGW(kTag, "audio unavailable for this boot: %s", esp_err_to_name(audio_status));
        }

        // The green key is optional: failure to configure it costs the key,
        // never the boot. A short press toggles the screen; a one-second hold
        // requests power off.
        const esp_err_t key_status = power_key_.Initialize();
        if (key_status != ESP_OK) {
            ESP_LOGW(kTag, "power key unavailable: %s", esp_err_to_name(key_status));
        }

        // USB development screenshots take the same path as Remote Control.
        // It also starts the MPX1 local-control transport. Capture, App
        // installation and synthetic touch injection must stay available even
        // when the physical touch backend is disabled or failed.
        const esp_err_t bridge_status = state_.development_display.Start(
            state_.touch_input, state_.local_control, static_cast<uint32_t>(reterminal_d1001::kWidth),
            static_cast<uint32_t>(reterminal_d1001::kHeight), transports::DevelopmentCaptureHook::For(presentation_));
        if (bridge_status != ESP_OK) {
            ESP_LOGW(kTag, "USB development bridge unavailable: %s", esp_err_to_name(bridge_status));
        }

        // Fixed-size process-lifetime registration lives in PSRAM BSS. Keeping
        // it on the stack would exceed the repository's 1536-byte frame limit.
        static MICROPIXEL_EXT_RAM_BSS BoardRegistration registration{{
            .board = "Seeed reTerminal D1001",
            .host_chip = "ESP32-P4",
            .firmware_target = "reterminal-d1001",
            .wifi_coprocessor = "ESP32-C6",
            .touch_controller = "GSL3670",
            .display =
                {
                    .driver = "JD9365",
                    .interface = "MIPI-DSI 2-lane 1.5 Gbps",
                    .pixel_format = "RGB565",
                    .width_pixels = static_cast<uint32_t>(board::kDisplayWidth),
                    .height_pixels = static_cast<uint32_t>(board::kDisplayHeight),
                    .refresh_rate_hz = board::kDisplayRefreshRateHz,
                },
            .graphics_acceleration = "PPA",
        }};
        registration.SetGraphics(graphics_);
        if (touch_status == ESP_OK) {
            registration.SetInput(state_.ui.Input());
        }
        registration.SetSystemUi(system_ui_);
        registration.SetPower(power_);
        registration.SetLocalControl(state_.local_control);
        if (audio_status == ESP_OK) {
            registration.SetAudioOutput(audio_output_, audio_output_.SampleRate());
        }
        // Wi-Fi runs on the on-board ESP32-C6 over ESP-Hosted SDIO. A missing or
        // mismatched coprocessor firmware only costs the network.
        registration.SetWifi(wifi_);
        ESP_LOGI(kTag, "ready: JD9365 800x1280 RGB565, GSL3670 %s, audio=%s",
                 touch_status == ESP_OK ? "polling" : "off", audio_status == ESP_OK ? "ES8311" : "off");
        return context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        state_.guest_graphics.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    static reterminal_d1001::D1001State& TaskState(reterminal_d1001::D1001InputState& input) {
        static MICROPIXEL_EXT_RAM_BSS reterminal_d1001::D1001State state(input);
        return state;
    }

    reterminal_d1001::D1001InputState input_state_{};
    reterminal_d1001::D1001State& state_;
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    reterminal_d1001::BoardHardware hardware_{};
    audio::Es8311I2sAudioSink audio_output_{reterminal_d1001::board::AudioConfig(hardware_),
                                            {.amplifier_voltage = 5.0F, .codec_dac_voltage = 3.3F}};
    reterminal_d1001::D1001PowerController power_;
    reterminal_d1001::D1001PowerKey power_key_;
    wifi::EspHostedRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
    reterminal_d1001::D1001Presentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
};

}  // namespace

Board& ConfiguredBoard() {
    static ReTerminalD1001Board board;
    return board;
}

}  // namespace micropixel::platform