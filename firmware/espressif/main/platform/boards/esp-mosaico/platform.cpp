#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_async_color_convert.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/esp-mosaico/battery_peripheral.hpp"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "platform/boards/esp-mosaico/display/brightness_controller.hpp"
#include "platform/boards/esp-mosaico/display/panel_transition_compositor.hpp"
#include "platform/boards/esp-mosaico/haptic_actuator.hpp"
#include "platform/boards/esp-mosaico/i2s_audio_sink.hpp"
#include "platform/boards/esp-mosaico/platform_state.hpp"
#include "platform/boards/esp-mosaico/power_controller.hpp"
#include "platform/boards/esp-mosaico/presentation.hpp"
#include "platform/boards/esp-mosaico/usb_cdc_console.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/lvgl/display/system_transition_timeline.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/host_pointer_router.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/random/system_random.hpp"
#include "platform/transports/tinyusb_cdc_local_control.hpp"
#include "platform/wifi/native_wifi_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "src/display/lv_display_private.h"
#include "work/background_executor.hpp"

namespace micropixel::platform {
namespace {

namespace board_detail = esp_mosaico::detail;

board_detail::MosaicoBoardState* g_display_flush_state{};

void CaptureDisplayedShadowFlush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    board_detail::MosaicoBoardState* state = g_display_flush_state;
    if (state != nullptr && state->display == display && state->displayed_shadow != nullptr && area != nullptr &&
        pixels != nullptr && area->x1 >= 0 && area->y1 >= 0 && area->x2 < board_detail::kWidth &&
        area->y2 < board_detail::kHeight) {
        const uint32_t area_width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
        const uint32_t area_height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
        const uint32_t source_stride = lv_display_get_buf_active(display)->header.stride;
        async_color_convert_request_t copy{};
        copy.src_buffer = pixels;
        copy.src_stride = source_stride / 2U;
        copy.src_height = area_height;
        copy.dst_buffer = state->displayed_shadow;
        copy.dst_stride = static_cast<uint32_t>(board_detail::kWidth);
        copy.dst_height = static_cast<uint32_t>(board_detail::kHeight);
        copy.dst_x = static_cast<uint32_t>(area->x1);
        copy.dst_y = static_cast<uint32_t>(area->y1);
        copy.copy_width = area_width;
        copy.copy_height = area_height;
        copy.src_color_format = ESP_COLOR_FOURCC_RGB16;
        copy.dst_color_format = ESP_COLOR_FOURCC_RGB16;
        const bool copied = state->shadow_copy_dma2d != nullptr &&
                            esp_color_convert_blocking(state->shadow_copy_dma2d, &copy, -1) == ESP_OK;
        for (int32_t y = area->y1; copied && y <= area->y2; ++y) {
            if (area->x1 == 0 && area->x2 == board_detail::kWidth - 1) {
                state->displayed_shadow_rows[static_cast<size_t>(y)] = true;
            }
        }
        if (!state->displayed_shadow_valid) {
            state->displayed_shadow_valid =
                std::all_of(state->displayed_shadow_rows.begin(), state->displayed_shadow_rows.end(),
                            [](bool row_ready) { return row_ready; });
        }
    }
    if (state != nullptr && state->adapter_flush_cb != nullptr) {
        state->adapter_flush_cb(display, area, pixels);
    } else {
        lv_display_flush_ready(display);
    }
}

void RoundQspiArea(lv_area_t* area, void*) {
    if (area == nullptr) {
        return;
    }
    // CO5300 partial writes require even start coordinates and even extents.
    // X is kept at the stricter four-pixel QSPI/PPA alignment; expand Y to an
    // even start and an even row count so moving objects do not leave their
    // first or last row behind when LVGL invalidates an odd-aligned area.
    area->x1 = std::max<int32_t>(0, area->x1 / 4 * 4);
    area->x2 = std::min<int32_t>(board_detail::kWidth - 1, (area->x2 + 4) / 4 * 4 - 1);
    area->y1 = std::max<int32_t>(0, area->y1 / 2 * 2);
    area->y2 = std::min<int32_t>(board_detail::kHeight - 1, (area->y2 + 2) / 2 * 2 - 1);
}

esp_err_t InitializePanel(board_detail::MosaicoBoardState& state) {
    static const uint8_t kPage20[] = {0x20};
    static const uint8_t kRegister19[] = {0x10};
    static const uint8_t kRegister1c[] = {0xa0};
    static const uint8_t kPage00[] = {0x00};
    static const uint8_t kRegisterC4[] = {0x80};
    static const uint8_t kPixelFormat[] = {0x55};
    static const uint8_t kTearEffect[] = {0x00};
    static const uint8_t kDisplayControl[] = {0x20};
    static const uint8_t kMaximumBrightness[] = {0xff};
    static const uint8_t kHbmBrightness[] = {0xff};
    static const uint8_t kColumnRange[] = {0x00, 0x00, 0x01, 0xdf};
    static const uint8_t kRowRange[] = {0x00, 0x00, 0x01, 0xdf};
    static const co5300_lcd_init_cmd_t kVendorInit[] = {
        {0x11, nullptr, 0, 600},
        {0xfe, kPage20, sizeof(kPage20), 0},
        {0x19, kRegister19, 1, 0},
        {0x1c, kRegister1c, 1, 0},
        {0xfe, kPage00, sizeof(kPage00), 0},
        {0xc4, kRegisterC4, 1, 0},
        {0x3a, kPixelFormat, 1, 0},
        {0x35, kTearEffect, 1, 0},
        {0x53, kDisplayControl, 1, 0},
        {0x51, kMaximumBrightness, 1, 0},
        {0x63, kHbmBrightness, 1, 0},
        {0x2a, kColumnRange, 4, 0},
        {0x2b, kRowRange, 4, 0},
        {0x29, nullptr, 0, 600},
    };

    spi_bus_config_t bus_config{};
    bus_config.data0_io_num = esp_mosaico::board::kDisplayData0;
    bus_config.data1_io_num = esp_mosaico::board::kDisplayData1;
    bus_config.sclk_io_num = esp_mosaico::board::kDisplayClock;
    bus_config.data2_io_num = esp_mosaico::board::kDisplayData2;
    bus_config.data3_io_num = esp_mosaico::board::kDisplayData3;
    bus_config.data4_io_num = -1;
    bus_config.data5_io_num = -1;
    bus_config.data6_io_num = -1;
    bus_config.data7_io_num = -1;
    // Match the DMA transaction ceiling to one LVGL draw buffer. On Mosaico
    // that buffer is a full-height RGB565 surface in PSRAM, and the panel IO
    // consumes it directly without an internal-SRAM bounce allocation.
    bus_config.max_transfer_sz = board_detail::kWidth * CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT *
                                 static_cast<int32_t>(esp_mosaico::board::kDisplayBitsPerPixel) / 8;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(esp_mosaico::board::kDisplaySpiHost, &bus_config, SPI_DMA_CH_AUTO),
                        board_detail::kTag, "initialize CO5300 QSPI bus failed");
    constexpr gpio_num_t kBusPins[] = {
        esp_mosaico::board::kDisplayClock, esp_mosaico::board::kDisplayData0, esp_mosaico::board::kDisplayData1,
        esp_mosaico::board::kDisplayData2, esp_mosaico::board::kDisplayData3,
    };
    for (gpio_num_t pin : kBusPins) {
        ESP_RETURN_ON_ERROR(
            gpio_set_drive_capability(pin, static_cast<gpio_drive_cap_t>(CONFIG_MICROPIXEL_MOSAICO_LCD_QSPI_DRIVE_CAP)),
            board_detail::kTag, "set QSPI GPIO%d drive capability failed", static_cast<int>(pin));
    }

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = esp_mosaico::board::kDisplayChipSelect;
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = esp_mosaico::board::kDisplayPixelClockHz;
    io_config.trans_queue_depth = 10U;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    // PPA and DMA2D finish before a transition frame is submitted. Let QSPI DMA
    // consume that immutable PSRAM frame directly instead of copying it through
    // an internal-SRAM strip and issuing one panel command per strip.
    io_config.flags.psram_dma_direct = true;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(esp_mosaico::board::kDisplaySpiHost), &io_config,
                                 &state.panel_io),
        board_detail::kTag, "create CO5300 panel IO failed");

    co5300_vendor_config_t vendor_config{};
    vendor_config.init_cmds = kVendorInit;
    vendor_config.init_cmds_size = std::size(kVendorInit);
    vendor_config.flags.use_qspi_interface = true;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = esp_mosaico::board::kDisplayReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = esp_mosaico::board::kDisplayBitsPerPixel;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(state.panel_io, &panel_config, &state.panel), board_detail::kTag,
                        "create CO5300 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(state.panel), board_detail::kTag, "reset CO5300 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(state.panel), board_detail::kTag, "initialize CO5300 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(state.panel, true), board_detail::kTag, "turn CO5300 on failed");
    ESP_LOGI(board_detail::kTag, "CO5300 ready: 480x480 QSPI with TE on GPIO%d",
             static_cast<int>(esp_mosaico::board::kDisplayTe));
    return ESP_OK;
}

