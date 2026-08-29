#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_async_color_convert.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "host/ui/lvgl/square_common/profiles/square_480.hpp"
#include "lvgl.h"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "platform/boards/esp-mosaico/display/panel_transition_compositor.hpp"
#include "platform/boards/esp-mosaico/usb_cdc_console.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/transports/tinyusb_cdc_local_control.hpp"

namespace micropixel::platform::esp_mosaico::detail {

namespace ui_profile = host_ui::lvgl::square_common::profiles::square_480;

inline constexpr char kTag[] = "esp_mosaico";
inline constexpr int32_t kWidth = board::kDisplayWidth;
inline constexpr int32_t kHeight = board::kDisplayHeight;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);
inline constexpr int kLvglTaskCore = 1;
inline constexpr uint32_t kTransitionAlignment = 128U;
inline constexpr uint32_t kDisplayFrameStride = static_cast<uint32_t>(kWidth) * 2U;
inline constexpr uint32_t kDisplayFrameBytes = kDisplayFrameStride * static_cast<uint32_t>(kHeight);
inline constexpr uint32_t kDisplayFrameAllocationBytes =
    (kDisplayFrameBytes + kTransitionAlignment - 1U) / kTransitionAlignment * kTransitionAlignment;
inline constexpr bool kEnableLvglAdapterPpaAccel = CONFIG_MICROPIXEL_LVGL_PPA_ACCEL;

struct MosaicoBoardState final {
    buses::I2cExecutor i2c_executor{};
    i2c_master_bus_handle_t i2c_bus{};
    esp_lcd_panel_handle_t panel{};
    esp_lcd_panel_io_handle_t panel_io{};
    esp_lcd_panel_io_handle_t touch_io{};
    lv_display_t* display{};
    async_color_convert_handle_t shadow_copy_dma2d{};
    lv_display_flush_cb_t adapter_flush_cb{};
    uint8_t* displayed_shadow{};
    std::array<bool, static_cast<size_t>(kHeight)> displayed_shadow_rows{};
    esp_lcd_touch_handle_t touch{};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts};
    PanelTransitionCompositor panel_transition{};
    input::EspLcdTouchInput touch_input{kWidth, kHeight};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, panel_transition,
                                                         ui_profile::kSystemUiProfile};
    transports::TinyUsbCdcLocalControl local_control{};
    transports::DevelopmentDisplayControl development_display{};
    uint8_t* guest_transition_intermediate{};
    uint8_t* guest_snapshot_cover{};
    PanelTransitionRect guest_transition_card{};
    bool guest_snapshot_in_hall{};
    bool displayed_shadow_valid{};
};

}  // namespace micropixel::platform::esp_mosaico::detail
