#include "platform/platform.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
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
#include "host_ui/system_gesture_router.hpp"
#include "lvgl.h"
#include "platform/boards/esp-mosaico/battery_backend.hpp"
#include "platform/boards/esp-mosaico/display/panel_transition_compositor.hpp"
#include "platform/boards/esp-mosaico/usb_cdc_console.hpp"
#include "platform/common/i2c_executor.hpp"
#include "platform/common/unavailable_backends.hpp"
#include "platform/common/wifi/wifi_backend.hpp"
#include "platform/configured_backends.hpp"
#include "platform/drivers/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/lvgl/display/system_transition_timeline.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/lvgl/ui/square_common/hall_card_ui.hpp"
#include "platform/lvgl/ui/square_common/hall_carousel.hpp"
#include "platform/lvgl/ui/square_common/hall_cover_cache.hpp"
#include "platform/lvgl/ui/square_common/hall_cover_codec.hpp"
#include "platform/lvgl/ui/square_common/hall_scene_ui.hpp"
#include "platform/lvgl/ui/square_common/profiles/square_480.hpp"
#include "platform/lvgl/ui/square_common/status_layer_transition.hpp"
#include "platform/lvgl/ui/square_common/status_layer_ui.hpp"
#include "platform/lvgl/ui/square_common/system_detail_ui.hpp"
#include "platform/lvgl/ui/square_common/system_menu_ui.hpp"
#include "platform/lvgl/ui/square_common/wifi_settings_ui.hpp"
#include "platform/radio/native_wifi_radio.hpp"
#include "platform/transports/tinyusb_cdc_local_control.hpp"
#include "src/display/lv_display_private.h"
#include "work/background_executor.hpp"