esp_err_t InitializeDisplay(board_detail::MosaicoBoardState& state) {
    ESP_RETURN_ON_ERROR(InitializePanel(state), board_detail::kTag, "initialize Mosaico panel failed");
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_config{};
        adapter_config.task_stack_size = 10240U;
        adapter_config.task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
        adapter_config.task_core_id = board_detail::kLvglTaskCore;
        adapter_config.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
#ifdef ESP_LV_ADAPTER_HAS_TICK_MODE
        adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_PERIODIC;
#endif
        adapter_config.task_min_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS;
        adapter_config.task_max_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS;
        adapter_config.stack_in_psram = true;
        adapter_config.auto_sleep.enable = false;
        adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
        adapter_config.auto_sleep.idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS;
        ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), board_detail::kTag, "initialize LVGL adapter failed");
    }
    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.panel;
    display_config.panel_io = state.panel_io;
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_config.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.profile.hor_res = board_detail::kWidth;
    display_config.profile.ver_res = board_detail::kHeight;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#ifdef CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.require_double_buffer = true;
    display_config.profile.enable_ppa_accel = board_detail::kEnableLvglAdapterPpaAccel;
    display_config.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    state.displayed_shadow = static_cast<uint8_t*>(heap_caps_aligned_calloc(board_detail::kTransitionAlignment,
                                                                            board_detail::kDisplayFrameAllocationBytes,
                                                                            1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (state.displayed_shadow == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    async_color_convert_config_t dma2d_config{};
    dma2d_config.backlog = 1U;
    dma2d_config.dma_burst_size = 128U;
    ESP_RETURN_ON_ERROR(esp_async_color_convert_install_dma2d(&dma2d_config, &state.shadow_copy_dma2d),
                        board_detail::kTag, "install displayed-shadow DMA2D copier failed");
    state.adapter_flush_cb = state.display->flush_cb;
    if (state.adapter_flush_cb == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    g_display_flush_state = &state;
    lv_display_set_flush_cb(state.display, CaptureDisplayedShadowFlush);
    ESP_RETURN_ON_ERROR(esp_lv_adapter_set_area_rounder_cb(state.display, RoundQspiArea, nullptr), board_detail::kTag,
                        "configure QSPI area alignment failed");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), board_detail::kTag, "start LVGL adapter failed");
    return ESP_OK;
}

esp_err_t InitializeTouch(board_detail::MosaicoBoardState& state) {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.sda_io_num = esp_mosaico::board::kI2cData;
    bus_config.scl_io_num = esp_mosaico::board::kI2cClock;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &state.i2c_bus), board_detail::kTag,
                        "initialize shared Mosaico I2C bus failed");
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST9217_ADDRESS;
    io_config.scl_speed_hz = 400000U;
    io_config.control_phase_bytes = 1U;
    io_config.dc_bit_offset = 0U;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.disable_control_phase = true;
    io_config.transaction_timeout_ms = 100U;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(state.i2c_bus, &io_config, &state.touch_io), board_detail::kTag,
                        "create CST9217 panel IO failed");

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = board_detail::kWidth - 1;
    touch_config.y_max = board_detail::kHeight - 1;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = esp_mosaico::board::kTouchInterrupt;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst9217(state.touch_io, &touch_config, &state.touch), board_detail::kTag,
                        "initialize CST9217 failed");
    ESP_LOGI(board_detail::kTag, "CST9217 ready: interrupt GPIO%d shared I2C0",
             static_cast<int>(esp_mosaico::board::kTouchInterrupt));
    return ESP_OK;
}

