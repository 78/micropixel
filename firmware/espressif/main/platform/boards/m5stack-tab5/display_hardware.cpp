// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/display_hardware.hpp"

#include <cstddef>

#include "esp_check.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lv_adapter.h"
#include "platform/boards/m5stack-tab5/board_config.hpp"
#include "platform/boards/m5stack-tab5/tab5_state.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "work/task_policy.hpp"

// The vendor initialization sequence is a C translation unit because its
// command payloads use compound literals.
extern "C" {
extern const ili9881c_lcd_init_cmd_t tab5_lcd_ili9881c_specific_init_code_default[];
extern const size_t tab5_lcd_ili9881c_specific_init_code_default_count;
}

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr char kTag[] = "tab5_display";
constexpr uint32_t kFramebufferCount = 2U;

// The DPI frame buffer the panel is scanning out. While a compositor owns the
// adapter's dummy draw (Direct Surface or App Surface scanout) the adapter
// hands out the off-screen buffer, so the displayed one is the other; without
// a compositor LVGL's active buffer is the one being flushed into.
uint8_t* DisplayedFramebuffer(lv_display_t* display, void* const* panel_buffers) {
    auto* free_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display));
    if (free_buffer == nullptr && lv_display_get_render_mode(display) == LV_DISPLAY_RENDER_MODE_DIRECT &&
        lv_display_is_double_buffered(display)) {
        const lv_draw_buf_t* active = lv_display_get_buf_active(display);
        if (active != nullptr) {
            free_buffer = static_cast<uint8_t*>(active->data);
        }
    }
    if (free_buffer == panel_buffers[0]) {
        return static_cast<uint8_t*>(panel_buffers[1]);
    }
    if (free_buffer == panel_buffers[1]) {
        return static_cast<uint8_t*>(panel_buffers[0]);
    }
    return nullptr;
}

}  // namespace

esp_err_t InitializeDisplayHardware(Tab5State& state) {
    if (state.panel != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_ldo_channel_config_t ldo_config{};
    ldo_config.chan_id = board::kDsiLdoChannel;
    ldo_config.voltage_mv = board::kDsiLdoMillivolts;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &state.dsi_ldo), kTag,
                        "acquire DSI PHY LDO channel %d failed", board::kDsiLdoChannel);

    esp_lcd_dsi_bus_config_t bus_config{};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = board::kDsiLaneBitRateMbps;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &state.dsi_bus), kTag, "create MIPI-DSI bus failed");

    esp_lcd_dbi_io_config_t dbi_config{};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(state.dsi_bus, &dbi_config, &state.panel_io), kTag,
                        "create DBI panel IO failed");

    esp_lcd_dpi_panel_config_t dpi_config{};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = board::kDpiClockMHz;
    dpi_config.num_fbs = static_cast<uint8_t>(board::kDisplayFramebufferCount);
    dpi_config.video_timing.h_size = board::kDisplayWidth;
    dpi_config.video_timing.v_size = board::kDisplayHeight;
    dpi_config.video_timing.hsync_back_porch = board::kPanelHsyncBackPorch;
    dpi_config.video_timing.hsync_pulse_width = board::kPanelHsyncPulseWidth;
    dpi_config.video_timing.hsync_front_porch = board::kPanelHsyncFrontPorch;
    dpi_config.video_timing.vsync_back_porch = board::kPanelVsyncBackPorch;
    dpi_config.video_timing.vsync_pulse_width = board::kPanelVsyncPulseWidth;
    dpi_config.video_timing.vsync_front_porch = board::kPanelVsyncFrontPorch;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;

    ili9881c_vendor_config_t vendor_config{};
    vendor_config.init_cmds = tab5_lcd_ili9881c_specific_init_code_default;
    vendor_config.init_cmds_size = static_cast<uint16_t>(tab5_lcd_ili9881c_specific_init_code_default_count);
    vendor_config.mipi_config.dsi_bus = state.dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.mipi_config.lane_num = 2;

    esp_lcd_panel_dev_config_t panel_config{};
    // LCD reset is driven through IO expander 1, not a GPIO.
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9881c(state.panel_io, &panel_config, &state.panel), kTag,
                        "create ILI9881C panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(state.panel), kTag, "reset panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(state.panel), kTag, "initialize panel failed");
    return esp_lcd_panel_disp_on_off(state.panel, true);
}

esp_err_t RegisterLvglDisplay(Tab5State& state) {
    if (state.panel == nullptr || state.panel_io == nullptr || state.display != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_config{};
        adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
        adapter_config.stack_in_psram = false;
        adapter_config.task_priority = task_policy::kDisplayPriority;
        adapter_config.task_core_id = task_policy::kSystemCore;
        adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_MONOTONIC;
        adapter_config.task_min_delay_ms = portTICK_PERIOD_MS;
        adapter_config.task_max_delay_ms = 120U * 1000U;
        // Automatic light sleep stays off until display and Wi-Fi retention
        // have been validated on this board; the USB console must remain
        // responsive during bring-up as well.
        adapter_config.auto_sleep.enable = false;
        adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
        adapter_config.auto_sleep.idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS;
        const esp_err_t status = esp_lv_adapter_init(&adapter_config);
        if (status != ESP_OK) {
            return status;
        }
    }

    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.panel;
    display_config.panel_io = state.panel_io;
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI;
    // The panel scans out its native 720x1280 frame, which is also the logical
    // canvas: no rotation pipeline runs on this board.
    display_config.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.profile.hor_res = board::kDisplayWidth;
    display_config.profile.ver_res = board::kDisplayHeight;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#ifdef CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.enable_ppa_accel = true;
    display_config.profile.require_double_buffer = true;
    display_config.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
    display_config.te_sync = {};
    display_config.te_sync.gpio_num = -1;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    // Lend the panel's framebuffers to the graphics engine before the LVGL task
    // starts: a Guest that owns the panel is scanned out of them directly and
    // Host overlays are blended by the presenter, instead of every frame going
    // through the LVGL composited path.
    state.framebuffers.Bind(state.display, state.panel);
    return ESP_OK;
}

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureDisplayedFrame(lv_display_t* display,
                                                                                    esp_lcd_panel_handle_t panel,
                                                                                    uint32_t width, uint32_t height) {
    if (display == nullptr || panel == nullptr || width == 0U || height == 0U) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void* framebuffers[kFramebufferCount]{};
    const esp_err_t status =
        esp_lcd_dpi_panel_get_frame_buffer(panel, kFramebufferCount, &framebuffers[0], &framebuffers[1]);
    std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> result =
        std::unexpected(host_ui::SystemUiError::kUnavailable);
    if (status == ESP_OK && framebuffers[0] != nullptr) {
        // A Guest scanned out of a framebuffer flips between the two, so read
        // the one on the panel rather than a fixed index.
        const uint8_t* displayed = DisplayedFramebuffer(display, framebuffers);
        if (displayed == nullptr) {
            displayed = static_cast<const uint8_t*>(framebuffers[0]);
        }
        static constexpr bool kReady = true;
        result = lvgl::CaptureScreenJpeg(display, width, height,
                                         {.pixels = displayed,
                                          .stride = width * 2U,
                                          .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                          .ready = &kReady});
    }
    esp_lv_adapter_unlock();
    return result;
}

}  // namespace micropixel::platform::m5stack_tab5
