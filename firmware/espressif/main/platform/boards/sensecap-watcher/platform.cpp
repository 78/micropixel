// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/platform.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>

#include "device/contracts/graphics.hpp"
#include "device/contracts/input.hpp"
#include "device/contracts/power.hpp"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ui/lvgl/square_common/profiles/square_412.hpp"
#include "host/ui/lvgl/square_common/square_presentation.hpp"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/esp32-s3-common/display_shadow.hpp"
#include "platform/boards/sensecap-watcher/battery_peripheral.hpp"
#include "platform/boards/sensecap-watcher/board_config.hpp"
#include "platform/boards/sensecap-watcher/board_power.hpp"
#include "platform/boards/sensecap-watcher/display_hardware.hpp"
#include "platform/boards/sensecap-watcher/i2s_audio_sink.hpp"
#include "platform/boards/sensecap-watcher/knob_input.hpp"
#include "platform/boards/sensecap-watcher/touch_hardware.hpp"
#include "platform/boards/sensecap-watcher/uart_local_control.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/host_encoder_router.hpp"
#include "platform/memory/ext_ram_bss.hpp"
#include "platform/memory/internal_ram.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/wifi/native_wifi_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "work/task_policy.hpp"

// Progressive bring-up, as the board port checklist prescribes: this board
// starts with identity and the shared unavailable implementations, then gains
// display, input, audio and power sequencing one at a time. Platform::Publish()
// fills every service the board does not provide yet, so the port can grow
// without board-local placeholders.
namespace micropixel::platform {
namespace {

constexpr char kTag[] = "sensecap_watcher";

// The power-off path retries instead of returning, so this is the gap between
// attempts while the system rail's hold-up capacitance still keeps the SoC
// alive.
constexpr uint32_t kPowerOffRetryDelayMs = 50U;

namespace board_detail = sensecap_watcher;

namespace detail {

namespace ui_profile = host_ui::lvgl::square_common::profiles::square_412;

inline constexpr int32_t kWidth = board_detail::board::kDisplayWidth;
inline constexpr int32_t kHeight = board_detail::board::kDisplayHeight;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);
inline constexpr int kLvglTaskCore = task_policy::kSystemCore;
inline constexpr graphics::SurfacePixelFormat kGuestSurfaceFormat = graphics::SurfacePixelFormat::kRgb565;
// Deterministic start-up level; the user-facing brightness control takes over
// once the Host settings are loaded.
inline constexpr int kStartupBrightnessPercent = 80;

// The cover is round, so the corners of the frame are outside the glass. The
// inset is the one the profile uses: half of (side - side/sqrt(2)), the largest
// axis-aligned box that fits inside the circle. Apps and diagnostics consume
// this, so it has to agree with the built-in layout rather than be a smaller
// cosmetic guard.
inline constexpr uint32_t kSafeAreaInsetPixels = 60U;

// Board-owned input control stays in internal RAM, polled panels included. The
// touch controller is brought up in the input increment; until then the adapter
// exists but is never started, so the Host UI runs with no pointer input.
struct SensecapWatcherInputState final {
    buses::I2cExecutor i2c_executor{};
    input::EspLcdTouchInput touch_input{kWidth, kHeight, device::kMaxTouchPoints};
};

// Large task-only state lives in PSRAM and references Board-owned input
// controls, following the other square-display boards.
struct SensecapWatcherState final {
    explicit SensecapWatcherState(SensecapWatcherInputState& input)
        : i2c_executor(input.i2c_executor), touch_input(input.touch_input) {}