namespace micropixel::platform {
namespace {

namespace ui_profile = lvgl::square_common::profiles::square_480;
using HallCarousel = lvgl::square_common::HallCarouselPolicy<ui_profile::Layout>;

constexpr char kTag[] = "esp_mosaico";
constexpr int32_t kWidth = 480;
constexpr int32_t kHeight = 480;
constexpr uint32_t kHostPointerQueueCapacity = 32U;
constexpr uint32_t kBackgroundRgb = 0x08111fU;
constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;
constexpr gpio_num_t kDisplayReset = GPIO_NUM_42;
constexpr gpio_num_t kDisplayTe = GPIO_NUM_43;
constexpr gpio_num_t kDisplayChipSelect = GPIO_NUM_50;
constexpr gpio_num_t kDisplayClock = GPIO_NUM_44;
constexpr gpio_num_t kDisplayData0 = GPIO_NUM_36;
constexpr gpio_num_t kDisplayData1 = GPIO_NUM_51;
constexpr gpio_num_t kDisplayData2 = GPIO_NUM_35;
constexpr gpio_num_t kDisplayData3 = GPIO_NUM_9;
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_6;
constexpr gpio_num_t kI2cData = GPIO_NUM_0;
constexpr gpio_num_t kI2cClock = GPIO_NUM_1;
constexpr gpio_num_t kPeripheralPower = GPIO_NUM_60;
constexpr gpio_num_t kPowerSwitch = GPIO_NUM_57;
constexpr uint32_t kDisplayPixelClockHz = 40U * 1000U * 1000U;
constexpr uint32_t kDisplayDataWidth = 4U;
constexpr uint32_t kDisplayBitsPerPixel = 16U;
constexpr uint32_t kPowerRampMs = 50U;
constexpr uint32_t kTransitionAlignment = 128U;
constexpr uint32_t kDisplayFrameStride = static_cast<uint32_t>(kWidth) * 2U;
constexpr uint32_t kDisplayFrameBytes = kDisplayFrameStride * static_cast<uint32_t>(kHeight);
constexpr uint32_t kDisplayFrameAllocationBytes =
    (kDisplayFrameBytes + kTransitionAlignment - 1U) / kTransitionAlignment * kTransitionAlignment;
constexpr uint32_t kTransitionIntermediateSize = ui_profile::kSquareLayout.transition_intermediate_width;
constexpr uint32_t kTransitionIntermediateBytes = kTransitionIntermediateSize * kTransitionIntermediateSize * 2U;
constexpr uint32_t kTransitionIntermediateAllocationBytes =
    (kTransitionIntermediateBytes + kTransitionAlignment - 1U) / kTransitionAlignment * kTransitionAlignment;
constexpr uint32_t kSnapshotCoverSize = static_cast<uint32_t>(ui_profile::Layout::kHallCardWidth);
constexpr uint32_t kSnapshotCoverStride = kSnapshotCoverSize * 3U;
constexpr uint32_t kSnapshotCoverBytes = kSnapshotCoverStride * kSnapshotCoverSize;
constexpr uint32_t kSnapshotCoverAllocationBytes =
    (kSnapshotCoverBytes + kTransitionAlignment - 1U) / kTransitionAlignment * kTransitionAlignment;
// The S31 adapter renders into bounded DMA-capable internal-SRAM strips, so
// PPA Fill/Blend never shares ownership of a PSRAM scanout buffer with LCD
// DMA. Unsupported masks and source formats fall back to LVGL software.
constexpr bool kEnableLvglAdapterPpaAccel = CONFIG_MICROPIXEL_LVGL_PPA_ACCEL;

struct MosaicoState;

struct HallCardBinding final {
    MosaicoState* state{};
    uint32_t index{};
};

struct MosaicoState final {
    common::I2cExecutor i2c_executor{};
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
    lv_obj_t* host_root{};
    lv_indev_t* host_pointer_indev{};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts};
    lvgl::square_common::StatusLayerUi status_layer_ui{};
    lvgl::square_common::HallSceneUi hall_scene_ui{};
    lvgl::square_common::SystemMenuUi system_menu_ui{};
    lvgl::square_common::SystemDetailUi system_detail_ui{ui_profile::kSystemPageLayout};
    lvgl::square_common::WifiSettingsUi wifi_settings_ui{};
    mosaico::PanelTransitionCompositor panel_transition{};
    lvgl::square_common::HallCoverCache hall_cover_cache{{
        .target_size = static_cast<uint32_t>(ui_profile::Layout::kHallCardWidth),
        .corner_radius = static_cast<uint32_t>(ui_profile::kHallCardLayout.radius),
        .top_background_rgb = kBackgroundRgb,
        .bottom_background_rgb = 0x111f32U,
    }};
    drivers::EspLcdTouchInput touch_input{kWidth, kHeight};
    host_ui::SystemGestureRouter input_router{touch_input, kWidth, kHeight};
    transports::TinyUsbCdcLocalControl local_control{};
    lvgl::ScreenCaptureDevelopment screen_capture{};
    lv_image_dsc_t launch_image_descriptor{};
    std::array<lvgl::square_common::HallCardObjects, host_ui::kMaxHallApps> hall_cards{};
    std::array<lv_image_dsc_t, host_ui::kMaxHallApps> hall_cover_descriptors{};
    std::array<HallCardBinding, host_ui::kMaxHallApps> hall_bindings{};
    host_ui::SystemUiActionSink hall_action_sink{};
    void* hall_action_context{};
    uint32_t hall_app_count{};
    uint64_t hall_catalog_signature{};
    host_ui::HallStatusBarModel hall_status_bar{};
    std::array<bool, host_ui::kMaxHallApps> hall_app_running{};
    uint8_t* guest_transition_intermediate{};
    uint8_t* guest_snapshot_cover{};
    mosaico::PanelTransitionRect guest_transition_card{};
    bool guest_snapshot_in_hall{};
    bool displayed_shadow_valid{};
    bool hall_status_bar_valid{};
    bool hall_firmware_update_available{};
    bool hall_launch_enabled{};
    bool hall_retained_for_status{};
    portMUX_TYPE host_pointer_lock = portMUX_INITIALIZER_UNLOCKED;
    device::TouchSample host_pointer_queue[kHostPointerQueueCapacity]{};
    lv_point_t host_pointer_point{};
    lv_indev_state_t host_pointer_state{LV_INDEV_STATE_RELEASED};
    uint32_t host_pointer_touch_id{};
    uint32_t host_pointer_queue_head{};
    uint32_t host_pointer_queue_tail{};
    bool host_pointer_enabled{};
    bool host_pointer_touch_active{};
    bool host_pointer_release_queued{};
};

MosaicoState* g_display_flush_state{};

void CaptureDisplayedShadowFlush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    MosaicoState* state = g_display_flush_state;
    if (state != nullptr && state->display == display && state->displayed_shadow != nullptr && area != nullptr &&
        pixels != nullptr && area->x1 >= 0 && area->y1 >= 0 && area->x2 < kWidth && area->y2 < kHeight) {
        const uint32_t area_width = static_cast<uint32_t>(area->x2 - area->x1 + 1);
        const uint32_t area_height = static_cast<uint32_t>(area->y2 - area->y1 + 1);
        const uint32_t source_stride = lv_display_get_buf_active(display)->header.stride;
        async_color_convert_request_t copy{};
        copy.src_buffer = pixels;
        copy.src_stride = source_stride / 2U;
        copy.src_height = area_height;
        copy.dst_buffer = state->displayed_shadow;
        copy.dst_stride = static_cast<uint32_t>(kWidth);
        copy.dst_height = static_cast<uint32_t>(kHeight);
        copy.dst_x = static_cast<uint32_t>(area->x1);
        copy.dst_y = static_cast<uint32_t>(area->y1);
        copy.copy_width = area_width;
        copy.copy_height = area_height;
        copy.src_color_format = ESP_COLOR_FOURCC_RGB16;
        copy.dst_color_format = ESP_COLOR_FOURCC_RGB16;
        const bool copied = state->shadow_copy_dma2d != nullptr &&
                            esp_color_convert_blocking(state->shadow_copy_dma2d, &copy, -1) == ESP_OK;
        for (int32_t y = area->y1; copied && y <= area->y2; ++y) {
            if (area->x1 == 0 && area->x2 == kWidth - 1) {
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

uint64_t HallCatalogSignature(const host_ui::HallModel& model, uint32_t app_count) {
    constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t signature = (kFnvOffset ^ app_count) * kFnvPrime;
    for (uint32_t index = 0U; index < app_count; ++index) {
        const char* app_id = model.apps[index].app_id;
        if (app_id != nullptr) {
            while (*app_id != '\0') {
                signature = (signature ^ static_cast<uint8_t>(*app_id)) * kFnvPrime;
                ++app_id;
            }
        }
        signature = (signature ^ 0xffU) * kFnvPrime;
    }
    return signature;
}

void ResetHallCardsLocked(MosaicoState& state) {
    for (uint32_t index = 0U; index < host_ui::kMaxHallApps; ++index) {
        lvgl::square_common::DetachHallCover(state.hall_cards[index], state.hall_cover_descriptors[index]);
        state.hall_cards[index] = {};
        state.hall_bindings[index] = {};
        state.hall_app_running[index] = false;
    }
    state.hall_scene_ui.ResetLocked();
}

void ShowCachedHallPlaceholder(void* context, uint32_t app_index) {
    auto& state = *static_cast<MosaicoState*>(context);
    if (app_index < host_ui::kMaxHallApps) {
        lvgl::square_common::ShowHallCoverPlaceholder(state.hall_cards[app_index]);
    }
}

void AttachCachedHallCover(void* context, uint32_t app_index, const host_ui::HallCoverModel& cover) {
    auto& state = *static_cast<MosaicoState*>(context);
    if (app_index < state.hall_app_count) {
        auto& objects = state.hall_cards[app_index];
        lvgl::square_common::AttachHallCover(ui_profile::kHallCardLayout, objects,
                                             state.hall_cover_descriptors[app_index], cover);
        if (objects.cover_image != nullptr) {
            lv_obj_invalidate(lv_obj_get_parent(objects.cover_image));
        }
        if (objects.card != nullptr) {
            lv_obj_invalidate(objects.card);
        }
    }
}

void DetachCachedHallCover(void* context, uint32_t app_index) {
    auto& state = *static_cast<MosaicoState*>(context);
    if (app_index < host_ui::kMaxHallApps) {
        lvgl::square_common::DetachHallCover(state.hall_cards[app_index], state.hall_cover_descriptors[app_index]);
    }
}

void RefreshCachedHallCover(void* context) {
    auto& state = *static_cast<MosaicoState*>(context);
    if (state.display != nullptr) {
        lvgl::RequestDisplayRefresh(state.display);
    }
}

void RoundQspiArea(lv_area_t* area, void*) {
    if (area == nullptr) {
        return;
    }
    area->x1 = std::max<int32_t>(0, area->x1 / 4 * 4);
    area->x2 = std::min<int32_t>(kWidth - 1, (area->x2 + 4) / 4 * 4 - 1);
}

esp_err_t ConfigureOutput(gpio_num_t pin, int level, gpio_mode_t mode = GPIO_MODE_OUTPUT) {
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), kTag, "preset GPIO%d failed", static_cast<int>(pin));
    gpio_config_t config{};
    config.pin_bit_mask = BIT64(pin);
    config.mode = mode;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&config);
}

esp_err_t EnablePeripheralPower() {
    constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_timer_t kTimer = LEDC_TIMER_1;
    constexpr ledc_channel_t kChannel = LEDC_CHANNEL_1;
    constexpr ledc_timer_bit_t kResolution = LEDC_TIMER_8_BIT;
    constexpr uint32_t kMaximumDuty = (1U << kResolution) - 1U;

    ESP_RETURN_ON_ERROR(ConfigureOutput(kPowerSwitch, 1, GPIO_MODE_OUTPUT_OD), kTag,
                        "release board power switch failed");
    ledc_timer_config_t timer{};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = kResolution;
    timer.timer_num = kTimer;
    timer.freq_hz = 100000U;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), kTag, "configure peripheral power ramp timer failed");

    ledc_channel_config_t channel{};
    channel.gpio_num = kPeripheralPower;
    channel.speed_mode = kSpeedMode;
    channel.channel = kChannel;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = kTimer;
    channel.duty = kMaximumDuty;
    channel.hpoint = 0U;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), kTag, "configure peripheral power ramp channel failed");

    const esp_err_t installed = ledc_fade_func_install(0U);
    esp_err_t status = installed == ESP_ERR_INVALID_STATE ? ESP_OK : installed;
    if (status == ESP_OK) {
        status = ledc_set_fade_with_time(kSpeedMode, kChannel, 0U, kPowerRampMs);
    }
    if (status == ESP_OK) {
        status = ledc_fade_start(kSpeedMode, kChannel, LEDC_FADE_WAIT_DONE);
    }
    if (installed == ESP_OK) {
        ledc_fade_func_uninstall();
    }
    ledc_stop(kSpeedMode, kChannel, status == ESP_OK ? 0 : 1);
    ESP_RETURN_ON_ERROR(status, kTag, "ramp peripheral power failed");
    ESP_RETURN_ON_ERROR(ConfigureOutput(kPeripheralPower, 0), kTag, "hold peripheral power on failed");
    ESP_LOGI(kTag, "Mosaico 3V3 peripheral rail ramped on over %" PRIu32 " ms", kPowerRampMs);
    return ESP_OK;
}

