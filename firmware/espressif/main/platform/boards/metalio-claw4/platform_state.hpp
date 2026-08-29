#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/profiles/square_720.hpp"
#include "lvgl.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/metalio-claw4/battery_peripheral.hpp"
#include "platform/boards/metalio-claw4/board_config.hpp"
#include "platform/boards/metalio-claw4/board_io.hpp"
#include "platform/boards/metalio-claw4/display/display_pipeline.hpp"
#include "platform/boards/metalio-claw4/gpio_peripheral.hpp"
#include "platform/boards/metalio-claw4/haptic_actuator.hpp"
#include "platform/boards/metalio-claw4/sensor_peripheral.hpp"
#include "platform/boards/metalio-claw4/tca9555_power_key.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/haptics/timed_haptics_peripheral.hpp"
#include "platform/input/gt911_input.hpp"
#include "platform/lvgl/display/system_transition_compositor.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"

namespace micropixel::platform::metalio_claw4::detail {

namespace ui_profile = host_ui::lvgl::square_common::profiles::square_720;

inline constexpr char kTag[] = "micropixel_platform";
inline constexpr int kWidth = board::kDisplayWidth;
inline constexpr int kHeight = board::kDisplayHeight;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);
inline constexpr BaseType_t kLvglTaskCore = 1;
inline constexpr uint32_t kRefreshPeriodMs = 1000;
inline constexpr uint32_t kLvglIdleTimeoutMs = 1000;
inline constexpr uint32_t kLvglMaximumWaitMs = 120U * 1000U;
inline constexpr uint32_t kLightSleepEntryAttempts = 3U;
inline constexpr uint32_t kLvglTaskMinDelayMs = portTICK_PERIOD_MS;
inline constexpr uint32_t kGuestTransitionStride = static_cast<uint32_t>(kWidth) * 3U;
inline constexpr uint32_t kGuestTransitionBytes = kGuestTransitionStride * static_cast<uint32_t>(kHeight);
inline constexpr uint32_t kPpaBufferAlignment = 128U;
inline constexpr bool kEnablePpaAccel = CONFIG_MICROPIXEL_LVGL_PPA_ACCEL;
inline constexpr esp_lv_adapter_tear_avoid_mode_t kTearAvoidMode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
inline constexpr const char* kTearAvoidModeName = "double-direct";

// Board-owned objects live together so presentation callbacks never reach
// through process-global board aliases. This is the private Board composition
// boundary exposed upward only through BoardRegistration.
struct MetalioClaw4BoardState final {
    BoardIo board_io{kWidth, kHeight};
    MetalioClaw4DisplayPipeline display_pipeline{board_io, kWidth, kHeight};
    buses::I2cExecutor i2c_executor{};
    BatteryPeripheral battery{};
    SensorPeripheral sensors{};
    GpioPeripheral gpio{};
    haptics::TimedHapticsPeripheral haptics{ConfiguredHapticActuator(), MICROPIXEL_HAPTICS_CAP_VARIABLE_STRENGTH};
    audio::AudioEngine* audio_engine{};
    lv_display_t* display{};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts};
    lvgl::SystemTransitionCompositor system_transition{};
    Tca9555PowerKey power_key{};
    input::Gt911Input touch_input{kWidth, kHeight, board::kTouchInterrupt};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, system_transition,
                                                         ui_profile::kSystemUiProfile};
    uint8_t* guest_snapshot_pixels{};
    uint8_t* guest_transition_pixels{};
    bool guest_snapshot_in_hall{};
};

}  // namespace micropixel::platform::metalio_claw4::detail
