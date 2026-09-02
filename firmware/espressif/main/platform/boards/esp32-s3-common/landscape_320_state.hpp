#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "host/ui/lvgl/square_common/profiles/landscape_320.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/boards/esp32-s3-common/display_shadow.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/transports/usb_serial_jtag_local_control.hpp"

namespace micropixel::platform::esp32_s3_common {

namespace ui_profile = host_ui::lvgl::square_common::profiles::landscape_320;

inline constexpr int32_t kWidth = 320;
inline constexpr int32_t kHeight = 240;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);
inline constexpr uint8_t kMaxTouchPoints = MICROPIXEL_MAX_TOUCH_POINTS;
inline constexpr int kLvglTaskCore = 1;
inline constexpr graphics::SurfacePixelFormat kGuestSurfaceFormat = graphics::SurfacePixelFormat::kRgb565;

struct Landscape320State final {
    esp_lcd_panel_handle_t panel{};
    esp_lcd_panel_io_handle_t panel_io{};
    esp_lcd_panel_io_handle_t touch_io{};
    esp_lcd_touch_handle_t touch{};
    lv_display_t* display{};
    DisplayShadow display_shadow{kWidth, kHeight};
    buses::I2cExecutor i2c_executor{};
    input::EspLcdTouchInput touch_input{kWidth, kHeight, kMaxTouchPoints};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts, kGuestSurfaceFormat};
    host_ui::lvgl::square_common::StaticStatusLayerTransition status_transition{};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, status_transition,
                                                         ui_profile::kSystemUiProfile};
    transports::UsbSerialJtagLocalControl local_control{};
    transports::DevelopmentDisplayControl development_display{};
    const char* panel_name{"Unknown"};
    const char* touch_name{"Unknown"};
};

}  // namespace micropixel::platform::esp32_s3_common