esp_err_t InitializePanel(MosaicoState& state) {
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

    ESP_RETURN_ON_ERROR(EnablePeripheralPower(), kTag, "enable Mosaico peripheral power failed");
    spi_bus_config_t bus_config{};
    bus_config.data0_io_num = kDisplayData0;
    bus_config.data1_io_num = kDisplayData1;
    bus_config.sclk_io_num = kDisplayClock;
    bus_config.data2_io_num = kDisplayData2;
    bus_config.data3_io_num = kDisplayData3;
    bus_config.data4_io_num = -1;
    bus_config.data5_io_num = -1;
    bus_config.data6_io_num = -1;
    bus_config.data7_io_num = -1;
    // Match the DMA transaction ceiling to one LVGL draw buffer. On Mosaico
    // that buffer is a full-height RGB565 surface in PSRAM, and the panel IO
    // consumes it directly without an internal-SRAM bounce allocation.
    bus_config.max_transfer_sz =
        kWidth * CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT * static_cast<int32_t>(kDisplayBitsPerPixel) / 8;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kDisplaySpiHost, &bus_config, SPI_DMA_CH_AUTO), kTag,
                        "initialize CO5300 QSPI bus failed");
    constexpr gpio_num_t kBusPins[] = {kDisplayClock, kDisplayData0, kDisplayData1, kDisplayData2, kDisplayData3};
    for (gpio_num_t pin : kBusPins) {
        ESP_RETURN_ON_ERROR(
            gpio_set_drive_capability(pin, static_cast<gpio_drive_cap_t>(CONFIG_MICROPIXEL_MOSAICO_LCD_QSPI_DRIVE_CAP)),
            kTag, "set QSPI GPIO%d drive capability failed", static_cast<int>(pin));
    }

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = kDisplayChipSelect;
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = kDisplayPixelClockHz;
    io_config.trans_queue_depth = 10U;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    // PPA and DMA2D finish before a transition frame is submitted. Let QSPI DMA
    // consume that immutable PSRAM frame directly instead of copying it through
    // an internal-SRAM strip and issuing one panel command per strip.
    io_config.flags.psram_dma_direct = true;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kDisplaySpiHost), &io_config, &state.panel_io),
        kTag, "create CO5300 panel IO failed");

    co5300_vendor_config_t vendor_config{};
    vendor_config.init_cmds = kVendorInit;
    vendor_config.init_cmds_size = std::size(kVendorInit);
    vendor_config.flags.use_qspi_interface = true;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = kDisplayReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = kDisplayBitsPerPixel;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(state.panel_io, &panel_config, &state.panel), kTag,
                        "create CO5300 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(state.panel), kTag, "reset CO5300 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(state.panel), kTag, "initialize CO5300 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(state.panel, true), kTag, "turn CO5300 on failed");
    ESP_LOGI(kTag, "CO5300 ready: 480x480 QSPI with TE on GPIO%d", static_cast<int>(kDisplayTe));
    return ESP_OK;
}

esp_err_t InitializeDisplay(MosaicoState& state) {
    ESP_RETURN_ON_ERROR(InitializePanel(state), kTag, "initialize Mosaico panel failed");
    if (!esp_lv_adapter_is_initialized()) {
        esp_lv_adapter_config_t adapter_config{};
        adapter_config.task_stack_size = 12288U;
        adapter_config.task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY;
        adapter_config.task_core_id = ESP_LV_ADAPTER_DEFAULT_TASK_CORE_ID;
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
        ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), kTag, "initialize LVGL adapter failed");
    }
    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.panel;
    display_config.panel_io = state.panel_io;
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_OTHER;
    display_config.profile.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.profile.hor_res = kWidth;
    display_config.profile.ver_res = kHeight;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#ifdef CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.require_double_buffer = true;
    display_config.profile.enable_ppa_accel = kEnableLvglAdapterPpaAccel;
    display_config.profile.mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    state.displayed_shadow = static_cast<uint8_t*>(heap_caps_aligned_calloc(
        kTransitionAlignment, kDisplayFrameAllocationBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (state.displayed_shadow == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    async_color_convert_config_t dma2d_config{};
    dma2d_config.backlog = 1U;
    dma2d_config.dma_burst_size = 128U;
    ESP_RETURN_ON_ERROR(esp_async_color_convert_install_dma2d(&dma2d_config, &state.shadow_copy_dma2d), kTag,
                        "install displayed-shadow DMA2D copier failed");
    state.adapter_flush_cb = state.display->flush_cb;
    if (state.adapter_flush_cb == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    g_display_flush_state = &state;
    lv_display_set_flush_cb(state.display, CaptureDisplayedShadowFlush);
    ESP_RETURN_ON_ERROR(esp_lv_adapter_set_area_rounder_cb(state.display, RoundQspiArea, nullptr), kTag,
                        "configure QSPI area alignment failed");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), kTag, "start LVGL adapter failed");
    return ESP_OK;
}

esp_err_t InitializeTouch(MosaicoState& state) {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.sda_io_num = kI2cData;
    bus_config.scl_io_num = kI2cClock;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &state.i2c_bus), kTag,
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
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(state.i2c_bus, &io_config, &state.touch_io), kTag,
                        "create CST9217 panel IO failed");

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = kWidth - 1;
    touch_config.y_max = kHeight - 1;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    touch_config.int_gpio_num = kTouchInterrupt;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst9217(state.touch_io, &touch_config, &state.touch), kTag,
                        "initialize CST9217 failed");
    ESP_LOGI(kTag, "CST9217 ready: interrupt GPIO%d shared I2C0", static_cast<int>(kTouchInterrupt));
    return ESP_OK;
}

void StyleFullscreen(lv_obj_t* object, uint32_t background) {
    lv_obj_set_pos(object, 0, 0);
    lv_obj_set_size(object, kWidth, kHeight);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* PrepareSystemPageRootLocked(MosaicoState& state) {
    if (state.host_root == nullptr) {
        state.host_root = lv_obj_create(lv_screen_active());
        StyleFullscreen(state.host_root, kBackgroundRgb);
    }
    ResetHallCardsLocked(state);
    lv_obj_clean(state.host_root);
    lv_obj_set_style_bg_color(state.host_root, lv_color_hex(kBackgroundRgb), 0);
    return state.host_root;
}

void SetHostPointerEnabledLocked(MosaicoState& state, bool enabled) {
    portENTER_CRITICAL(&state.host_pointer_lock);
    state.host_pointer_queue_head = 0U;
    state.host_pointer_queue_tail = 0U;
    state.host_pointer_state = LV_INDEV_STATE_RELEASED;
    state.host_pointer_touch_active = false;
    state.host_pointer_release_queued = false;
    state.host_pointer_enabled = enabled;
    portEXIT_CRITICAL(&state.host_pointer_lock);
    if (state.host_pointer_indev != nullptr) {
        lv_indev_set_mode(state.host_pointer_indev, LV_INDEV_MODE_EVENT);
        lv_indev_enable(state.host_pointer_indev, enabled);
        lv_indev_reset(state.host_pointer_indev, nullptr);
    }
}

void HostPointerRead(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* state = static_cast<MosaicoState*>(lv_indev_get_user_data(indev));
    if (state == nullptr || data == nullptr) {
        return;
    }
    bool sample_read = false;
    bool pointer_released = false;
    portENTER_CRITICAL(&state->host_pointer_lock);
    if (state->host_pointer_queue_tail != state->host_pointer_queue_head) {
        const device::TouchSample& sample = state->host_pointer_queue[state->host_pointer_queue_tail];
        state->host_pointer_queue_tail = (state->host_pointer_queue_tail + 1U) % kHostPointerQueueCapacity;
        state->host_pointer_point = {.x = sample.x, .y = sample.y};
        state->host_pointer_state =
            sample.phase == device::TouchPhase::kDown || sample.phase == device::TouchPhase::kMove
                ? LV_INDEV_STATE_PRESSED
                : LV_INDEV_STATE_RELEASED;
        if (sample.phase == device::TouchPhase::kUp || sample.phase == device::TouchPhase::kCancel) {
            state->host_pointer_release_queued = false;
            pointer_released = true;
        }
        sample_read = true;
    }
    data->point = state->host_pointer_point;
    data->state = state->host_pointer_enabled ? state->host_pointer_state : LV_INDEV_STATE_RELEASED;
    data->continue_reading = state->host_pointer_queue_tail != state->host_pointer_queue_head;
    portEXIT_CRITICAL(&state->host_pointer_lock);
    if (sample_read && state->display != nullptr) {
        lvgl::RequestDisplayRefresh(state->display);
    }
    if (pointer_released) {
        state->wifi_settings_ui.PointerReleased();
    }
}

bool HostPointerBusy(MosaicoState& state) {
    portENTER_CRITICAL(&state.host_pointer_lock);
    const bool busy = state.host_pointer_touch_active || state.host_pointer_release_queued ||
                      state.host_pointer_queue_head != state.host_pointer_queue_tail;
    portEXIT_CRITICAL(&state.host_pointer_lock);
    return busy;
}

bool HostPointerTouchSink(void* context, const device::TouchSample& sample) {
    auto* state = static_cast<MosaicoState*>(context);
    if (state == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&state->host_pointer_lock);
    if (!state->host_pointer_enabled) {
        portEXIT_CRITICAL(&state->host_pointer_lock);
        return true;
    }
    if (sample.phase == device::TouchPhase::kDown && !state->host_pointer_touch_active) {
        state->host_pointer_touch_active = true;
        state->host_pointer_touch_id = sample.id;
    } else if (!state->host_pointer_touch_active || sample.id != state->host_pointer_touch_id) {
        portEXIT_CRITICAL(&state->host_pointer_lock);
        return true;
    }
    const uint32_t next_head = (state->host_pointer_queue_head + 1U) % kHostPointerQueueCapacity;
    if (next_head == state->host_pointer_queue_tail) {
        state->host_pointer_queue_tail = (state->host_pointer_queue_tail + 1U) % kHostPointerQueueCapacity;
    }
    state->host_pointer_queue[state->host_pointer_queue_head] = sample;
    state->host_pointer_queue_head = next_head;
    if (sample.phase == device::TouchPhase::kUp || sample.phase == device::TouchPhase::kCancel) {
        state->host_pointer_touch_active = false;
        state->host_pointer_release_queued = true;
    }
    portEXIT_CRITICAL(&state->host_pointer_lock);

    if (state->host_pointer_indev != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_timer_t* read_timer = lv_indev_get_read_timer(state->host_pointer_indev);
        if (read_timer != nullptr) {
            lv_timer_resume(read_timer);
            lv_timer_ready(read_timer);
        }
        esp_lv_adapter_unlock();
    }
    (void)esp_lv_adapter_request_wake();
    return true;
}

void HallCardEvent(lv_event_t* event) {
    auto* binding = static_cast<HallCardBinding*>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->state == nullptr || binding->index >= binding->state->hall_app_count) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lvgl::square_common::SetHallCardPressed(binding->state->hall_cards[binding->index], true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lvgl::square_common::SetHallCardPressed(binding->state->hall_cards[binding->index], false);
    } else if (code == LV_EVENT_SHORT_CLICKED && binding->state->hall_launch_enabled &&
               binding->state->hall_action_sink != nullptr) {
        ESP_LOGI(kTag, "App Hall accepted card tap: index=%" PRIu32, binding->index);
        binding->state->hall_action_sink(
            binding->state->hall_action_context,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchApp, .app_index = binding->index});
    }
}