    lv_display_t* display{};
    buses::I2cExecutor& i2c_executor;
    input::EspLcdTouchInput& touch_input;
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts, kGuestSurfaceFormat};
    // This board has no transition compositor yet, so status dialogs and action
    // sheets are presented as static LVGL frames. The shared UI supports that
    // fallback explicitly.
    host_ui::lvgl::square_common::StaticStatusLayerTransition status_transition{};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, status_transition,
                                                         ui_profile::kSystemUiProfile};
    // The SPD2010 adapter byte-swaps its partial buffers, so this PSRAM shadow is
    // the only complete, canonical-RGB565 frame on the board: screenshots and the
    // transition-free presentation both read it. MPX1 rides the board's UART
    // because the PC-facing port is an external USB-UART bridge, and the
    // development bridge owns the screenshot and touch commands on that stream.
    esp32_s3_common::DisplayShadow display_shadow{kWidth, kHeight};
    board_detail::BatteryPeripheral battery{};
    board_detail::UartLocalControl local_control{};
    transports::DevelopmentDisplayControl development_display{};
};

}  // namespace detail

// Rotation moves the LVGL focus and the confirm gesture activates whatever is
// focused, so the knob drives every screen without any screen knowing about it.
// Back runs the page's own back button instead of a Host-wide rule, so the
// gesture always means what the button the user can see means. Holds are not
// navigation: they go to the Host's power path instead.
struct KnobNavigationContext final {
    platform::lvgl::HostEncoderRouter* router{};
    host_ui::lvgl::square_common::SquareSystemUi* system_ui{};
};

// Runs in the encoder router's read callback, which owns the LVGL context.
void RunBackAction(void* context) {
    auto* navigation = static_cast<KnobNavigationContext*>(context);
    if (navigation != nullptr && navigation->system_ui != nullptr) {
        navigation->system_ui->Back();
    }
}

void RouteKnobNavigation(void* context, int32_t rotation_steps, bool confirm, bool back) {
    auto* navigation = static_cast<KnobNavigationContext*>(context);
    if (navigation == nullptr || navigation->router == nullptr) {
        return;
    }
    if (back) {
        navigation->router->AddBack();
        return;
    }
    if (confirm) {
        navigation->router->AddActivation();
        return;
    }
    navigation->router->AddRotation(rotation_steps);
}

// The current screen gets the first offer: the Hall turns a detent into the next
// card and a press into launching the selected one, and every other screen
// returns false so the focus moves and activates as before.
bool RouteKnobRotation(void* context, int32_t rotation_steps) {
    auto* navigation = static_cast<KnobNavigationContext*>(context);
    if (navigation == nullptr || navigation->system_ui == nullptr) {
        return false;
    }
    return navigation->system_ui->Rotate(rotation_steps);
}

bool RouteKnobConfirm(void* context) {
    auto* navigation = static_cast<KnobNavigationContext*>(context);
    if (navigation == nullptr || navigation->system_ui == nullptr) {
        return false;
    }
    return navigation->system_ui->Confirm();
}

// The knob's centre switch sits on the IO expander rather than a GPIO, so the
// read goes through the power object that owns it. The vendor treats the switch
// as active low.
bool ReadKnobSwitch(void* context) {
    bool level = false;
    auto* power = static_cast<board_detail::BoardPower*>(context);
    if (power->ReadPinLevel(board_detail::board::kExpanderPinKnobButton, level) != ESP_OK) {
        return false;
    }
    return !level;
}

// SPD2010 starts a transfer on a multiple of four columns and ends on a 4N+3
// column. Widen the requested band rather than moving it, and keep the result
// inside the panel, so a flush never addresses a partial quad.
void RoundFlushArea(lv_area_t* area, void*) {
    const int32_t start = (area->x1 >> 2) << 2;
    const int32_t end = ((area->x2 >> 2) << 2) + 3;
    area->x1 = std::max<int32_t>(start, 0);
    area->x2 = std::min<int32_t>(end, board_detail::board::kDisplayWidth - 1);
}

