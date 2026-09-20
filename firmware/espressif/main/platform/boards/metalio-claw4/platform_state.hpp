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
#include "platform/boards/metalio-claw4/haptic_actuator.hpp"
#include "platform/boards/metalio-claw4/tca9555_power_key.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/sensors/qmc6309.hpp"
#include "platform/drivers/sensors/sc7a20htr.hpp"
#include "platform/gpio/esp_gpio_peripheral.hpp"
#include "platform/haptics/timed_haptics_peripheral.hpp"
#include "platform/input/gt911_input.hpp"
#include "platform/lvgl/display/system_transition_compositor.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/memory/graphics_buffer_alignment.hpp"
#include "platform/sensors/polled_vector_sensor_peripheral.hpp"
#include "soc/soc_caps.h"
#include "work/task_policy.hpp"

namespace micropixel::platform::metalio_claw4::detail {

namespace ui_profile = host_ui::lvgl::square_common::profiles::square_720;

inline constexpr char kTag[] = "micropixel_platform";
inline constexpr char kSensorTag[] = "micropixel_sensors";
inline constexpr int kWidth = board::kDisplayWidth;
inline constexpr int kHeight = board::kDisplayHeight;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);
inline constexpr BaseType_t kLvglTaskCore = task_policy::kSystemCore;
inline constexpr uint32_t kRefreshPeriodMs = 1000;
inline constexpr uint32_t kLvglIdleTimeoutMs = 1000;
inline constexpr uint32_t kLvglMaximumWaitMs = 120U * 1000U;
inline constexpr uint32_t kLightSleepEntryAttempts = 3U;
inline constexpr uint32_t kLvglTaskMinDelayMs = portTICK_PERIOD_MS;
inline constexpr uint32_t kGuestTransitionStride = static_cast<uint32_t>(kWidth) * 3U;
inline constexpr uint32_t kGuestTransitionBytes = kGuestTransitionStride * static_cast<uint32_t>(kHeight);
inline constexpr uint32_t kPpaBufferAlignment = memory::kGraphicsBufferAlignment;
static_assert(SOC_PPA_SUPPORTED);
inline constexpr bool kEnablePpaAccel = true;
inline constexpr esp_lv_adapter_tear_avoid_mode_t kTearAvoidMode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
inline constexpr const char* kTearAvoidModeName = "double-direct";

// Large task-only state lives in PSRAM. Input/GPIO controls and the executor
// remain owned by the internal-RAM Board; cache-off ISR paths cannot access it.
struct MetalioClaw4BoardState final {
    MetalioClaw4BoardState(buses::I2cExecutor& executor, gpio::EspGpioPeripheral& gpio_control,
                           input::Gt911Input& touch)
        : i2c_executor(executor), gpio(gpio_control), touch_input(touch) {}

    BoardIo board_io{kWidth, kHeight};
    MetalioClaw4DisplayPipeline display_pipeline{board_io, kWidth, kHeight};
    buses::I2cExecutor& i2c_executor;
    BatteryPeripheral battery{};
    drivers::Sc7a20htr acceleration{};
    drivers::Qmc6309 magnetic_field{};
    std::array<sensors::PolledVectorSensorPeripheral::Channel, 2> sensor_channels{{
        {board::kAccelerationChannel, MICROPIXEL_SENSOR_ACCELERATION, acceleration, "micropixel_accel"},
        {board::kMagneticFieldChannel, MICROPIXEL_SENSOR_MAGNETIC_FIELD, magnetic_field, "micropixel_magnet"},
    }};
    sensors::PolledVectorSensorPeripheral sensors{sensor_channels, kSensorTag};
    gpio::EspGpioPeripheral& gpio;
    haptics::TimedHapticsPeripheral haptics{ConfiguredHapticActuator(), MICROPIXEL_HAPTICS_CAP_VARIABLE_STRENGTH};
    audio::AudioEngine* audio_engine{};
    lv_display_t* display{};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts};
    lvgl::SystemTransitionCompositor system_transition{};
    Tca9555PowerKey power_key{};
    input::Gt911Input& touch_input;
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, system_transition,
                                                         ui_profile::kSystemUiProfile};
    uint8_t* guest_snapshot_pixels{};
    uint8_t* guest_transition_pixels{};
    bool guest_snapshot_in_hall{};
};

}  // namespace micropixel::platform::metalio_claw4::detail