void HallStopButtonEvent(lv_event_t* event) {
    auto* binding = static_cast<HallCardBinding*>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->state == nullptr || binding->index >= binding->state->hall_app_count ||
        !binding->state->hall_app_running[binding->index] || binding->state->hall_action_sink == nullptr) {
        return;
    }
    ESP_LOGI(kTag, "App Hall accepted stop tap: index=%" PRIu32, binding->index);
    binding->state->hall_action_sink(
        binding->state->hall_action_context,
        host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kStopApp, .app_index = binding->index});
}

void HallCarouselEvent(lv_event_t* event) {
    auto* state = static_cast<MosaicoState*>(lv_event_get_user_data(event));
    lv_obj_t* viewport = lv_event_get_current_target_obj(event);
    if (state == nullptr || viewport == nullptr || viewport != state->hall_scene_ui.objects().carousel_viewport) {
        return;
    }
    const int32_t offset = HallCarousel::ClampOffset(state->hall_app_count, lv_obj_get_scroll_x(viewport));
    state->hall_cover_cache.RequestWindow(HallCarousel::CoverWindowFirst(state->hall_app_count, offset),
                                          HallCarousel::CoverWindowLast(state->hall_app_count, offset),
                                          lv_event_get_code(event) == LV_EVENT_SCROLL_END);
    state->hall_scene_ui.UpdateScrollLocked(state->hall_app_count, offset);
}

void DrawHallCard(MosaicoState& state, lv_obj_t* parent, const host_ui::HallAppModel& app, uint32_t index) {
    state.hall_bindings[index] = {.state = &state, .index = index};
    state.hall_app_running[index] = app.running;
    lvgl::square_common::DrawHallCard(parent, ui_profile::kHallCardLayout,
                                      {.app_id = app.app_id, .display_name = app.display_name, .running = app.running},
                                      index, HallCardEvent, HallStopButtonEvent, &state.hall_bindings[index],
                                      state.hall_cards[index]);
    lv_obj_set_pos(
        state.hall_cards[index].card,
        static_cast<int32_t>(index) * (ui_profile::Layout::kHallCardWidth + ui_profile::Layout::kHallCardGap), 0);
}

void DeleteHostRootLocked(MosaicoState& state) {
    if (state.host_root != nullptr) {
        ResetHallCardsLocked(state);
        lv_obj_delete(state.host_root);
        state.host_root = nullptr;
    }
    state.hall_scene_ui.ResetLocked();
    state.launch_image_descriptor = {};
}

class MosaicoGraphicsBackend final : public device::GraphicsBackend {
   public:
    explicit MosaicoGraphicsBackend(MosaicoState& state) : state_(state) {}

    [[nodiscard]] bool Available() const override { return state_.guest_graphics.Available(); }
    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t& info) override {
        return state_.guest_graphics.GetInfo(info);
    }
    [[nodiscard]] int32_t BeginFrame() override { return state_.guest_graphics.BeginFrame(); }
    [[nodiscard]] int32_t Submit(const uint8_t* bytes, uint32_t length,
                                 const device::TextureAccess& textures) override {
        return state_.guest_graphics.Submit(bytes, length, textures);
    }
    [[nodiscard]] int32_t CommitFrame(const device::TextureAccess& textures) override {
        return state_.guest_graphics.CommitFrame(textures);
    }
    [[nodiscard]] int32_t CancelFrame() override { return state_.guest_graphics.CancelFrame(); }
    [[nodiscard]] int32_t LoadFont(const device::FontResourceView& resource,
                                   micropixel_font_info_t& info_out) override {
        return state_.guest_graphics.LoadFont(resource, info_out);
    }
    [[nodiscard]] int32_t ReleaseFont(micropixel_font_handle_t font) override {
        return state_.guest_graphics.ReleaseFont(font);
    }
    [[nodiscard]] int32_t MeasureText(micropixel_font_handle_t font, const char* text, uint32_t text_length,
                                      micropixel_text_metrics_t& metrics_out) override {
        return state_.guest_graphics.MeasureText(font, text, text_length, metrics_out);
    }
    [[nodiscard]] int32_t BeginBitmapUpdateFrame() override { return state_.guest_graphics.BeginBitmapUpdateFrame(); }
    [[nodiscard]] int32_t UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height, const uint8_t* pixels, uint32_t stride) override {
        return state_.guest_graphics.UpdateBitmap(bitmap, x, y, width, height, pixels, stride);
    }
    [[nodiscard]] int32_t CommitBitmapUpdateFrame() override { return state_.guest_graphics.CommitBitmapUpdateFrame(); }

    [[nodiscard]] int32_t ShowLaunchBitmap(const device::BitmapView& bitmap) override {
        const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 3U;
        if (state_.display == nullptr || state_.host_root == nullptr || bitmap.data == nullptr ||
            (!esp_ptr_in_drom(bitmap.data) && !esp_ptr_external_ram(bitmap.data)) ||
            (bitmap.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
             bitmap.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888) ||
            bitmap.width == 0U || bitmap.height == 0U || bitmap.stride != bitmap.width * bytes_per_pixel ||
            bitmap.size != bitmap.stride * bitmap.height) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        if (esp_ptr_external_ram(bitmap.data)) {
            lv_draw_buf_t draw_buffer{};
            if (lv_draw_buf_init(&draw_buffer, bitmap.width, bitmap.height,
                                 bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? LV_COLOR_FORMAT_ARGB8888
                                                                                         : LV_COLOR_FORMAT_RGB888,
                                 bitmap.stride, const_cast<uint8_t*>(bitmap.data), bitmap.size) != LV_RESULT_OK) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            lv_draw_buf_flush_cache(&draw_buffer, nullptr);
        }
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return MICROPIXEL_STATUS_INTERNAL;
        }
        lv_obj_clean(state_.host_root);
        lv_obj_set_style_bg_color(state_.host_root, lv_color_hex(kBackgroundRgb), 0);
        state_.launch_image_descriptor = {};
        state_.launch_image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        state_.launch_image_descriptor.header.cf =
            bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB888;
        state_.launch_image_descriptor.header.w = bitmap.width;
        state_.launch_image_descriptor.header.h = bitmap.height;
        state_.launch_image_descriptor.header.stride = bitmap.stride;
        state_.launch_image_descriptor.data_size = bitmap.size;
        state_.launch_image_descriptor.data = bitmap.data;
        lv_obj_t* image = lv_image_create(state_.host_root);
        lv_image_set_src(image, &state_.launch_image_descriptor);
        const uint32_t maximum_dimension = std::max(bitmap.width, bitmap.height);
        if (maximum_dimension > static_cast<uint32_t>(kWidth)) {
            lv_image_set_scale(image, static_cast<uint16_t>(256U * static_cast<uint32_t>(kWidth) / maximum_dimension));
        }
        lv_obj_center(image);
        lv_obj_t* label = lv_label_create(state_.host_root);
        lv_label_set_text(label, "Loading...");
        lv_obj_set_style_text_color(label, lv_color_hex(0xeaf4ffU), 0);
        lv_obj_set_style_text_font(label, lvgl::BuiltinLatinFont(lvgl::SystemFontRole::kLarge), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -24);
        lvgl::RequestDisplayRefresh(state_.display);
        esp_lv_adapter_unlock();
        ESP_LOGI(kTag, "Bundle launch bitmap visible: %" PRIu32 "x%" PRIu32, bitmap.width, bitmap.height);
        return MICROPIXEL_STATUS_OK;
    }

    void DismissLaunchBitmap() override {
        if (state_.display == nullptr || state_.launch_image_descriptor.data == nullptr ||
            esp_lv_adapter_lock(-1) != ESP_OK) {
            return;
        }
        DeleteHostRootLocked(state_);
        lvgl::RequestDisplayRefresh(state_.display);
        esp_lv_adapter_unlock();
    }
    void ReleaseGuestResources() override { state_.guest_graphics.Release(); }

   private:
    MosaicoState& state_;
};