esp_err_t RegisterLvglDisplay(detail::SensecapWatcherState& state, board_detail::DisplayHardware& hardware) {
    if (hardware.Panel() == nullptr || hardware.PanelIo() == nullptr || state.display != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_config{};
        adapter_config.task_stack_size = 10240U;
        adapter_config.task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
        adapter_config.task_core_id = detail::kLvglTaskCore;
        adapter_config.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
#ifdef ESP_LV_ADAPTER_HAS_TICK_MODE
        adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_PERIODIC;
#endif
        adapter_config.task_min_delay_ms = 1U;
        adapter_config.task_max_delay_ms = 1U;
        adapter_config.stack_in_psram = true;
        adapter_config.auto_sleep.enable = false;
        adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
        adapter_config.auto_sleep.idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS;
        ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), kTag, "initialize LVGL adapter failed");
    }

    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = hardware.Panel();
    display_config.panel_io = hardware.PanelIo();
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_config.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.profile.hor_res = detail::kWidth;
    display_config.profile.ver_res = detail::kHeight;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#if CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.enable_ppa_accel = false;
    display_config.profile.require_double_buffer = false;
    display_config.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.te_sync = {};
    display_config.te_sync.gpio_num = -1;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_lv_adapter_set_area_rounder_cb(state.display, RoundFlushArea, nullptr), kTag,
                        "configure SPD2010 area alignment failed");
    return esp_lcd_panel_disp_on_off(hardware.Panel(), true);
}