class EspMosaicoBoard final : public Board {
   public:
    EspMosaicoBoard()
        : graphics_context_{
              .engine = &state_.guest_graphics,
              .hooks = state_.ui.GraphicsHooks(),
          },
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          presentation_(state_),
          system_ui_(state_.ui, presentation_) {
        state_.ui.BindThemeChanged(
            [](void* context) {
                static_cast<board_detail::MosaicoBoardState*>(context)->panel_transition.ReleaseBackground();
            },
            &state_);
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        presentation_.BindAudioEngine(context.AudioEngine());
        ESP_RETURN_ON_ERROR(InitializeUsbCdcConsole(), board_detail::kTag, "initialize Type-C USB CDC console failed");
        ESP_LOGI(board_detail::kTag, "initializing ESP-Mosaico with official CO5300/CST9217 drivers");
        ESP_RETURN_ON_ERROR(power_.Initialize(), board_detail::kTag, "initialize Mosaico power control failed");
        ESP_RETURN_ON_ERROR(InitializeDisplay(state_), board_detail::kTag, "initialize Mosaico display failed");
        ESP_RETURN_ON_ERROR(
            state_.panel_transition.Initialize(
                state_.display, board_detail::kWidth, board_detail::kHeight,
                host_ui::lvgl::square_common::TransitionProfile(board_detail::ui_profile::kSystemUiProfile.square),
                state_.shadow_copy_dma2d, state_.displayed_shadow, &state_.displayed_shadow_valid),
            board_detail::kTag, "initialize CO5300 direct transition compositor failed");
        ESP_RETURN_ON_ERROR(InitializeTouch(state_), board_detail::kTag, "initialize Mosaico touch failed");
        ESP_RETURN_ON_ERROR(state_.i2c_executor.Initialize(), board_detail::kTag,
                            "start shared Mosaico I2C executor failed");
        ESP_RETURN_ON_ERROR(audio_output_.Configure(state_.i2c_bus, state_.i2c_executor), board_detail::kTag,
                            "configure Mosaico ES8311 audio hardware failed");
        battery_.Initialize(state_.i2c_bus, state_.i2c_executor);
        ESP_RETURN_ON_ERROR(haptics_.Initialize(), board_detail::kTag, "initialize Mosaico vibration motor failed");
        ESP_RETURN_ON_ERROR(state_.touch_input.Initialize(state_.touch, state_.i2c_executor), board_detail::kTag,
                            "bind CST9217 input failed");
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, nullptr), board_detail::kTag,
                            "initialize shared Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        const esp_err_t host_pointer_status = state_.ui.InitializeLocked(state_.display);
        if (host_pointer_status != ESP_OK) {
            esp_lv_adapter_unlock();
            return host_pointer_status;
        }