class MosaicoSystemUiBackend final : public common::UnavailableSystemUiBackend {
   public:
    explicit MosaicoSystemUiBackend(MosaicoState& state) : state_(state) {}

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowHall(const host_ui::HallModel& model,
                                                                       host_ui::SystemUiActionSink action_sink,
                                                                       void* action_context) override {
        if (state_.display == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        const uint32_t visible_count = std::min(model.app_count, host_ui::kMaxHallApps);
        const uint64_t catalog_signature = HallCatalogSignature(model, visible_count);
        bool running_state_matches = visible_count == state_.hall_app_count;
        for (uint32_t index = 0U; running_state_matches && index < visible_count; ++index) {
            running_state_matches = state_.hall_app_running[index] == model.apps[index].running;
        }
        const bool may_resume = state_.hall_retained_for_status && state_.host_root != nullptr &&
                                catalog_signature == state_.hall_catalog_signature && running_state_matches &&
                                model.launch_enabled == state_.hall_launch_enabled &&
                                model.firmware_update_available == state_.hall_firmware_update_available;
        state_.hall_retained_for_status = false;
        if (may_resume && esp_lv_adapter_lock(-1) == ESP_OK) {
            const auto& objects = state_.hall_scene_ui.objects();
            const bool scene_valid = lv_obj_is_valid(state_.host_root) && objects.carousel_content != nullptr &&
                                     lv_obj_is_valid(objects.carousel_content);
            if (scene_valid) {
                if (!state_.hall_status_bar_valid ||
                    !lvgl::square_common::HallStatusBarMatches(state_.hall_status_bar, model.status_bar)) {
                    state_.hall_scene_ui.UpdateStatusBarLocked(model.status_bar);
                    state_.hall_status_bar = model.status_bar;
                    state_.hall_status_bar_valid = true;
                    lvgl::RequestDisplayRefresh(state_.display);
                }
                state_.status_layer_ui.RaisePerformanceOverlayLocked();
                SetHostPointerEnabledLocked(state_, true);
                esp_lv_adapter_unlock();
                state_.hall_action_sink = action_sink;
                state_.hall_action_context = action_context;
                state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
                state_.input_router.BindSystemActionSink(action_sink, action_context);
                ESP_LOGI(kTag, "Mosaico App Hall resumed after status layer: redraw=no");
                return {};
            }
            esp_lv_adapter_unlock();
        }

        state_.hall_app_count = visible_count;
        state_.hall_catalog_signature = catalog_signature;
        state_.hall_cover_cache.BeginCatalog(model, state_.hall_app_count, catalog_signature);
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        if (state_.host_root == nullptr) {
            state_.host_root = lv_obj_create(lv_screen_active());
            StyleFullscreen(state_.host_root, kBackgroundRgb);
        }
        ResetHallCardsLocked(state_);
        lv_obj_clean(state_.host_root);
        state_.launch_image_descriptor = {};
        state_.hall_action_sink = action_sink;
        state_.hall_action_context = action_context;
        state_.hall_launch_enabled = model.launch_enabled;
        state_.hall_firmware_update_available = model.firmware_update_available;
        state_.hall_status_bar = model.status_bar;
        state_.hall_status_bar_valid = true;

        state_.hall_scene_ui.DrawLocked(state_.host_root, ui_profile::kHallSceneLayout, model, state_.hall_app_count,
                                        {.carousel_event = HallCarouselEvent,
                                         .carousel_context = &state_,
                                         .action_sink = action_sink,
                                         .action_context = action_context});
        lv_obj_t* content = state_.hall_scene_ui.objects().carousel_content;
        for (uint32_t index = 0U; index < state_.hall_app_count; ++index) {
            DrawHallCard(state_, content, model.apps[index], index);
        }
        state_.hall_cover_cache.RequestWindow(HallCarousel::CoverWindowFirst(state_.hall_app_count, 0),
                                              HallCarousel::CoverWindowLast(state_.hall_app_count, 0), true);
        state_.hall_scene_ui.UpdateScrollLocked(state_.hall_app_count, 0);

        lv_obj_move_foreground(state_.host_root);
        lvgl::RequestDisplayRefresh(state_.display);
        esp_lv_adapter_unlock();
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            SetHostPointerEnabledLocked(state_, true);
            esp_lv_adapter_unlock();
        }
        state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        ESP_LOGI(kTag, "Mosaico App Hall rendered: apps=%" PRIu32 " launch=%s", state_.hall_app_count,
                 state_.hall_launch_enabled ? "enabled" : "disabled");
        return {};
    }

    void UpdateHallStatusBar(const host_ui::HallStatusBarModel& model) override {
        if (state_.hall_scene_ui.objects().time_label == nullptr || esp_lv_adapter_lock(0) != ESP_OK) {
            return;
        }
        state_.hall_scene_ui.UpdateStatusBarLocked(model);
        state_.hall_status_bar = model;
        state_.hall_status_bar_valid = true;
        lvgl::RequestDisplayRefresh(state_.display);
        esp_lv_adapter_unlock();
    }

    void PauseHallCoverLoading() override { state_.hall_cover_cache.Pause(); }