// Brightness, volume and screen capture. Capture reads the pre-transport shadow
// because the SPD2010 adapter byte-swaps its partial buffers, so the active LVGL
// draw buffer never holds a complete frame. Volume drives the shared engine's
// master gain, which is what makes the Host's volume control the single place
// attenuation happens. The remaining optional roles stay null and the shared UI
// remains fully usable without them.
class SensecapWatcherPresentation final : public host_ui::lvgl::square_common::SquarePresentation,
                                          public host_ui::lvgl::square_common::ScreenCapture,
                                          public host_ui::lvgl::square_common::BrightnessControl,
                                          public host_ui::lvgl::square_common::VolumeControl {
   public:
    using BrightnessSetter = esp_err_t (*)(void* context, int percent);

    SensecapWatcherPresentation(detail::SensecapWatcherState& state, BrightnessSetter brightness_setter,
                                void* brightness_context)
        : state_(state), brightness_setter_(brightness_setter), brightness_context_(brightness_context) {}

    // Bound by Initialize() before any audio can play.
    void BindAudioEngine(audio::AudioEngine& audio) { audio_ = &audio; }

    [[nodiscard]] host_ui::lvgl::square_common::ScreenCapture* Capture() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::BrightnessControl* Brightness() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::VolumeControl* Volume() override { return this; }

    [[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() override {
        if (state_.display == nullptr || state_.display_shadow.Pixels() == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        return lvgl::CaptureScreenJpeg(state_.display, static_cast<uint32_t>(detail::kWidth),
                                       static_cast<uint32_t>(detail::kHeight),
                                       {.pixels = state_.display_shadow.Pixels(),
                                        .stride = state_.display_shadow.Stride(),
                                        .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                        .ready = state_.display_shadow.Ready()});
    }

    void ApplyBrightness(uint8_t percent) override {
        const esp_err_t status = brightness_setter_(brightness_context_, percent);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "apply brightness failed: %s", esp_err_to_name(status));
        }
    }

    // Host master volume. Guests never apply app-wide attenuation themselves, so
    // this engine gain is the one place the saved volume setting takes effect.
    void ApplyVolume(uint8_t percent) override {
        if (audio_ != nullptr) {
            audio_->SetMasterVolumePercent(percent);
        }
    }

   private:
    detail::SensecapWatcherState& state_;
    BrightnessSetter brightness_setter_{};
    void* brightness_context_{};
    audio::AudioEngine* audio_{};
};

class SensecapWatcherBoard final : public Board, public device::Power {
   public:
    SensecapWatcherBoard()
        : state_(TaskState(input_state_)),
          graphics_context_{.engine = &state_.guest_graphics, .hooks = state_.ui.GraphicsHooks()},
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          presentation_(
              state_,
              [](void* context, int percent) {
                  return static_cast<board_detail::DisplayHardware*>(context)->SetBrightness(percent);
              },
              &hardware_),
          system_ui_(state_.ui, presentation_) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        ESP_RETURN_ON_FALSE(memory::IsInternalObject(*this), ESP_ERR_INVALID_STATE, kTag,
                            "Board control objects must reside in internal RAM");
        // The Host's master volume reaches the mix through the presentation, so
        // the engine is bound before anything can play.
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_LOGI(kTag, "initializing SenseCAP Watcher ESP32-S3 (412x412 SPD2010)");
        // Every peripheral rail is switched by this expander, so the sequence
        // runs before the panel is powered. Input and audio follow as each one
        // is brought up.
        ESP_RETURN_ON_ERROR(power_.Initialize(), kTag, "initialize PCA95xx power sequencing failed");
        ESP_RETURN_ON_ERROR(hardware_.Initialize(), kTag, "initialize SPD2010 display failed");
        ESP_RETURN_ON_ERROR(RegisterLvglDisplay(state_, hardware_), kTag, "register LVGL display failed");
        // The shadow wraps the adapter's flush callback, so it has to run after
        // the display is registered. It is optional: a board that cannot spare
        // the PSRAM keeps running, only screenshots are unavailable.
        const esp_err_t shadow_status = state_.display_shadow.Initialize(state_.display);
        if (shadow_status != ESP_OK) {
            ESP_LOGW(kTag, "display shadow unavailable, screenshots are off: %s", esp_err_to_name(shadow_status));
        }
        // Touch and the rotary knob are optional. A controller that does not
        // answer must not take the display down with it, so each one is
        // reported and skipped instead of aborting the board, and the Host UI
        // simply starts with fewer input sources.
        // The shared I2C executor serialises the codec control writes and the
        // touch reads, so it is created before either of them and independently
        // of whether the touch panel answers.
        const esp_err_t executor_status = state_.i2c_executor.Initialize();
        if (executor_status != ESP_OK) {
            ESP_LOGW(kTag, "shared I2C executor unavailable: %s", esp_err_to_name(executor_status));
        }
        bool touch_ready = false;
        const esp_err_t touch_status = touch_hardware_.Initialize();
        if (touch_status != ESP_OK) {
            ESP_LOGW(kTag, "touch controller unavailable for this boot: %s", esp_err_to_name(touch_status));
        } else if (executor_status == ESP_OK) {
            const esp_err_t bind_status = state_.touch_input.Initialize(touch_hardware_.Touch(), state_.i2c_executor);
            if (bind_status != ESP_OK) {
                ESP_LOGW(kTag, "bind touch input failed: %s", esp_err_to_name(bind_status));
            } else {
                touch_ready = true;
            }
        }
        // Audio is optional the same way, and it is the one service a game
        // cannot do without: with no published sink the Host falls back to its
        // unavailable default and every tone and PCM stream is dropped.
        esp_err_t audio_status = ESP_ERR_INVALID_STATE;
        if (executor_status == ESP_OK) {
            audio_status = audio_output_.Configure(power_.ControlBus(), state_.i2c_executor);
            if (audio_status != ESP_OK) {
                ESP_LOGW(kTag, "ES8311 codec unavailable for this boot: %s", esp_err_to_name(audio_status));
            }
        }
        // The battery monitor is optional in the same way. Its sense line is an
        // ADC channel, but charging and presence are expander inputs, so it
        // samples on the shared executor; without either the Host keeps its
        // unavailable default instead of showing an invented level.
        esp_err_t battery_status = ESP_ERR_INVALID_STATE;
        if (executor_status == ESP_OK) {
            battery_status = state_.battery.Initialize(power_, state_.i2c_executor);
            if (battery_status != ESP_OK) {
                ESP_LOGW(kTag, "battery monitor unavailable for this boot: %s", esp_err_to_name(battery_status));
            }
        }
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, nullptr), kTag,
                            "initialize RGB565 Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        // Install rotary navigation before the UI builds its screens: LVGL adds
        // each focusable object to the default group as it is constructed, so
        // every screen - including ones built later - takes part without having
        // to know that the knob exists.
        const esp_err_t encoder_status = encoder_router_.InitializeLocked(state_.display);
        const esp_err_t ui_status = state_.ui.InitializeLocked(state_.display);
        esp_lv_adapter_unlock();
        if (encoder_status != ESP_OK) {
            ESP_LOGW(kTag, "rotary screen navigation unavailable: %s", esp_err_to_name(encoder_status));
        }
        ESP_RETURN_ON_ERROR(ui_status, kTag, "initialize 412x412 Host UI failed");
        ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), kTag, "start LVGL adapter failed");
        if (touch_ready) {
            const esp_err_t touch_start = state_.touch_input.Start(state_.display);
            if (touch_start != ESP_OK) {
                ESP_LOGW(kTag, "polled touch input did not start: %s", esp_err_to_name(touch_start));
            }
        }
        const esp_err_t knob_status = knob_.Initialize(state_.ui.Input());
        if (knob_status != ESP_OK) {
            ESP_LOGW(kTag, "rotary knob unavailable for this boot: %s", esp_err_to_name(knob_status));
        } else {
            navigation_context_.router = &encoder_router_;
            navigation_context_.system_ui = &system_ui_;
            encoder_router_.SetBackSink(RunBackAction, &navigation_context_);
            encoder_router_.SetScreenSinks(RouteKnobRotation, RouteKnobConfirm, &navigation_context_);
            knob_.SetNavigationSink(RouteKnobNavigation, &navigation_context_);
        }
        ESP_RETURN_ON_ERROR(hardware_.SetBrightness(detail::kStartupBrightnessPercent), kTag,
                            "set startup brightness failed");

        static MICROPIXEL_EXT_RAM_BSS BoardRegistration registration{{
            .board = "SenseCAP Watcher",
            .host_chip = "ESP32-S3",
            .firmware_target = "sensecap-watcher",
            .wifi_coprocessor = "Native ESP32-S3",
            .touch_controller = "SPD2010",
            .display =
                {
                    .driver = "SPD2010",
                    .interface = "QSPI 40 MHz",
                    .pixel_format = "RGB565",
                    .width_pixels = static_cast<uint32_t>(board_detail::board::kDisplayWidth),
                    .height_pixels = static_cast<uint32_t>(board_detail::board::kDisplayHeight),
                    // The cover is round, so the corners are not visible.
                    .safe_area =
                        {
                            .top_pixels = detail::kSafeAreaInsetPixels,
                            .right_pixels = detail::kSafeAreaInsetPixels,
                            .bottom_pixels = detail::kSafeAreaInsetPixels,
                            .left_pixels = detail::kSafeAreaInsetPixels,
                        },
                },
            .graphics_acceleration = "CPU only; QSPI panel with internal GRAM",
        }};
        registration.SetInput(state_.ui.Input());
        registration.SetGraphics(graphics_);
        if (audio_status == ESP_OK) {
            // This board has one codec: an ES8311 DAC. The microphone is a
            // separate ES7243E ADC sharing the same I2S bus, and the device
            // contracts have no capture path yet, so only output is published.
            registration.SetAudioOutput(audio_output_, audio_output_.SampleRate());
        }
        // The Watcher's radio is the ESP32-S3's own, driven through the same
        // shared NativeWifiRadio + WifiManager path as every other S3 board, so
        // nothing here is board-specific beyond publishing the service. Without
        // this the Host falls back to its UnavailableWifi default and every
        // Wi-Fi surface reports "unavailable".
        registration.SetWifi(wifi_);
        registration.SetPower(*this);
        // The board has no fuel gauge, so the level is a curve over the pack
        // voltage; publishing it is what puts the battery on the status bar and
        // in the Hall instead of the Host's unavailable default.
        if (battery_status == ESP_OK) {
            registration.SetBattery(state_.battery);
        }
        registration.SetSystemUi(system_ui_);
        // Starting the development bridge also starts the transport it is given,
        // so the byte stream is owned in one place: MPX1 and the screenshot and
        // touch commands share it exactly as the USB boards share theirs.
        const esp_err_t local_control_status = state_.development_display.Start(
            state_.ui.Input(), state_.local_control, static_cast<uint32_t>(detail::kWidth),
            static_cast<uint32_t>(detail::kHeight), transports::DevelopmentCaptureHook::For(presentation_));
        if (local_control_status == ESP_OK) {
            registration.SetLocalControl(state_.local_control);
        } else {
            ESP_LOGW(kTag, "local control unavailable on this boot: %s", esp_err_to_name(local_control_status));
        }
        ESP_LOGI(kTag,
                 "ready: SPD2010 panel + touch, rotary knob, native Wi-Fi, audio=%s, battery=%s, power rails up, "
                 "Host UI in place",
                 audio_status == ESP_OK ? "ES8311" : "off", battery_status == ESP_OK ? "ADC" : "off");
        return context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        state_.guest_graphics.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

    void SetPowerButtonSink(device::PowerButtonSink, void*) override {
        // The knob's centre switch is the board's only button, and its short
        // presses are UI gestures rather than a wake or sleep request, so no
        // press is offered to the Host as a power button. Holds still reach the
        // Host through SetPowerOffButtonSink below. The M5Stack CoreS3 board
        // leaves this same hook unused for the same reason.
    }

    void SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context) override {
        knob_.SetLongPressSink(sink, context);
    }

    [[nodiscard]] device::IdlePowerAction GetIdlePowerAction() const override {
        // Nothing here can yet wake the SoC out of light sleep, so the idle
        // timeout stays disabled instead of asking the Host to cut power behind
        // the user's back. This is also what the unavailable default reports, so
        // publishing this service does not change idle behaviour.
        return device::IdlePowerAction::kDisabled;
    }

    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return std::unexpected(device::PowerError::kSleepRejected);
    }

    [[noreturn]] void PowerOff() override {
        // The Host calls this only after its own shutdown sequence has run, so
        // dropping the system rail is the last thing the SoC does. P1.2 is both
        // that rail and the board's power latch: the switch re-arms it in
        // hardware, which is what lets a hold bring the device back up. Retry
        // rather than return, as the contract requires, in case a write is lost
        // while the rail is still held up.
        for (;;) {
            const esp_err_t status = power_.PowerOff();
            if (status != ESP_OK) {
                ESP_LOGE(kTag, "power-off rail cut failed: %s", esp_err_to_name(status));
            }
            vTaskDelay(pdMS_TO_TICKS(kPowerOffRetryDelayMs));
        }
    }

   private:
    static detail::SensecapWatcherState& TaskState(detail::SensecapWatcherInputState& input) {
        static MICROPIXEL_EXT_RAM_BSS detail::SensecapWatcherState state(input);
        return state;
    }

    // Declaration order is construction order: presentation binds the display
    // object and the knob binds the power object, so both must come first.
    detail::SensecapWatcherInputState input_state_{};
    detail::SensecapWatcherState& state_;
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    board_detail::DisplayHardware hardware_{};
    board_detail::TouchHardware touch_hardware_{};
    board_detail::I2sAudioSink audio_output_{};
    board_detail::BoardPower power_;
    SensecapWatcherPresentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
    board_detail::KnobInput knob_{{.phase_a = board_detail::board::kKnobPhaseA,
                                   .phase_b = board_detail::board::kKnobPhaseB,
                                   .read_pressed = ReadKnobSwitch,
                                   .read_pressed_context = &power_,
                                   .log_tag = kTag,
                                   .task_name = "watcher_knob",
                                   .task_core = task_policy::kSystemCore}};
    // Rotary navigation is opt-in, so only a board with a rotary control installs
    // it; the shared UI and every other board are unaffected.
    platform::lvgl::HostEncoderRouter encoder_router_{};
    KnobNavigationContext navigation_context_{};
    wifi::NativeWifiRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
};

}  // namespace

Board& ConfiguredBoard() {
    static SensecapWatcherBoard board;
    return board;
}

}  // namespace micropixel::platform