        esp_lv_adapter_unlock();

        ESP_RETURN_ON_ERROR(state_.touch_input.Start(state_.display), board_detail::kTag,
                            "start interrupt-driven touch failed");
        ESP_RETURN_ON_ERROR(state_.development_display.Start(state_.display, state_.touch_input, state_.local_control,
                                                             board_detail::kWidth, board_detail::kHeight,
                                                             {.pixels = state_.displayed_shadow,
                                                              .stride = board_detail::kWidth * 2U,
                                                              .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                                              .ready = &state_.displayed_shadow_valid}),
                            board_detail::kTag, "start USB screen capture/local control failed");
        ESP_LOGI(board_detail::kTag,
                 "Mosaico HMI ready: 480x480 QSPI display, interrupt-driven touch, shared Guest renderer");
        BoardRegistration registration{{
            .board = "ESP-Mosaico V1.0",
            .host_chip = "ESP32-S31",
            .wifi_coprocessor = "Native ESP32-S31",
            .touch_controller = "CST9217",
            .display =
                {
                    .driver = "CO5300",
                    .interface = "QSPI",
                    .pixel_format = "RGB565",
                    .width_pixels = static_cast<uint32_t>(board_detail::kWidth),
                    .height_pixels = static_cast<uint32_t>(board_detail::kHeight),
                    .refresh_rate_hz = 60U,
                },
        }};
        registration.SetGraphics(graphics_);
        registration.SetInput(state_.ui.Input());
        registration.SetAudioOutput(audio_output_, 16000U, &power_);
        registration.SetBattery(battery_);
        registration.SetWifi(wifi_);
        registration.SetPower(power_);
        registration.SetLocalControl(state_.local_control);
        registration.SetSystemUi(system_ui_);
        const bool registered =
            registration.AddHaptics(haptics_, haptics::TimedHapticsPeripheral::kChannel, "Built-in vibration motor");
        return registered && context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    board_detail::MosaicoBoardState state_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    esp_mosaico::I2sAudioSink audio_output_{};
    board_detail::MosaicoPresentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
    esp_mosaico::BatteryPeripheral battery_{};
    wifi::NativeWifiRadio wifi_radio_{};
    wifi::WifiManager wifi_{wifi_radio_};
    esp_mosaico::PowerController power_{};
    haptics::TimedHapticsPeripheral haptics_{esp_mosaico::ConfiguredHapticActuator(), 0U};
};

}  // namespace

Board& ConfiguredBoard() {
    static EspMosaicoBoard board;
    return board;
}

}  // namespace micropixel::platform