    void LeaveHall() override {
        state_.hall_cover_cache.Pause();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.hall_action_context);
        state_.hall_action_sink = nullptr;
        state_.hall_action_context = nullptr;
        state_.hall_app_count = 0U;
        state_.hall_launch_enabled = false;
        state_.hall_retained_for_status = false;
        state_.hall_status_bar_valid = false;
        if (state_.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
            return;
        }
        SetHostPointerEnabledLocked(state_, false);
        if (state_.host_root != nullptr) {
            const int64_t background_started_us = esp_timer_get_time();
            if (state_.displayed_shadow_valid &&
                state_.panel_transition.CaptureDisplayedBackground(state_.displayed_shadow)) {
                ESP_LOGI(kTag, "Hall transition baseline retained before App launch: elapsed=%" PRIu32 " us",
                         static_cast<uint32_t>(esp_timer_get_time() - background_started_us));
            } else {
                ESP_LOGW(kTag, "could not retain Hall transition baseline before App launch");
            }
            ResetHallCardsLocked(state_);
            lv_obj_clean(state_.host_root);
            lv_obj_set_style_bg_color(state_.host_root, lv_color_hex(kBackgroundRgb), 0);
        }
        state_.hall_scene_ui.ResetLocked();
        lvgl::RequestDisplayRefresh(state_.display);
        esp_lv_adapter_unlock();
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) override {
        if (state_.display == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }

        state_.hall_cover_cache.Pause();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.hall_action_context);
        state_.hall_action_sink = nullptr;
        state_.hall_action_context = nullptr;
        state_.hall_app_count = 0U;
        state_.hall_launch_enabled = false;
        state_.hall_retained_for_status = false;

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        if (state_.host_root == nullptr) {
            state_.host_root = lv_obj_create(lv_screen_active());
            StyleFullscreen(state_.host_root, kBackgroundRgb);
        }
        ResetHallCardsLocked(state_);
        lv_obj_clean(state_.host_root);
        lv_obj_set_style_bg_color(state_.host_root, lv_color_hex(kBackgroundRgb), 0);
        state_.launch_image_descriptor = {};
        auto result = state_.system_menu_ui.ShowLocked(state_.host_root, state_.display, ui_profile::kSystemMenuLayout,
                                                       model, action_sink, action_context);
        if (result.has_value() && state_.status_layer_ui.PerformanceOverlayVisibleLocked()) {
            state_.status_layer_ui.RaisePerformanceOverlayLocked();
        }
        SetHostPointerEnabledLocked(state_, result.has_value());
        esp_lv_adapter_unlock();
        if (!result.has_value()) {
            state_.system_menu_ui.Deactivate();
            return result;
        }

        state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        return {};
    }

    void UpdateSystemMenu(const host_ui::SystemMenuModel& model) override { state_.system_menu_ui.Update(model); }

    void LeaveSystemMenu() override {
        if (!state_.system_menu_ui.Active()) {
            return;
        }
        void* action_context = state_.system_menu_ui.ActionContext();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(action_context);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            SetHostPointerEnabledLocked(state_, false);
            esp_lv_adapter_unlock();
        }
        state_.system_menu_ui.Deactivate();
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformation(
        const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        if (state_.display == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.system_menu_ui.ActionContext());
        state_.system_menu_ui.Deactivate();
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        auto result = state_.system_detail_ui.ShowSystemInformationLocked(PrepareSystemPageRootLocked(state_), model,
                                                                          action_sink, action_context);
        SetHostPointerEnabledLocked(state_, result.has_value());
        esp_lv_adapter_unlock();
        if (!result.has_value()) {
            state_.system_detail_ui.LeaveSystemInformation();
            return result;
        }
        state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        return {};
    }

    void UpdateSystemInformation(const host_ui::SystemInformationModel& model) override {
        if (!state_.system_detail_ui.SystemInformationVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
            return;
        }
        state_.system_detail_ui.UpdateSystemInformationLocked(model);
        esp_lv_adapter_unlock();
    }

    void LeaveSystemInformation() override {
        if (!state_.system_detail_ui.SystemInformationVisible()) {
            return;
        }
        void* action_context = state_.system_detail_ui.SystemInformationActionContext();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(action_context);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            SetHostPointerEnabledLocked(state_, false);
            esp_lv_adapter_unlock();
        }
        state_.system_detail_ui.LeaveSystemInformation();
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowPowerManagement(
        const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        if (state_.display == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.system_menu_ui.ActionContext());
        state_.system_menu_ui.Deactivate();
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        auto result = state_.system_detail_ui.ShowPowerManagementLocked(PrepareSystemPageRootLocked(state_), model,
                                                                        action_sink, action_context);
        SetHostPointerEnabledLocked(state_, result.has_value());
        esp_lv_adapter_unlock();
        if (!result.has_value()) {
            state_.system_detail_ui.LeavePowerManagement();
            return result;
        }
        state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        return {};
    }

    void UpdatePowerManagement(const host_ui::PowerManagementModel& model) override {
        if (!state_.system_detail_ui.PowerManagementVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
            return;
        }
        state_.system_detail_ui.UpdatePowerManagementLocked(model);
        esp_lv_adapter_unlock();
    }

    void LeavePowerManagement() override {
        if (!state_.system_detail_ui.PowerManagementVisible()) {
            return;
        }
        void* action_context = state_.system_detail_ui.PowerManagementActionContext();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(action_context);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            SetHostPointerEnabledLocked(state_, false);
            esp_lv_adapter_unlock();
        }
        state_.system_detail_ui.LeavePowerManagement();
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowRemoteControl(
        const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        if (state_.display == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.system_menu_ui.ActionContext());
        state_.system_menu_ui.Deactivate();
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        auto result = state_.system_detail_ui.ShowRemoteControlLocked(PrepareSystemPageRootLocked(state_), model,
                                                                      action_sink, action_context);
        SetHostPointerEnabledLocked(state_, result.has_value());
        esp_lv_adapter_unlock();
        if (!result.has_value()) {
            state_.system_detail_ui.LeaveRemoteControl();
            return result;
        }
        state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        return {};
    }

    void UpdateRemoteControl(const host_ui::RemoteControlModel& model) override {
        if (!state_.system_detail_ui.RemoteControlVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
            return;
        }
        state_.system_detail_ui.UpdateRemoteControlLocked(model);
        esp_lv_adapter_unlock();
    }

    void LeaveRemoteControl() override {
        if (!state_.system_detail_ui.RemoteControlVisible()) {
            return;
        }
        void* action_context = state_.system_detail_ui.RemoteControlActionContext();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(action_context);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            SetHostPointerEnabledLocked(state_, false);
            esp_lv_adapter_unlock();
        }
        state_.system_detail_ui.LeaveRemoteControl();
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppManagement(
        const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override {
        if (state_.display == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.system_menu_ui.ActionContext());
        state_.system_menu_ui.Deactivate();
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        auto result = state_.system_detail_ui.ShowAppManagementLocked(PrepareSystemPageRootLocked(state_), model,
                                                                      action_sink, action_context);
        SetHostPointerEnabledLocked(state_, result.has_value());
        esp_lv_adapter_unlock();
        if (!result.has_value()) {
            state_.system_detail_ui.LeaveAppManagement();
            return result;
        }
        state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        return {};
    }

    void LeaveAppManagement() override {
        if (!state_.system_detail_ui.AppManagementVisible()) {
            return;
        }
        void* action_context = state_.system_detail_ui.AppManagementActionContext();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(action_context);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            SetHostPointerEnabledLocked(state_, false);
            esp_lv_adapter_unlock();
        }
        state_.system_detail_ui.LeaveAppManagement();
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowWifiSettings(const host_ui::WifiSettingsModel& model,
                                                                               host_ui::SystemUiActionSink action_sink,
                                                                               void* action_context) override {
        if (state_.display == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.system_menu_ui.ActionContext());
        state_.system_menu_ui.Deactivate();
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        auto result = state_.wifi_settings_ui.ShowLocked(
            PrepareSystemPageRootLocked(state_), state_.display, ui_profile::kSystemPageLayout, model, action_sink,
            action_context,
            [](void* context) {
                static_cast<lvgl::square_common::StatusLayerUi*>(context)->RaisePerformanceOverlayLocked();
            },
            &state_.status_layer_ui);
        SetHostPointerEnabledLocked(state_, result.has_value());
        esp_lv_adapter_unlock();
        if (!result.has_value()) {
            state_.wifi_settings_ui.Leave();
            return result;
        }
        state_.input_router.BindTouchSink(HostPointerTouchSink, &state_);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        return {};
    }

    void UpdateWifiSettings(const host_ui::WifiSettingsModel& model) override {
        state_.wifi_settings_ui.Update(model, HostPointerBusy(state_));
    }

    void LeaveWifiSettings() override {
        if (!state_.wifi_settings_ui.Visible()) {
            return;
        }
        void* action_context = state_.wifi_settings_ui.ActionContext();
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(action_context);
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            SetHostPointerEnabledLocked(state_, false);
            esp_lv_adapter_unlock();
        }
        state_.wifi_settings_ui.Leave();
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> RestoreGuestView() override {
        state_.input_router.UnbindTouchSink(&state_);
        state_.input_router.ClearSystemActionSink(state_.hall_action_context);
        lv_obj_t* guest_frame = state_.guest_graphics.FrameLocked();
        if (state_.display == nullptr || guest_frame == nullptr) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        const bool transition_requested = state_.guest_transition_intermediate != nullptr &&
                                          state_.guest_snapshot_cover != nullptr && state_.guest_snapshot_in_hall;
        bool transitioned = false;
        if (transition_requested && state_.displayed_shadow_valid &&
            state_.panel_transition.CaptureDisplayedBackground(state_.displayed_shadow)) {
            transitioned =
                state_.panel_transition.Animate(state_.guest_transition_intermediate, state_.guest_transition_card,
                                                mosaico::PanelTransitionDirection::kToGuest);
        }
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        SetHostPointerEnabledLocked(state_, false);
        DeleteHostRootLocked(state_);
        lv_obj_move_foreground(guest_frame);
        lv_obj_invalidate(guest_frame);
        lvgl::RequestDisplayRefresh(state_.display);
        esp_lv_adapter_unlock();
        state_.guest_snapshot_in_hall = false;
        ESP_LOGI(kTag, "retained Guest view restored from App Hall: transition=%s", transitioned ? "PPA" : "direct");
        return {};
    }

    [[nodiscard]] std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame(
        uint32_t hall_app_index, uint64_t trigger_timestamp_us) override {
        lv_obj_t* guest_frame = state_.guest_graphics.FrameLocked();
        if (state_.display == nullptr || guest_frame == nullptr || hall_app_index >= host_ui::kMaxHallApps ||
            state_.guest_transition_intermediate != nullptr || state_.guest_snapshot_cover != nullptr ||
            state_.displayed_shadow == nullptr || !state_.displayed_shadow_valid ||
            !state_.panel_transition.HasBackground()) {
            ESP_LOGW(kTag, "retained Guest capture unavailable: shadow=%s Hall-baseline=%s",
                     state_.displayed_shadow_valid ? "ready" : "incomplete",
                     state_.panel_transition.HasBackground() ? "ready" : "missing");
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        auto* intermediate = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            kTransitionAlignment, kTransitionIntermediateAllocationBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        auto* cover = static_cast<uint8_t*>(heap_caps_aligned_alloc(kTransitionAlignment, kSnapshotCoverAllocationBytes,
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (intermediate == nullptr || cover == nullptr) {
            heap_caps_free(cover);
            heap_caps_free(intermediate);
            return std::unexpected(host_ui::SystemUiError::kUnavailable);
        }
        const int32_t card_x =
            ui_profile::Layout::kHallLeft + static_cast<int32_t>(hall_app_index) *
                                                (ui_profile::Layout::kHallCardWidth + ui_profile::Layout::kHallCardGap);
        state_.guest_transition_card = {
            .x = std::min(card_x, kWidth - ui_profile::Layout::kHallCardWidth),
            .y = ui_profile::Layout::kHallTop,
            .width = ui_profile::Layout::kHallCardWidth,
            .height = ui_profile::Layout::kHallCardWidth,
        };

        uint32_t first_frame_elapsed_us = 0U;
        const bool first_frame_presented = state_.panel_transition.CaptureDisplayedToIntermediate(
            state_.displayed_shadow, intermediate, kTransitionIntermediateAllocationBytes, state_.guest_transition_card,
            trigger_timestamp_us, first_frame_elapsed_us);
        const int64_t cover_started_us = esp_timer_get_time();
        const bool cover_scaled = first_frame_presented && state_.panel_transition.ScaleIntermediateToCoverRgb888(
                                                               intermediate, cover, kSnapshotCoverAllocationBytes);
        if (cover_scaled) {
            lvgl::square_common::MaskHallCoverRgb888(cover, kSnapshotCoverSize,
                                                     static_cast<uint32_t>(ui_profile::kHallCardLayout.radius),
                                                     kBackgroundRgb, 0x111f32U);
        }
        const bool background_patched =
            cover_scaled && state_.panel_transition.UpdateBackgroundRgb888(cover, state_.guest_transition_card);
        const uint32_t cover_elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - cover_started_us);
        const bool animated =
            background_patched &&
            state_.panel_transition.Animate(intermediate, state_.guest_transition_card,
                                            mosaico::PanelTransitionDirection::kToHall, 140U, trigger_timestamp_us);
        if (!animated) {
            state_.panel_transition.CancelPreparedToHall();
            heap_caps_free(intermediate);
            heap_caps_free(cover);
            return std::unexpected(host_ui::SystemUiError::kRenderFailed);
        }
        lv_draw_buf_t cover_draw_buf{};
        if (lv_draw_buf_init(&cover_draw_buf, kSnapshotCoverSize, kSnapshotCoverSize, LV_COLOR_FORMAT_RGB888,
                             kSnapshotCoverStride, cover, kSnapshotCoverBytes) == LV_RESULT_OK) {
            lv_draw_buf_flush_cache(&cover_draw_buf, nullptr);
        }
        state_.guest_transition_intermediate = intermediate;
        state_.guest_snapshot_cover = cover;
        state_.guest_snapshot_in_hall = true;
        ESP_LOGI(kTag,
                 "captured retained Guest transition: source=displayed-%dx%d RGB565 intermediate=%" PRIu32
                 " cover=%" PRIu32 "x%" PRIu32 " first-frame=%" PRIu32 " us cover=%" PRIu32 " us transition=complete",
                 kWidth, kHeight, kTransitionIntermediateSize, kSnapshotCoverSize, kSnapshotCoverSize,
                 first_frame_elapsed_us, cover_elapsed_us);
        return host_ui::HallCoverModel{
            .data = cover,
            .size = kSnapshotCoverBytes,
            .width = kSnapshotCoverSize,
            .height = kSnapshotCoverSize,
            .stride = kSnapshotCoverStride,
            .cache_key = reinterpret_cast<uintptr_t>(cover),
        };
    }

    void ReleaseGuestSnapshot() override {
        state_.panel_transition.CancelPreparedToHall();
        heap_caps_free(state_.guest_snapshot_cover);
        heap_caps_free(state_.guest_transition_intermediate);
        state_.guest_snapshot_cover = nullptr;
        state_.guest_transition_intermediate = nullptr;
        state_.guest_transition_card = {};
        state_.guest_snapshot_in_hall = false;
    }

    void WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) override {
        state_.input_router.BindSystemActionSink(action_sink, action_context);
    }
    void StopWatchingGuestActions(void* action_context) override {
        state_.input_router.ClearSystemActionSink(action_context);
    }

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                              uint64_t trigger_timestamp_us,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) override {
        state_.hall_retained_for_status = false;
        if (state_.display != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
            const auto& objects = state_.hall_scene_ui.objects();
            state_.hall_retained_for_status = state_.host_root != nullptr && lv_obj_is_valid(state_.host_root) &&
                                              objects.carousel_content != nullptr &&
                                              lv_obj_is_valid(objects.carousel_content) &&
                                              state_.hall_action_context != nullptr;
            esp_lv_adapter_unlock();
        }
        auto result =
            lvgl::square_common::PresentStatusLayer(state_.display, state_.status_layer_ui, state_.panel_transition,
                                                    model, trigger_timestamp_us, action_sink, action_context, false);
        if (!result.has_value()) {
            state_.hall_retained_for_status = false;
            return std::unexpected(result.error());
        }
        state_.input_router.BindTouchSink(lvgl::square_common::StatusLayerUi::TouchSink, &state_.status_layer_ui);
        state_.input_router.BindSystemActionSink(action_sink, action_context);
        ESP_LOGI(kTag, "Mosaico status layer presented: transition=%s elapsed=%" PRIu32 " us",
                 result->hardware_accelerated ? "PPA" : "static", result->elapsed_us);
        return {};
    }

    void UpdateStatusLayer(const host_ui::StatusLayerModel& model) override {
        if (state_.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
            return;
        }
        state_.status_layer_ui.UpdateLocked(model);
        esp_lv_adapter_unlock();
    }

    void LeaveStatusLayer(uint64_t trigger_timestamp_us) override {
        void* action_context = state_.status_layer_ui.ActionContext();
        state_.input_router.UnbindTouchSink(&state_.status_layer_ui);
        state_.input_router.ClearSystemActionSink(action_context);
        const auto result = lvgl::square_common::DismissStatusLayer(
            state_.display, state_.status_layer_ui, state_.panel_transition, trigger_timestamp_us, false);
        ESP_LOGI(kTag, "Mosaico status layer hidden: transition=%s elapsed=%" PRIu32 " us",
                 result.hardware_accelerated ? "PPA" : "static", result.elapsed_us);
    }

    void ApplyBrightness(uint8_t percent) override {
        const uint8_t value =
            static_cast<uint8_t>(static_cast<uint32_t>(std::min<uint8_t>(percent, 100U)) * 255U / 100U);
        esp_err_t status = ESP_ERR_INVALID_STATE;
        if (state_.panel_io != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
            status = esp_lcd_panel_io_tx_param(state_.panel_io, 0x51, &value, sizeof(value));
            esp_lv_adapter_unlock();
        }
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "could not set display brightness: %s", esp_err_to_name(status));
        }
    }

   private:
    MosaicoState& state_;
};

