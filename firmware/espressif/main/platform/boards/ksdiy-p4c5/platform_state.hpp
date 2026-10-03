#pragma once

#include <cstdint>

#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/ksdiy-p4c5/battery_peripheral.hpp"
#include "platform/boards/ksdiy-p4c5/board_config.hpp"
#include "platform/boards/ksdiy-p4c5/board_io.hpp"
#include "platform/boards/ksdiy-p4c5/display_pipeline.hpp"
#include "platform/boards/ksdiy-p4c5/sensor_peripheral.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/transports/usb_serial_jtag_local_control.hpp"
#include "soc/soc_caps.h"
#include "work/task_policy.hpp"

#if CONFIG_MICROPIXEL_KSDIY_P4C5_LANDSCAPE
#include "host/ui/lvgl/square_common/profiles/landscape_800x480.hpp"
#else
#include "host/ui/lvgl/square_common/profiles/portrait_480x800.hpp"
#endif

namespace micropixel::platform::ksdiy_p4c5::detail {

#if CONFIG_MICROPIXEL_KSDIY_P4C5_LANDSCAPE
namespace ui_profile = host_ui::lvgl::square_common::profiles::landscape_800x480;
#else
namespace ui_profile = host_ui::lvgl::square_common::profiles::portrait_480x800;
#endif

inline constexpr char kTag[] = "micropixel_platform";
inline constexpr int kWidth = board::kDisplayWidth;
inline constexpr int kHeight = board::kDisplayHeight;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);
inline constexpr BaseType_t kLvglTaskCore = task_policy::kSystemCore;
inline constexpr uint32_t kRefreshPeriodMs = 1000;
inline constexpr uint32_t kLvglIdleTimeoutMs = 1000;
inline constexpr uint32_t kLvglMaximumWaitMs = 120U * 1000U;
inline constexpr uint32_t kLvglTaskMinDelayMs = portTICK_PERIOD_MS;
static_assert(SOC_PPA_SUPPORTED);
// Portrait renders directly into the two DPI framebuffers. Landscape renders
// 800x480 partial buffers that the adapter rotates with the PPA into three
// portrait framebuffers.
inline constexpr esp_lv_adapter_rotation_t kRotation =
    board::kLandscape ? ESP_LV_ADAPTER_ROTATE_90 : ESP_LV_ADAPTER_ROTATE_0;
inline constexpr esp_lv_adapter_tear_avoid_mode_t kTearAvoidMode =
    board::kLandscape ? ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL : ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
inline constexpr const char* kTearAvoidModeName = board::kLandscape ? "triple-partial/rotate-90" : "double-direct";

// Large task-only state lives in PSRAM. The I2C executor and touch input stay
// in the internal-RAM Board because the touch ISR reaches them.
struct KsdiyP4c5BoardState final {
    KsdiyP4c5BoardState(buses::I2cExecutor& executor, input::EspLcdTouchInput& touch)
        : i2c_executor(executor), touch_input(touch) {}

    BoardIo board_io{};
    DisplayPipeline display_pipeline{board_io};
    buses::I2cExecutor& i2c_executor;
    input::EspLcdTouchInput& touch_input;
    BatteryPeripheral battery{};
    SensorPeripheral sensors{};
    audio::AudioEngine* audio_engine{};
    lv_display_t* display{};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts};
    // No Guest<->Hall transition compositor: status dialogs and action sheets
    // are presented as static LVGL frames.
    host_ui::lvgl::square_common::StaticStatusLayerTransition status_transition{};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, status_transition,
                                                         ui_profile::kSystemUiProfile};
    transports::UsbSerialJtagLocalControl local_control{};
    transports::DevelopmentDisplayControl development_display{};
};

}  // namespace micropixel::platform::ksdiy_p4c5::detail