class MosaicoPowerBackend final : public device::PowerBackend {
   public:
    void SetPowerButtonSink(device::PowerButtonSink, void*) override {}
    void SetPowerOffButtonSink(device::PowerOffButtonSink, void*) override {}
    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override {
        return std::unexpected(device::PowerError::kUnavailable);
    }
    [[noreturn]] void PowerOff() override {
        (void)ConfigureOutput(kPowerSwitch, 0, GPIO_MODE_OUTPUT_OD);
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
};

class MosaicoHardwareInfoBackend final : public device::HardwareInfoBackend {
   public:
    [[nodiscard]] device::HardwareInfo Snapshot() const override {
        return {
            .board = "ESP-Mosaico V1.0",
            .host_chip = "ESP32-S31",
            .wifi_coprocessor = "Native ESP32-S31",
            .touch_controller = "CST9217",
            .display =
                {
                    .driver = "CO5300",
                    .interface = "QSPI",
                    .pixel_format = "RGB565",
                    .width_pixels = kWidth,
                    .height_pixels = kHeight,
                    .refresh_rate_hz = 60U,
                },
        };
    }
};

class EspMosaicoPlatform final : public Platform {
   public:
    EspMosaicoPlatform() : graphics_(state_), system_ui_(state_) {
        state_.hall_cover_cache.BindUi({
            .context = &state_,
            .show_placeholder = ShowCachedHallPlaceholder,
            .attach = AttachCachedHallCover,
            .detach = DetachCachedHallCover,
            .request_refresh = RefreshCachedHallCover,
        });
        state_.guest_graphics.SetPresentationHooks({
            .context = &state_,
            .prepare_frame_locked =
                [](void* context, lv_obj_t* guest_frame, bool, bool& needs_present) {
                    auto& state = *static_cast<MosaicoState*>(context);
                    if (state.host_root != nullptr) {
                        DeleteHostRootLocked(state);
                        needs_present = true;
                    }
                    lv_obj_move_foreground(guest_frame);
                },
        });
    }

    [[nodiscard]] esp_err_t Initialize() override {
        ESP_RETURN_ON_ERROR(InitializeUsbCdcConsole(), kTag, "initialize Type-C USB CDC console failed");
        ESP_LOGI(kTag, "initializing ESP-Mosaico with official CO5300/CST9217 drivers");
        ESP_RETURN_ON_ERROR(InitializeDisplay(state_), kTag, "initialize Mosaico display failed");
        ESP_RETURN_ON_ERROR(
            state_.panel_transition.Initialize(
                state_.display, kWidth, kHeight, lvgl::square_common::TransitionProfile(ui_profile::kSquareLayout),
                state_.shadow_copy_dma2d, state_.displayed_shadow, &state_.displayed_shadow_valid),
            kTag, "initialize CO5300 direct transition compositor failed");
        ESP_RETURN_ON_ERROR(InitializeTouch(state_), kTag, "initialize Mosaico touch failed");
        ESP_RETURN_ON_ERROR(state_.i2c_executor.Initialize(), kTag, "start shared Mosaico I2C executor failed");
        battery_.Initialize(state_.i2c_bus, state_.i2c_executor);
        ESP_RETURN_ON_ERROR(state_.touch_input.Initialize(state_.touch, state_.i2c_executor), kTag,
                            "bind CST9217 input failed");
        ESP_RETURN_ON_ERROR(state_.guest_graphics.Initialize(state_.display, nullptr), kTag,
                            "initialize shared Guest graphics failed");

        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return ESP_FAIL;
        }
        state_.host_pointer_indev = lv_indev_create();
        if (state_.host_pointer_indev == nullptr) {
            esp_lv_adapter_unlock();
            return ESP_ERR_NO_MEM;
        }
        lv_indev_set_type(state_.host_pointer_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_mode(state_.host_pointer_indev, LV_INDEV_MODE_EVENT);
        lv_indev_set_read_cb(state_.host_pointer_indev, HostPointerRead);
        lv_indev_set_user_data(state_.host_pointer_indev, &state_);
        lv_indev_set_display(state_.host_pointer_indev, state_.display);
        SetHostPointerEnabledLocked(state_, false);

        lv_obj_t* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(kBackgroundRgb), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        state_.host_root = lv_obj_create(screen);
        StyleFullscreen(state_.host_root, kBackgroundRgb);
        lv_obj_t* label = lv_label_create(state_.host_root);
        lv_label_set_text(label, "Starting MicroPixel...");
        lv_obj_set_style_text_color(label, lv_color_hex(0xeaf4ffU), 0);
        lv_obj_set_style_text_font(label, lvgl::BuiltinLatinFont(lvgl::SystemFontRole::kTitle), 0);
        lv_obj_center(label);
        lvgl::RequestDisplayRefresh(state_.display);
        esp_lv_adapter_unlock();

        ESP_RETURN_ON_ERROR(state_.touch_input.Start(state_.display), kTag, "start interrupt-driven touch failed");
        ESP_RETURN_ON_ERROR(
            state_.screen_capture.Start(state_.display, state_.touch_input, state_.local_control, kWidth, kHeight,
                                        {.pixels = state_.displayed_shadow,
                                         .stride = kWidth * 2U,
                                         .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                         .ready = &state_.displayed_shadow_valid}),
            kTag, "start USB screen capture/local control failed");
        ESP_LOGI(kTag, "Mosaico HMI ready: 480x480 QSPI display, interrupt-driven touch, shared Guest renderer");
        return ESP_OK;
    }

    [[nodiscard]] device::GraphicsBackend& graphics() override { return graphics_; }
    [[nodiscard]] device::InputBackend& input() override { return state_.input_router; }
    [[nodiscard]] device::AudioBackend& audio() override { return audio_; }
    [[nodiscard]] device::BatteryBackend& battery() override { return battery_; }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }
    [[nodiscard]] device::WifiBackend& wifi() override { return wifi_; }
    [[nodiscard]] device::PowerBackend& power() override { return power_; }
    [[nodiscard]] device::LocalControlBackend& local_control() override { return state_.local_control; }
    [[nodiscard]] device::DeviceCatalogBackend& devices() override { return devices_; }
    [[nodiscard]] device::SensorBackend& sensors() override { return sensors_; }
    [[nodiscard]] device::GpioBackend& gpio() override { return gpio_; }
    [[nodiscard]] device::HapticsBackend& haptics() override { return haptics_; }
    [[nodiscard]] device::HardwareInfoBackend& hardware_info() override { return hardware_info_; }
    [[nodiscard]] host_ui::SystemUiBackend& system_ui() override { return system_ui_; }
    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.hall_cover_cache.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    MosaicoState state_{};
    MosaicoGraphicsBackend graphics_;
    MosaicoSystemUiBackend system_ui_;
    common::UnavailableAudioBackend audio_{};
    esp_mosaico::BatteryBackend battery_{};
    radio::NativeWifiRadio wifi_radio_{};
    common::wifi::WifiBackend wifi_{wifi_radio_};
    MosaicoPowerBackend power_{};
    common::UnavailableDeviceCatalogBackend devices_{};
    common::UnavailableSensorBackend sensors_{};
    common::UnavailableGpioBackend gpio_{};
    common::UnavailableHapticsBackend haptics_{};
    MosaicoHardwareInfoBackend hardware_info_{};
};

}  // namespace

Platform& ConfiguredPlatform() {
    static EspMosaicoPlatform platform;
    return platform;
}

}  // namespace micropixel::platform
