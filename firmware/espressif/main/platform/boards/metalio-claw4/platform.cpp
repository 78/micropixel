#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "device/graphics.hpp"
#include "draw/lv_draw_buf_private.h"
#include "esp_async_color_convert.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host_ui/system_gesture_router.hpp"
#include "host_ui/system_ui.hpp"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#if CONFIG_MICROPIXEL_SYSTEM_SHELL_SNAPSHOT
#include "src/draw/snapshot/lv_snapshot.h"
#endif
#include "platform/boards/metalio-claw4/audio/audio_backend.hpp"
#include "platform/boards/metalio-claw4/battery_backend.hpp"
#include "platform/boards/metalio-claw4/board_hardware.hpp"
#include "platform/boards/metalio-claw4/device_catalog.hpp"
#include "platform/boards/metalio-claw4/display/display_pipeline.hpp"
#include "platform/boards/metalio-claw4/display/screen_capture.hpp"
#include "platform/boards/metalio-claw4/gpio_backend.hpp"
#include "platform/boards/metalio-claw4/haptics_backend.hpp"
#include "platform/boards/metalio-claw4/input/tca9555_power_key.hpp"
#include "platform/boards/metalio-claw4/perceptual_control.hpp"
#include "platform/boards/metalio-claw4/sensor_backend.hpp"
#include "platform/common/graphics_adapter.hpp"
#include "platform/common/i2c_executor.hpp"
#include "platform/common/system_ui_adapter.hpp"
#include "platform/common/wifi/wifi_backend.hpp"
#include "platform/configured_backends.hpp"
#include "platform/drivers/input/gt911_input.hpp"
#include "platform/lvgl/display/jpeg_cover_decoder.hpp"
#include "platform/lvgl/display/png_cover_decoder.hpp"
#include "platform/lvgl/display/system_transition_compositor.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/lvgl/ui/square_common/hall_card_ui.hpp"
#include "platform/lvgl/ui/square_common/hall_carousel.hpp"
#include "platform/lvgl/ui/square_common/hall_cover_cache.hpp"
#include "platform/lvgl/ui/square_common/hall_cover_codec.hpp"
#include "platform/lvgl/ui/square_common/hall_scene_ui.hpp"
#include "platform/lvgl/ui/square_common/hall_transition_policy.hpp"
#include "platform/lvgl/ui/square_common/profiles/square_720.hpp"
#include "platform/lvgl/ui/square_common/status_layer_transition.hpp"
#include "platform/lvgl/ui/square_common/status_layer_ui.hpp"
#include "platform/lvgl/ui/square_common/system_detail_ui.hpp"
#include "platform/lvgl/ui/square_common/system_menu_ui.hpp"
#include "platform/lvgl/ui/square_common/wifi_settings_ui.hpp"
#include "platform/platform.hpp"
#include "platform/radio/esp_hosted_radio.hpp"
#include "task_policy.hpp"
#include "work/background_executor.hpp"

#if !defined(CONFIG_PM_ENABLE)
#error "Metalio-Claw4 LVGL idle pause requires CONFIG_PM_ENABLE"
#endif

namespace micropixel::platform {
namespace {

namespace ui_profile = lvgl::square_common::profiles::square_720;
using HallCarousel = lvgl::square_common::HallCarouselPolicy<ui_profile::Layout>;

constexpr char kTag[] = "micropixel_platform";
constexpr int kWidth = ui_profile::Layout::kWidth;
constexpr int kHeight = ui_profile::Layout::kHeight;
constexpr BaseType_t kLvglTaskCore = 1;
constexpr uint32_t kRefreshPeriodMs = 1000;
constexpr uint32_t kLvglIdleTimeoutMs = 1000;
constexpr uint32_t kLvglMaximumWaitMs = 120U * 1000U;
constexpr uint32_t kLightSleepEntryAttempts = 3U;
constexpr uint32_t kLvglTaskMinDelayMs = portTICK_PERIOD_MS;
constexpr uint32_t kHostPointerQueueCapacity = 32U;
constexpr uint32_t kThemeAccentColor = 0x287ee8U;
constexpr int32_t kHallCardWidth = HallCarousel::kCardWidth;
constexpr int32_t kHallCardRadius = 22;
constexpr int32_t kHallCardBorderWidth = 1;
constexpr uint32_t kHallCoverBytesPerPixel = 3U;
constexpr uint32_t kHallCoverNativeSize = static_cast<uint32_t>(kHallCardWidth);
constexpr uint32_t kHallCoverNativeStride = kHallCoverNativeSize * kHallCoverBytesPerPixel;
constexpr uint32_t kHallCoverNativeBytes = kHallCoverNativeStride * kHallCoverNativeSize;
constexpr int32_t kHallScrollTrackWidth = ui_profile::Layout::kHallScrollTrackWidth;
constexpr int32_t kHallScrollTrackHeight = ui_profile::Layout::kHallScrollTrackHeight;
constexpr uint32_t kGuestTransitionStride = static_cast<uint32_t>(kWidth) * kHallCoverBytesPerPixel;
constexpr uint32_t kGuestTransitionBytes = kGuestTransitionStride * static_cast<uint32_t>(kHeight);
constexpr uint32_t kGuestTransitionHalfSize = ui_profile::kSquareLayout.transition_intermediate_width;
constexpr uint32_t kGuestTransitionHalfBytes =
    kGuestTransitionHalfSize * kGuestTransitionHalfSize * kHallCoverBytesPerPixel;
constexpr uint32_t kPpaBufferAlignment = 128U;
constexpr uint32_t kGuestTransitionHalfAllocationBytes =
    (kGuestTransitionHalfBytes + kPpaBufferAlignment - 1U) / kPpaBufferAlignment * kPpaBufferAlignment;
constexpr uint32_t kGuestTransitionDurationMs = 100U;
#if CONFIG_MICROPIXEL_LVGL_PPA_ACCEL
constexpr bool kEnablePpaAccel = true;
#else
constexpr bool kEnablePpaAccel = false;
#endif
constexpr esp_lv_adapter_tear_avoid_mode_t kTearAvoidMode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
constexpr const char* kTearAvoidModeName = "double-direct";
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_33;

void* AllocateImageBuffer(size_t size, lv_color_format_t) {
    return heap_caps_aligned_alloc(64U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void FreeImageBuffer(void* buffer) { heap_caps_free(buffer); }

async_color_convert_handle_t& LvglBufferCopyDma2dClient() {
    // LVGL's draw-buffer copy callback has no context parameter. This client is
    // therefore process-lifetime state, just like LVGL's global handlers.
    static async_color_convert_handle_t client{};
    return client;
}

void CopyImageBuffer(lv_draw_buf_t* destination, const lv_area_t* destination_area, const lv_draw_buf_t* source,
                     const lv_area_t* source_area) {
    if (destination == nullptr || source == nullptr || destination->header.cf != source->header.cf) {
        return;
    }
    const int32_t line_width =
        destination_area == nullptr ? destination->header.w : lv_area_get_width(destination_area);
    const int32_t source_width = source_area == nullptr ? source->header.w : lv_area_get_width(source_area);
    const int32_t line_count =
        destination_area == nullptr ? destination->header.h : lv_area_get_height(destination_area);
    const int32_t source_line_count = source_area == nullptr ? source->header.h : lv_area_get_height(source_area);
    if (line_width <= 0 || line_count <= 0 || line_width != source_width || line_count != source_line_count) {
        return;
    }

    if ((destination_area == nullptr || source_area == nullptr) && LV_COLOR_FORMAT_IS_INDEXED(destination->header.cf)) {
        std::memcpy(destination->data, source->data,
                    LV_COLOR_INDEXED_PALETTE_SIZE(destination->header.cf) * sizeof(lv_color32_t));
    }

    auto* destination_data =
        static_cast<uint8_t*>(lv_draw_buf_goto_xy(destination, destination_area == nullptr ? 0 : destination_area->x1,
                                                  destination_area == nullptr ? 0 : destination_area->y1));
    const auto* source_data = static_cast<const uint8_t*>(lv_draw_buf_goto_xy(
        source, source_area == nullptr ? 0 : source_area->x1, source_area == nullptr ? 0 : source_area->y1));
    if (destination_data == nullptr || source_data == nullptr) {
        return;
    }

    async_color_convert_handle_t dma2d = LvglBufferCopyDma2dClient();
    const uint32_t bits_per_pixel = lv_color_format_get_bpp(static_cast<lv_color_format_t>(destination->header.cf));
    if (dma2d != nullptr && bits_per_pixel == 24U && destination->data != source->data &&
        destination->header.stride % 3U == 0U && source->header.stride % 3U == 0U) {
        async_color_convert_request_t copy{};
        copy.src_buffer = source->data;
        copy.src_stride = source->header.stride / 3U;
        copy.src_height = source->header.h;
        copy.src_x = source_area == nullptr ? 0U : static_cast<uint32_t>(source_area->x1);
        copy.src_y = source_area == nullptr ? 0U : static_cast<uint32_t>(source_area->y1);
        copy.dst_buffer = destination->data;
        copy.dst_stride = destination->header.stride / 3U;
        copy.dst_height = destination->header.h;
        copy.dst_x = destination_area == nullptr ? 0U : static_cast<uint32_t>(destination_area->x1);
        copy.dst_y = destination_area == nullptr ? 0U : static_cast<uint32_t>(destination_area->y1);
        copy.copy_width = static_cast<uint32_t>(line_width);
        copy.copy_height = static_cast<uint32_t>(line_count);
        copy.src_color_format = ESP_COLOR_FOURCC_BGR24;
        copy.dst_color_format = ESP_COLOR_FOURCC_BGR24;
        if (esp_color_convert_blocking(dma2d, &copy, -1) == ESP_OK) {
            return;
        }
    }

    const uint32_t line_bytes = (static_cast<uint32_t>(line_width) * bits_per_pixel + 7U) >> 3U;
    for (int32_t line = 0; line < line_count; ++line) {
        std::memcpy(destination_data, source_data, line_bytes);
        destination_data += destination->header.stride;
        source_data += source->header.stride;
    }
}

void* AlignImageBuffer(void* buffer, lv_color_format_t) { return buffer; }

uint32_t ImageWidthToStride(uint32_t width, lv_color_format_t color_format) {
    return LV_DRAW_BUF_STRIDE(width, color_format);
}

uint32_t BitmapPixelRgb888(const device::BitmapView& bitmap, uint32_t x, uint32_t y) {
    const uint8_t* pixel = bitmap.data + y * bitmap.stride + x * 3U;
    return (static_cast<uint32_t>(pixel[2]) << 16U) | (static_cast<uint32_t>(pixel[1]) << 8U) |
           static_cast<uint32_t>(pixel[0]);
}

uint32_t LaunchBackgroundRgb888(const device::BitmapView& bitmap) {
    const uint32_t top_left = BitmapPixelRgb888(bitmap, 0U, 0U);
    const uint32_t top_right = BitmapPixelRgb888(bitmap, bitmap.width - 1U, 0U);
    const uint32_t bottom_left = BitmapPixelRgb888(bitmap, 0U, bitmap.height - 1U);
    const uint32_t bottom_right = BitmapPixelRgb888(bitmap, bitmap.width - 1U, bitmap.height - 1U);
    if (top_left == top_right || top_left == bottom_left || top_left == bottom_right) {
        return top_left;
    }
    if (top_right == bottom_left || top_right == bottom_right) {
        return top_right;
    }
    return bottom_left == bottom_right ? bottom_left : top_left;
}

void MaskHallCoverCorners(uint8_t* destination) {
    // Bake the four small corner masks into the retained bitmap instead of
    // making LVGL allocate a card-sized rounded clipping layer on every redraw.
    // The cover is drawn full-bleed below the card's post-drawn border, so its
    // baked corner mask must use the same outer radius as the card.
    lvgl::square_common::MaskHallCoverRgb888(destination, kHallCoverNativeSize, static_cast<uint32_t>(kHallCardRadius),
                                             0x08111fU, 0x111f32U);
}

constexpr size_t kHallAppTextCapacity = 65U;

struct HallAppPresentation final {
    std::array<char, kHallAppTextCapacity> app_id{};
    std::array<char, kHallAppTextCapacity> display_name{};
    bool running{};
};

// All board-owned display/input state is stored inside the selected Platform
// object. Callbacks receive this state explicitly instead of reaching through
// file-level aliases.
struct MetalioClaw4PlatformState final {
    metalio_claw4::BoardHardware hardware{kWidth, kHeight};
    metalio_claw4::MetalioClaw4DisplayPipeline display_pipeline{hardware, kWidth, kHeight};
    common::I2cExecutor i2c_executor{};
    metalio_claw4::BatteryBackend battery{};
    metalio_claw4::SensorBackend sensors{};
    metalio_claw4::GpioBackend gpio{};
    metalio_claw4::HapticsBackend haptics{};
    metalio_claw4::DeviceCatalog devices{};
    lv_display_t* display{};
    lv_obj_t* host_smoke{};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts};
    lvgl::SystemTransitionCompositor system_transition{};
    lvgl::square_common::HallSceneUi hall_scene_ui{};
    lv_image_dsc_t launch_image_descriptor{};
    lv_image_dsc_t hall_cover_descriptors[host_ui::kMaxHallApps]{};
    lvgl::square_common::HallCoverCache hall_cover_cache{{
        .target_size = kHallCoverNativeSize,
        .corner_radius = static_cast<uint32_t>(kHallCardRadius),
        .top_background_rgb = 0x08111fU,
        .bottom_background_rgb = 0x111f32U,
    }};
    std::array<host_ui::HallCoverModel, host_ui::kMaxHallApps> hall_cover_sources{};
    // The visible source for a suspended App is its Guest snapshot. Retain the
    // installed cover separately so transition baselines can always render a
    // Hall with no running cards.
    std::array<host_ui::HallCoverModel, host_ui::kMaxHallApps> hall_idle_cover_sources{};
    std::array<HallAppPresentation, host_ui::kMaxHallApps> hall_app_presentations{};
    lv_obj_t* hall_cover_images[host_ui::kMaxHallApps]{};
    lv_obj_t* hall_cover_placeholders[host_ui::kMaxHallApps]{};
    uint32_t hall_card_window_first{};
    uint32_t hall_card_window_last{};
    lv_obj_t* hall_cards[host_ui::kMaxHallApps]{};
    lv_obj_t* hall_card_press_overlays[host_ui::kMaxHallApps]{};
    metalio_claw4::Tca9555PowerKey power_key{};
    drivers::Gt911Input touch_input{kWidth, kHeight, kTouchInterrupt};
    host_ui::SystemGestureRouter input_router{touch_input, kWidth, kHeight};
    lvgl::square_common::SystemDetailUi system_detail_ui{ui_profile::kSystemPageLayout};
    lvgl::square_common::SystemMenuUi system_menu_ui{};
    lvgl::square_common::StatusLayerUi status_layer_ui{};
    lvgl::square_common::WifiSettingsUi wifi_settings_ui{};
    lv_indev_t* host_pointer_indev{};
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
    uint8_t* guest_snapshot_pixels{};
    uint8_t* guest_transition_pixels{};
    host_ui::SystemUiActionSink hall_action_sink{};
    void* hall_action_context{};
    uint32_t hall_app_count{};
    int32_t hall_scroll_offset{};
    uint64_t hall_catalog_signature{};
    host_ui::HallStatusBarModel hall_status_bar{};
    bool hall_firmware_update_available{};
    bool hall_status_bar_valid{};
    bool hall_launch_enabled{};
    bool hall_retained_for_status{};
    bool hall_app_running[host_ui::kMaxHallApps]{};
    bool guest_snapshot_in_hall{};
};

void SyncHallCardWindowLocked(MetalioClaw4PlatformState& state);
void RequestHallCoverWindowLocked(MetalioClaw4PlatformState& state, bool force = false);
void HallCarouselEvent(lv_event_t* event);
void HallCardEvent(lv_event_t* event);
void HallCloseButtonEvent(lv_event_t* event);

void HostPointerRead(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(lv_indev_get_user_data(indev));
    if (state == nullptr || data == nullptr) {
        return;
    }
    bool pointer_released = false;
    bool pointer_sample_read = false;
    portENTER_CRITICAL(&state->host_pointer_lock);
    if (state->host_pointer_queue_tail != state->host_pointer_queue_head) {
        const device::TouchSample& sample = state->host_pointer_queue[state->host_pointer_queue_tail];
        pointer_sample_read = true;
        state->host_pointer_queue_tail = (state->host_pointer_queue_tail + 1U) % kHostPointerQueueCapacity;
        state->host_pointer_point = lv_point_t{.x = sample.x, .y = sample.y};
        state->host_pointer_state =
            sample.phase == device::TouchPhase::kDown || sample.phase == device::TouchPhase::kMove
                ? LV_INDEV_STATE_PRESSED
                : LV_INDEV_STATE_RELEASED;
        if (sample.phase == device::TouchPhase::kUp || sample.phase == device::TouchPhase::kCancel) {
            state->host_pointer_release_queued = false;
            pointer_released = true;
        }
    }
    data->point = state->host_pointer_point;
    data->state = state->host_pointer_enabled ? state->host_pointer_state : LV_INDEV_STATE_RELEASED;
    data->continue_reading = state->host_pointer_queue_tail != state->host_pointer_queue_head;
    portEXIT_CRITICAL(&state->host_pointer_lock);
    if (pointer_sample_read && state->display != nullptr) {
        lvgl::RequestDisplayRefresh(state->display);
    }
    if (pointer_released) {
        state->wifi_settings_ui.PointerReleased();
    }
}

bool HostPointerTouchSink(void* context, const device::TouchSample& sample) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(context);
    if (state == nullptr) {
        return true;
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

void SetHostPointerEnabledLocked(MetalioClaw4PlatformState& state, bool enabled) {
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

bool HostPointerBusy(MetalioClaw4PlatformState& state) {
    portENTER_CRITICAL(&state.host_pointer_lock);
    const bool busy = state.host_pointer_touch_active || state.host_pointer_release_queued ||
                      state.host_pointer_queue_head != state.host_pointer_queue_tail;
    portEXIT_CRITICAL(&state.host_pointer_lock);
    return busy;
}

void StyleFullscreenContainer(lv_obj_t* container, uint32_t background) {
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, kWidth, kHeight);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE);
}

constexpr const char* GraphicsBackendName() { return "retained-objects"; }

void CreateStartingScreen(MetalioClaw4PlatformState& state) {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x08111f), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    state.host_smoke = lv_obj_create(screen);
    StyleFullscreenContainer(state.host_smoke, 0x08111f);

    lv_obj_t* label = lv_label_create(state.host_smoke);
    lv_label_set_text(label, "Starting...");
    lv_obj_set_style_text_color(label, lv_color_hex(0xeaf4ff), 0);
    lv_obj_set_style_text_font(label, lvgl::BuiltinLatinFont(lvgl::SystemFontRole::kTitle), 0);
    lv_obj_center(label);
}

esp_err_t InitializeLvgl(MetalioClaw4PlatformState& state) {
    esp_lv_adapter_config_t adapter_config{};
    adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_config.stack_in_psram = false;
    adapter_config.task_priority = task_policy::kDisplayPriority;
    adapter_config.task_core_id = kLvglTaskCore;
    adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_MONOTONIC;
    // A sub-tick timeout is rounded down to zero by pdMS_TO_TICKS().  LVGL's
    // animation timer can then keep this high-priority task in a busy loop and
    // starve IDLE1 until the task watchdog fires.
    adapter_config.task_min_delay_ms = kLvglTaskMinDelayMs;
    adapter_config.task_max_delay_ms = kLvglMaximumWaitMs;
    adapter_config.auto_sleep.enable = true;
    adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_PAUSE;
    adapter_config.auto_sleep.idle_timeout_ms = kLvglIdleTimeoutMs;
    esp_err_t status = esp_lv_adapter_init(&adapter_config);
    if (status != ESP_OK) {
        return status;
    }
    async_color_convert_config_t copy_config{};
    copy_config.backlog = 1U;
    copy_config.dma_burst_size = 128U;
    status = esp_async_color_convert_install_dma2d(&copy_config, &LvglBufferCopyDma2dClient());
    if (status != ESP_OK) {
        LvglBufferCopyDma2dClient() = nullptr;
        ESP_LOGW(kTag, "LVGL draw-buffer DMA2D copy unavailable; using CPU fallback: %s", esp_err_to_name(status));
    }
    lv_draw_buf_handlers_init(lv_draw_buf_get_image_handlers(), AllocateImageBuffer, FreeImageBuffer, CopyImageBuffer,
                              AlignImageBuffer, nullptr, nullptr, ImageWidthToStride);
    lv_draw_buf_get_handlers()->buf_copy_cb = CopyImageBuffer;

    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.display_pipeline.Panel();
    display_config.panel_io = state.display_pipeline.PanelIo();
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI;
    display_config.profile.hor_res = kWidth;
    display_config.profile.ver_res = kHeight;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#ifdef CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.enable_ppa_accel = kEnablePpaAccel;
    display_config.profile.require_double_buffer = true;
    display_config.tear_avoid_mode = kTearAvoidMode;
    state.display = esp_lv_adapter_register_display(&display_config);
    if (state.display == nullptr) {
        return ESP_FAIL;
    }
    state.display_pipeline.BindLvgl(state.display);
    status = state.guest_graphics.Initialize(state.display, state.display_pipeline.DirectFramebuffers());
    if (status != ESP_OK) {
        return status;
    }
    status =
        state.system_transition.Initialize(state.display, state.display_pipeline.DirectFramebuffers(), kWidth, kHeight,
                                           lvgl::square_common::TransitionProfile(ui_profile::kSquareLayout));
    if (status != ESP_OK) {
        return status;
    }
    lv_timer_set_period(lv_display_get_refr_timer(state.display), kRefreshPeriodMs);

    state.host_pointer_indev = lv_indev_create();
    if (state.host_pointer_indev == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(state.host_pointer_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_mode(state.host_pointer_indev, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(state.host_pointer_indev, HostPointerRead);
    lv_indev_set_user_data(state.host_pointer_indev, &state);
    lv_indev_set_display(state.host_pointer_indev, state.display);
    SetHostPointerEnabledLocked(state, false);

    status = state.touch_input.Start(state.display);
    if (status != ESP_OK) {
        return status;
    }
    CreateStartingScreen(state);
    lvgl::RequestDisplayRefresh(state.display);
    status = esp_lv_adapter_start();
    if (status == ESP_OK) {
        status = metalio_claw4::InitializeScreenCapture(state.display, state.touch_input, kWidth, kHeight);
    }
    return status;
}

}  // namespace

namespace {

esp_err_t InitializePlatformImpl(MetalioClaw4PlatformState& state) {
    ESP_LOGI(kTag, "initializing Metalio-Claw4 LVGL backend");
    esp_err_t status = state.hardware.Initialize();
    if (status == ESP_OK) {
        status = state.i2c_executor.Initialize();
    }
    if (status == ESP_OK) {
        state.battery.Initialize(state.hardware.I2cBus(), state.hardware.IoExpander(), state.i2c_executor);
        state.power_key.SetPowerInputChangeSink(
            [](void* context) { static_cast<metalio_claw4::BatteryBackend*>(context)->NotifyExternalPowerChanged(); },
            &state.battery);
        status = state.power_key.Initialize(state.hardware.IoExpander(), state.i2c_executor);
    }
    if (status == ESP_OK) {
        state.sensors.Initialize(state.hardware.I2cBus(), state.i2c_executor);
        status = state.gpio.Initialize();
    }
    if (status == ESP_OK) {
        status = state.haptics.Initialize();
    }
    if (status == ESP_OK) {
        state.devices.Initialize(state.sensors.acceleration_available(), state.sensors.magnetic_field_available());
    }
    if (status == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        status = state.touch_input.Initialize(state.hardware.I2cBus(), state.i2c_executor);
    }
    if (status == ESP_OK) {
        status = InitializeLvgl(state);
    }
    if (status == ESP_OK) {
        status = state.display_pipeline.SetBrightness(metalio_claw4::kPerceptualControlScale);
    }
    if (status == ESP_OK) {
        status = metalio_claw4::ConfiguredAudioBackend().Initialize(state.hardware.IoExpander(), state.i2c_executor);
    }
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "Metalio-Claw4 LVGL backend failed: %s", esp_err_to_name(status));
        return status;
    }

    ESP_LOGI(kTag,
             "Metalio-Claw4 LVGL backend ready: %dx%d RGB888, %s, %lu ms idle refresh, "
             "GT911 IRQ GPIO%d, power-key=TCA9555-P0.5/IRQ-GPIO2, graphics=%s, ppa=%s, lvgl-core=%d priority=%u",
             kWidth, kHeight, kTearAvoidModeName, static_cast<unsigned long>(kRefreshPeriodMs),
             static_cast<int>(kTouchInterrupt), GraphicsBackendName(), kEnablePpaAccel ? "on" : "off", kLvglTaskCore,
             static_cast<unsigned>(task_policy::kDisplayPriority));
    return ESP_OK;
}

std::expected<void, device::PowerError> RestoreDisplayAfterSleep(MetalioClaw4PlatformState& state) {
    const esp_err_t resume_status = state.display_pipeline.Resume();
    if (resume_status != ESP_OK) {
        ESP_LOGE(kTag, "display reinitialization failed after sleep: %s", esp_err_to_name(resume_status));
        return std::unexpected(device::PowerError::kDisplayRestore);
    }

    state.guest_graphics.RebindFramebuffers(state.display_pipeline.DirectFramebuffers());
    state.system_transition.RebindFramebuffers(state.display_pipeline.DirectFramebuffers());
    const esp_err_t status =
        esp_lv_adapter_sleep_recover(state.display, state.hardware.Panel(), state.hardware.PanelIo());
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "LVGL display recovery failed after sleep: %s", esp_err_to_name(status));
        return std::unexpected(device::PowerError::kDisplayRestore);
    }
    return {};
}

std::expected<void, device::PowerError> EnterLowPowerImpl(MetalioClaw4PlatformState& state) {
    esp_err_t status = esp_lv_adapter_sleep_prepare();
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "LVGL sleep preparation failed: %s", esp_err_to_name(status));
        return std::unexpected(device::PowerError::kDisplayPrepare);
    }

    status = state.display_pipeline.Suspend();
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "display shutdown failed: %s", esp_err_to_name(status));
        (void)RestoreDisplayAfterSleep(state);
        return std::unexpected(device::PowerError::kDisplayShutdown);
    }

    const auto power_press_pending = [&state] {
        return state.power_key.PowerPressOccurredAfterRequest() || state.power_key.IsPressed();
    };
    const auto cancel_sleep_entry = [&state]() -> std::expected<void, device::PowerError> {
        ESP_LOGI(kTag, "light sleep canceled by a power press during the entry transition");
        state.power_key.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
        return RestoreDisplayAfterSleep(state);
    };

    if (power_press_pending()) {
        return cancel_sleep_entry();
    }

    for (uint32_t attempt = 1U; attempt <= kLightSleepEntryAttempts; ++attempt) {
        status = state.power_key.PrepareForLightSleep();
        if (status != ESP_OK) {
            if (power_press_pending()) {
                return cancel_sleep_entry();
            }
            ESP_LOGE(kTag, "power-key wake source preparation failed: %s", esp_err_to_name(status));
            const auto restore = RestoreDisplayAfterSleep(state);
            if (!restore.has_value()) {
                return restore;
            }
            return std::unexpected(device::PowerError::kWakeSource);
        }
        if (power_press_pending()) {
            return cancel_sleep_entry();
        }

        ESP_LOGI(kTag, "entering light sleep; power key is the wake source (attempt=%" PRIu32 ")", attempt);
        const int64_t sleep_started_us = esp_timer_get_time();
        status = esp_light_sleep_start();
        const uint64_t sleep_duration_us = static_cast<uint64_t>(esp_timer_get_time() - sleep_started_us);
        if (status == ESP_OK) {
            const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
            const uint64_t gpio_wakeup_status = esp_sleep_get_gpio_wakeup_status();
            ESP_LOGI(kTag,
                     "light sleep returned: status=ESP_OK duration=%" PRIu64 " ms causes=0x%08" PRIx32
                     " gpio=0x%016" PRIx64,
                     sleep_duration_us / 1000U, wake_causes, gpio_wakeup_status);
            state.power_key.GuardWakeButtonUntilRelease(static_cast<uint64_t>(esp_timer_get_time()) + 2000000U);
            break;
        }

        ESP_LOGW(kTag, "light sleep entry rejected: attempt=%" PRIu32 " status=%s duration=%" PRIu64 " ms", attempt,
                 status == ESP_ERR_SLEEP_REJECT ? "ESP_ERR_SLEEP_REJECT" : esp_err_to_name(status),
                 sleep_duration_us / 1000U);
        if (power_press_pending()) {
            return cancel_sleep_entry();
        }
        if (status != ESP_ERR_SLEEP_REJECT || attempt == kLightSleepEntryAttempts) {
            break;
        }
        ESP_LOGI(kTag, "retrying light sleep after clearing a transient wake condition");
    }

    const auto restore = RestoreDisplayAfterSleep(state);
    if (!restore.has_value()) {
        return restore;
    }
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "light sleep rejected: %s", esp_err_to_name(status));
        return std::unexpected(device::PowerError::kSleepRejected);
    }
    ESP_LOGI(kTag, "light sleep cycle complete");
    return {};
}

void ResetHallImageDescriptorsLocked(MetalioClaw4PlatformState& state) {
    state.hall_scene_ui.ResetLocked();
    state.hall_card_window_first = host_ui::kMaxHallApps;
    state.hall_card_window_last = host_ui::kMaxHallApps;
    for (uint32_t index = 0U; index < host_ui::kMaxHallApps; ++index) {
        state.hall_cards[index] = nullptr;
        state.hall_card_press_overlays[index] = nullptr;
        state.hall_cover_images[index] = nullptr;
        state.hall_cover_placeholders[index] = nullptr;
        state.hall_app_running[index] = false;
        auto& descriptor = state.hall_cover_descriptors[index];
        if (descriptor.data != nullptr) {
            lv_image_cache_drop(&descriptor);
            lv_image_header_cache_drop(&descriptor);
        }
        descriptor = {};
    }
}

bool DismissHostSmokeLocked(MetalioClaw4PlatformState& state) {
    if (state.host_smoke == nullptr) {
        return false;
    }
    state.touch_input.ClearSmokeUi();
    lv_obj_delete(state.host_smoke);
    state.host_smoke = nullptr;
    state.launch_image_descriptor = {};
    ResetHallImageDescriptorsLocked(state);
    return true;
}

int32_t ShowLaunchBitmapImpl(MetalioClaw4PlatformState& state, const device::BitmapView& bitmap) {
    const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 3U;
    if (state.display == nullptr || state.host_smoke == nullptr || bitmap.data == nullptr ||
        (!esp_ptr_in_drom(bitmap.data) && !esp_ptr_external_ram(bitmap.data)) ||
        (bitmap.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
         bitmap.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888) ||
        bitmap.width == 0U || bitmap.height == 0U || bitmap.width > static_cast<uint32_t>(kWidth) ||
        bitmap.height > static_cast<uint32_t>(kHeight) || bitmap.stride != bitmap.width * bytes_per_pixel ||
        bitmap.size != bitmap.stride * bitmap.height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (esp_ptr_external_ram(bitmap.data)) {
        lv_draw_buf_t draw_buf{};
        if (lv_draw_buf_init(&draw_buf, bitmap.width, bitmap.height,
                             bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? LV_COLOR_FORMAT_ARGB8888
                                                                                     : LV_COLOR_FORMAT_RGB888,
                             bitmap.stride, const_cast<uint8_t*>(bitmap.data), bitmap.size) != LV_RESULT_OK) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        lv_draw_buf_flush_cache(&draw_buf, nullptr);
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    state.touch_input.ClearSmokeUi();
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    const uint32_t launch_background =
        bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888 ? LaunchBackgroundRgb888(bitmap) : 0x08111fU;
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(launch_background), 0);
    state.launch_image_descriptor = {};
    state.launch_image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    state.launch_image_descriptor.header.cf =
        bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB888;
    state.launch_image_descriptor.header.w = bitmap.width;
    state.launch_image_descriptor.header.h = bitmap.height;
    state.launch_image_descriptor.header.stride = bitmap.stride;
    state.launch_image_descriptor.data_size = bitmap.size;
    state.launch_image_descriptor.data = bitmap.data;
    lv_obj_t* image = lv_image_create(state.host_smoke);
    lv_image_set_src(image, &state.launch_image_descriptor);
    lv_obj_center(image);
    lv_obj_t* label = lv_label_create(state.host_smoke);
    lv_label_set_text(label, "Loading...");
    lv_obj_set_style_text_color(label, lv_color_hex(0xeaf4ff), 0);
    lv_obj_set_style_text_font(label, lvgl::BuiltinLatinFont(lvgl::SystemFontRole::kLarge), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -70);
    lvgl::RequestDisplayRefresh(state.display);
    esp_lv_adapter_unlock();
    ESP_LOGI(kTag, "Bundle launch bitmap visible: %" PRIu32 "x%" PRIu32 " format=%s source=%s:%p bytes=%" PRIu32,
             bitmap.width, bitmap.height,
             bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? "ARGB8888" : "RGB888",
             esp_ptr_external_ram(bitmap.data) ? "psram" : "flash-mmap", bitmap.data, bitmap.size);
    return MICROPIXEL_STATUS_OK;
}

void DismissLaunchBitmapImpl(MetalioClaw4PlatformState& state) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    // AppSession outlives its launch screen. After a suspended Guest returns
    // to the Hall, host_smoke belongs to the Hall rather than that session.
    // Only delete the container while it still carries this session's launch
    // descriptor; otherwise stopping the suspended Guest would tear down the
    // newly rendered Hall just before the replacement App starts.
    if (state.launch_image_descriptor.data != nullptr && DismissHostSmokeLocked(state)) {
        lvgl::RequestDisplayRefresh(state.display);
    }
    esp_lv_adapter_unlock();
}

struct HallCardBounds final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

HallCardBounds HallCard(uint32_t index, int32_t scroll_offset) {
    return HallCardBounds{.x = HallCarousel::CardX(index, scroll_offset),
                          .y = HallCarousel::kTop,
                          .width = kHallCardWidth,
                          .height = HallCarousel::kCardHeight};
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

void UpdateHallCarouselLocked(MetalioClaw4PlatformState& state, int32_t offset) {
    state.hall_scroll_offset = HallCarousel::ClampOffset(state.hall_app_count, offset);
    state.hall_scene_ui.UpdateScrollLocked(state.hall_app_count, state.hall_scroll_offset);
    SyncHallCardWindowLocked(state);
    // Decoding runs on the bounded background worker. Submit the newly visible
    // window immediately, including during drag and inertia, so covers can
    // appear before motion finishes instead of leaving placeholders behind.
    RequestHallCoverWindowLocked(state);
}

bool ValidHallCover(const host_ui::HallCoverModel& cover) {
    if (cover.data == nullptr || (!esp_ptr_in_drom(cover.data) && !esp_ptr_external_ram(cover.data)) ||
        cover.width == 0U || cover.height == 0U || cover.size == 0U) {
        return false;
    }
    if (cover.format == host_ui::HallCoverFormat::kJpeg || cover.format == host_ui::HallCoverFormat::kPng) {
        return esp_ptr_in_drom(cover.data) && cover.stride == 0U;
    }
    const uint64_t expected_stride = static_cast<uint64_t>(cover.width) * 3U;
    const uint64_t expected_size = expected_stride * cover.height;
    return expected_stride == cover.stride && expected_size == cover.size;
}

bool PrepareNativeHallCover(MetalioClaw4PlatformState& state, const host_ui::HallCoverModel& source, uint32_t index,
                            host_ui::HallCoverModel& prepared) {
    // A suspended Guest snapshot is already a native Hall thumbnail and stays
    // alive until AppController resumes or stops that Guest.
    if (source.format == host_ui::HallCoverFormat::kRgb888 && source.data == state.guest_snapshot_pixels &&
        source.width == kHallCoverNativeSize && source.height == kHallCoverNativeSize &&
        source.stride == kHallCoverNativeStride && source.size == kHallCoverNativeBytes) {
        prepared = source;
        return true;
    }
    if (index >= host_ui::kMaxHallApps) {
        return false;
    }
    return index < host_ui::kMaxHallApps && state.hall_cover_cache.PrepareSource(source, prepared);
}

void ShowHallCoverPlaceholderLocked(MetalioClaw4PlatformState& state, uint32_t index) {
    if (index >= host_ui::kMaxHallApps) {
        return;
    }
    if (state.hall_cover_images[index] != nullptr) {
        lv_obj_add_flag(state.hall_cover_images[index], LV_OBJ_FLAG_HIDDEN);
    }
    if (state.hall_cover_placeholders[index] != nullptr) {
        lv_obj_remove_flag(state.hall_cover_placeholders[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void AttachHallCoverLocked(MetalioClaw4PlatformState& state, uint32_t index, const host_ui::HallCoverModel& cover) {
    if (index >= state.hall_app_count || cover.data == nullptr || state.hall_cover_images[index] == nullptr) {
        return;
    }
    auto& descriptor = state.hall_cover_descriptors[index];
    if (descriptor.data != nullptr && descriptor.data != cover.data) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
    }
    descriptor = {};
    descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf = LV_COLOR_FORMAT_RGB888;
    descriptor.header.w = kHallCoverNativeSize;
    descriptor.header.h = kHallCoverNativeSize;
    descriptor.header.stride = cover.stride;
    descriptor.data_size = cover.size;
    descriptor.data = cover.data;
    lv_image_set_src(state.hall_cover_images[index], &descriptor);
    lv_obj_remove_flag(state.hall_cover_images[index], LV_OBJ_FLAG_HIDDEN);
    if (state.hall_cover_placeholders[index] != nullptr) {
        lv_obj_add_flag(state.hall_cover_placeholders[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void ShowCachedHallPlaceholder(void* context, uint32_t app_index) {
    ShowHallCoverPlaceholderLocked(*static_cast<MetalioClaw4PlatformState*>(context), app_index);
}

void AttachCachedHallCover(void* context, uint32_t app_index, const host_ui::HallCoverModel& cover) {
    AttachHallCoverLocked(*static_cast<MetalioClaw4PlatformState*>(context), app_index, cover);
}

void DetachCachedHallCover(void* context, uint32_t app_index) {
    auto& state = *static_cast<MetalioClaw4PlatformState*>(context);
    if (app_index >= host_ui::kMaxHallApps) {
        return;
    }
    auto& descriptor = state.hall_cover_descriptors[app_index];
    if (descriptor.data != nullptr) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
        descriptor = {};
    }
    ShowHallCoverPlaceholderLocked(state, app_index);
}

void RefreshCachedHallCover(void* context) {
    auto& state = *static_cast<MetalioClaw4PlatformState*>(context);
    if (state.display != nullptr) {
        lvgl::RequestDisplayRefresh(state.display);
    }
}

void RequestHallCoverWindowLocked(MetalioClaw4PlatformState& state, bool force) {
    if (state.hall_app_count == 0U || state.hall_scene_ui.objects().carousel_content == nullptr) {
        return;
    }
    const uint32_t first = HallCarousel::CoverWindowFirst(state.hall_app_count, state.hall_scroll_offset);
    const uint32_t last = HallCarousel::CoverWindowLast(state.hall_app_count, state.hall_scroll_offset);
    state.hall_cover_cache.RequestWindow(first, last, force);
}

void DrawHallCard(MetalioClaw4PlatformState& state, lv_obj_t* parent, const HallAppPresentation& app, uint32_t index) {
    const HallCardBounds bounds = HallCard(index, 0);
    lvgl::square_common::HallCardObjects objects{};
    lvgl::square_common::DrawHallCard(
        parent, ui_profile::kHallCardLayout,
        {.app_id = app.app_id.data(), .display_name = app.display_name.data(), .running = app.running}, index,
        HallCardEvent, HallCloseButtonEvent, &state, objects);
    state.hall_cards[index] = objects.card;
    state.hall_cover_images[index] = objects.cover_image;
    state.hall_cover_placeholders[index] = objects.cover_placeholder;
    state.hall_card_press_overlays[index] = objects.press_overlay;
    lv_obj_set_pos(objects.card, bounds.x - HallCarousel::kLeft, 0);

    host_ui::HallCoverModel prepared_cover{};
    const host_ui::HallCoverModel& source = state.hall_cover_sources[index];
    if (ValidHallCover(source) && PrepareNativeHallCover(state, source, index, prepared_cover)) {
        AttachHallCoverLocked(state, index, prepared_cover);
    }
    state.hall_app_running[index] = app.running;
}

void DestroyHallCardLocked(MetalioClaw4PlatformState& state, uint32_t index) {
    if (index >= host_ui::kMaxHallApps || state.hall_cards[index] == nullptr) {
        return;
    }
    auto& descriptor = state.hall_cover_descriptors[index];
    if (descriptor.data != nullptr) {
        lv_image_cache_drop(&descriptor);
        lv_image_header_cache_drop(&descriptor);
        descriptor = {};
    }
    lv_obj_delete(state.hall_cards[index]);
    state.hall_cards[index] = nullptr;
    state.hall_card_press_overlays[index] = nullptr;
    state.hall_cover_images[index] = nullptr;
    state.hall_cover_placeholders[index] = nullptr;
}

uint32_t RunningHallAppIndex(const MetalioClaw4PlatformState& state) {
    for (uint32_t index = 0U; index < state.hall_app_count; ++index) {
        if (state.hall_app_running[index]) {
            return index;
        }
    }
    return host_ui::kMaxHallApps;
}

bool PrepareCleanHallBackgroundLocked(MetalioClaw4PlatformState& state, uint32_t running_index) {
    if (running_index >= host_ui::kMaxHallApps || state.hall_cards[running_index] == nullptr ||
        state.hall_scene_ui.objects().carousel_content == nullptr) {
        return state.system_transition.PrepareBackgroundLocked(state.host_smoke);
    }

    const host_ui::HallCoverModel running_cover = state.hall_cover_sources[running_index];
    DestroyHallCardLocked(state, running_index);
    state.hall_cover_sources[running_index] = state.hall_idle_cover_sources[running_index];
    state.hall_app_presentations[running_index].running = false;
    DrawHallCard(state, state.hall_scene_ui.objects().carousel_content, state.hall_app_presentations[running_index],
                 running_index);

    const bool prepared = state.system_transition.PrepareBackgroundLocked(state.host_smoke);

    DestroyHallCardLocked(state, running_index);
    state.hall_cover_sources[running_index] = running_cover;
    state.hall_app_presentations[running_index].running = true;
    DrawHallCard(state, state.hall_scene_ui.objects().carousel_content, state.hall_app_presentations[running_index],
                 running_index);
    return prepared;
}

bool PrepareCleanHallBackgroundLocked(MetalioClaw4PlatformState& state) {
    return PrepareCleanHallBackgroundLocked(state, RunningHallAppIndex(state));
}

void SyncHallCardWindowLocked(MetalioClaw4PlatformState& state) {
    if (state.hall_scene_ui.objects().carousel_content == nullptr) {
        return;
    }
    const uint32_t first = HallCarousel::CoverWindowFirst(state.hall_app_count, state.hall_scroll_offset);
    const uint32_t last = HallCarousel::CoverWindowLast(state.hall_app_count, state.hall_scroll_offset);
    if (first == state.hall_card_window_first && last == state.hall_card_window_last) {
        return;
    }
    for (uint32_t index = 0U; index < state.hall_app_count; ++index) {
        if (state.hall_cards[index] != nullptr && (index < first || index >= last)) {
            DestroyHallCardLocked(state, index);
        }
    }
    for (uint32_t index = first; index < last; ++index) {
        if (state.hall_cards[index] == nullptr) {
            DrawHallCard(state, state.hall_scene_ui.objects().carousel_content, state.hall_app_presentations[index],
                         index);
        }
    }
    state.hall_card_window_first = first;
    state.hall_card_window_last = last;
}

uint32_t FindHallCardIndex(const MetalioClaw4PlatformState& state, const lv_obj_t* card) {
    for (uint32_t index = 0U; index < state.hall_app_count; ++index) {
        if (state.hall_cards[index] == card) {
            return index;
        }
    }
    return host_ui::kMaxHallApps;
}

void SetHallPressOverlayLocked(lv_obj_t* overlay, bool pressed) {
    if (overlay == nullptr) {
        return;
    }
    if (pressed) {
        lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(overlay);
    } else {
        lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

void HallCarouselEvent(lv_event_t* event) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(lv_event_get_user_data(event));
    lv_obj_t* viewport = lv_event_get_current_target_obj(event);
    if (state == nullptr || viewport == nullptr || viewport != state->hall_scene_ui.objects().carousel_viewport) {
        return;
    }
    UpdateHallCarouselLocked(*state, lv_obj_get_scroll_x(viewport));
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
        RequestHallCoverWindowLocked(*state, true);
        if (RunningHallAppIndex(*state) < state->hall_app_count) {
            const int64_t started_us = esp_timer_get_time();
            const bool prepared = PrepareCleanHallBackgroundLocked(*state);
            ESP_LOGI(kTag, "clean Hall transition baseline refreshed after scroll: ready=%s elapsed=%" PRIu32 " us",
                     prepared ? "yes" : "no", static_cast<uint32_t>(esp_timer_get_time() - started_us));
        }
    }
    if (state->display != nullptr) {
        lvgl::RequestDisplayRefresh(state->display);
    }
}

void HallCardEvent(lv_event_t* event) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(lv_event_get_user_data(event));
    lv_obj_t* card = lv_event_get_current_target_obj(event);
    if (state == nullptr || card == nullptr) {
        return;
    }
    const uint32_t index = FindHallCardIndex(*state, card);
    if (index >= state->hall_app_count) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        SetHallPressOverlayLocked(state->hall_card_press_overlays[index], true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        SetHallPressOverlayLocked(state->hall_card_press_overlays[index], false);
    } else if (code == LV_EVENT_SHORT_CLICKED && state->hall_launch_enabled && state->hall_action_sink != nullptr) {
        const int32_t reveal_offset =
            HallCarousel::RevealOffset(state->hall_app_count, state->hall_scroll_offset, index);
        lv_obj_t* viewport = state->hall_scene_ui.objects().carousel_viewport;
        if (reveal_offset != state->hall_scroll_offset && viewport != nullptr) {
            // The transition compositor requires the complete square cover to
            // be on-screen. Preserve that fully revealed position as the Hall
            // baseline before handing the launch request to FirmwareApp.
            lv_obj_scroll_to_x(viewport, reveal_offset, LV_ANIM_OFF);
            UpdateHallCarouselLocked(*state, lv_obj_get_scroll_x(viewport));
        }
        ESP_LOGI(kTag, "App Hall accepted native card tap: card=%" PRIu32, index);
        state->hall_action_sink(
            state->hall_action_context,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchApp, .app_index = index});
    }
}

void HallCloseButtonEvent(lv_event_t* event) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(lv_event_get_user_data(event));
    lv_obj_t* close = lv_event_get_current_target_obj(event);
    lv_obj_t* card = close != nullptr ? lv_obj_get_parent(close) : nullptr;
    if (state == nullptr || card == nullptr) {
        return;
    }
    const uint32_t index = FindHallCardIndex(*state, card);
    if (index < state->hall_app_count && state->hall_app_running[index] && state->hall_action_sink != nullptr) {
        ESP_LOGI(kTag, "App Hall accepted native close tap: card=%" PRIu32, index);
        state->hall_action_sink(
            state->hall_action_context,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kStopApp, .app_index = index});
    }
}

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrameImpl(MetalioClaw4PlatformState& state,
                                                                                     uint32_t hall_app_index,
                                                                                     uint64_t trigger_timestamp_us) {
#if CONFIG_MICROPIXEL_SYSTEM_SHELL_SNAPSHOT
    lv_obj_t* guest_frame = state.guest_graphics.FrameLocked();
    if (state.display == nullptr || guest_frame == nullptr || state.guest_snapshot_pixels != nullptr ||
        state.guest_transition_pixels != nullptr || hall_app_index >= host_ui::kMaxHallApps) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    constexpr uint32_t kThumbnailWidth = kHallCoverNativeSize;
    constexpr uint32_t kThumbnailHeight = kHallCoverNativeSize;
    constexpr uint32_t kThumbnailStride = kHallCoverNativeStride;
    constexpr uint32_t kThumbnailBytes = kHallCoverNativeBytes;
    constexpr uint32_t kThumbnailAllocationBytes =
        (kThumbnailBytes + kPpaBufferAlignment - 1U) / kPpaBufferAlignment * kPpaBufferAlignment;
    auto* thumbnail = static_cast<uint8_t*>(heap_caps_aligned_calloc(kPpaBufferAlignment, kThumbnailAllocationBytes, 1U,
                                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (thumbnail == nullptr) {
        ESP_LOGE(kTag, "suspended-App thumbnail could not allocate %" PRIu32 " PSRAM bytes", kThumbnailAllocationBytes);
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }

    lv_draw_buf_t thumbnail_draw_buf{};
    if (lv_draw_buf_init(&thumbnail_draw_buf, kThumbnailWidth, kThumbnailHeight, LV_COLOR_FORMAT_RGB888,
                         kThumbnailStride, thumbnail, kThumbnailBytes) != LV_RESULT_OK) {
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "could not initialize suspended-App thumbnail draw buffer");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    auto* half = static_cast<uint8_t*>(heap_caps_aligned_alloc(kPpaBufferAlignment, kGuestTransitionHalfAllocationBytes,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (half == nullptr) {
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "suspended-App transition source could not allocate %" PRIu32 " PSRAM bytes",
                 kGuestTransitionHalfAllocationBytes);
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (!state.system_transition.ResetBackgroundToBaseline()) {
        heap_caps_free(half);
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "could not restore baseline Hall background before transition");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    uint32_t half_elapsed_us = 0U;
    const HallCardBounds target_card = HallCard(hall_app_index, state.hall_scroll_offset);
    const bool scaled_half = state.system_transition.CaptureDisplayedToHalf(
        half, kGuestTransitionHalfAllocationBytes,
        lvgl::SystemTransitionRect{
            .x = target_card.x, .y = target_card.y, .width = target_card.width, .height = target_card.width},
        trigger_timestamp_us, half_elapsed_us);
    const int64_t scale_started_us = esp_timer_get_time();
    const bool scaled_card =
        scaled_half && state.system_transition.ScaleHalfToCard(half, thumbnail, kThumbnailAllocationBytes);
    const uint32_t card_elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - scale_started_us);
    if (!scaled_card) {
        state.system_transition.CancelPreparedToHall();
        heap_caps_free(half);
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "PPA suspended-App transition preparation failed");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    MaskHallCoverCorners(thumbnail);
    lv_draw_buf_flush_cache(&thumbnail_draw_buf, nullptr);

    const lvgl::SystemTransitionRect cover_region{
        .x = target_card.x, .y = target_card.y, .width = target_card.width, .height = target_card.width};
    const bool background_patched = state.system_transition.UpdateBackgroundPixels(thumbnail, cover_region);
    const bool animated = background_patched &&
                          state.system_transition.Animate(half, cover_region, lvgl::SystemTransitionDirection::kToHall,
                                                          kGuestTransitionDurationMs, trigger_timestamp_us);
    heap_caps_free(half);
    if (!animated) {
        state.system_transition.CancelPreparedToHall();
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "PPA suspended-App early Hall transition failed");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    state.guest_snapshot_pixels = thumbnail;
    state.guest_transition_pixels = nullptr;
    state.guest_snapshot_in_hall = true;
    ESP_LOGI(kTag,
             "captured suspended-App snapshot: source=displayed-%dx%d RGB888 thumbnail=%" PRIu32 "x%" PRIu32
             " bytes=%" PRIu32 " first-frame=%" PRIu32 " us ppa-card=%" PRIu32 " us transition=complete",
             kWidth, kHeight, kHallCoverNativeSize, kHallCoverNativeSize, kHallCoverNativeBytes, half_elapsed_us,
             card_elapsed_us);
    return host_ui::HallCoverModel{.data = thumbnail,
                                   .size = kHallCoverNativeBytes,
                                   .width = kHallCoverNativeSize,
                                   .height = kHallCoverNativeSize,
                                   .stride = kHallCoverNativeStride};
#else
    (void)state;
    (void)hall_app_index;
    (void)trigger_timestamp_us;
    return std::unexpected(host_ui::SystemUiError::kUnavailable);
#endif
}

void ReleaseGuestSnapshotImpl(MetalioClaw4PlatformState& state) {
    state.system_transition.CancelPreparedToHall();
    if (state.guest_snapshot_pixels == nullptr && state.guest_transition_pixels == nullptr) {
        return;
    }
    heap_caps_free(state.guest_snapshot_pixels);
    heap_caps_free(state.guest_transition_pixels);
    state.guest_snapshot_pixels = nullptr;
    state.guest_transition_pixels = nullptr;
    state.guest_snapshot_in_hall = false;
    ESP_LOGI(kTag, "released suspended-App snapshot");
}

std::expected<void, host_ui::SystemUiError> ShowShutdownImpl(MetalioClaw4PlatformState& state) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    state.hall_cover_cache.Pause();
    SetHostPointerEnabledLocked(state, false);
    state.touch_input.ClearSmokeUi();
    state.input_router.UnbindTouchSink(&state);
    state.hall_action_sink = nullptr;
    state.hall_action_context = nullptr;
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    lv_obj_set_pos(state.host_smoke, 0, 0);
    lv_obj_set_style_opa(state.host_smoke, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    lv_obj_t* label = lv_label_create(state.host_smoke);
    lv_label_set_text(label, "Shutting down...");
    lv_obj_set_style_text_color(label, lv_color_hex(0xeaf4ffU), 0);
    lv_obj_set_style_text_font(label, lvgl::BuiltinLatinFont(lvgl::SystemFontRole::kTitle), 0);
    lv_obj_center(label);
    lv_obj_move_foreground(state.host_smoke);
    lvgl::RequestDisplayRefresh(state.display);
    esp_lv_adapter_unlock();
    ESP_LOGI(kTag, "shutdown screen visible");
    return {};
}

std::expected<void, host_ui::SystemUiError> ShowHallImpl(MetalioClaw4PlatformState& state,
                                                         const host_ui::HallModel& model,
                                                         host_ui::SystemUiActionSink action_sink,
                                                         void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    const uint32_t visible_count = std::min(model.app_count, host_ui::kMaxHallApps);
    const uint64_t catalog_signature = HallCatalogSignature(model, visible_count);
    bool running_state_matches = visible_count == state.hall_app_count;
    for (uint32_t index = 0U; running_state_matches && index < visible_count; ++index) {
        running_state_matches = state.hall_app_running[index] == model.apps[index].running;
    }
    const bool may_resume = state.hall_retained_for_status && state.host_smoke != nullptr &&
                            catalog_signature == state.hall_catalog_signature && running_state_matches &&
                            model.launch_enabled == state.hall_launch_enabled &&
                            model.firmware_update_available == state.hall_firmware_update_available;
    state.hall_retained_for_status = false;
    if (may_resume && esp_lv_adapter_lock(-1) == ESP_OK) {
        const auto& objects = state.hall_scene_ui.objects();
        const bool scene_valid = lv_obj_is_valid(state.host_smoke) && objects.carousel_content != nullptr &&
                                 lv_obj_is_valid(objects.carousel_content);
        if (scene_valid) {
            if (!state.hall_status_bar_valid ||
                !lvgl::square_common::HallStatusBarMatches(state.hall_status_bar, model.status_bar)) {
                state.hall_scene_ui.UpdateStatusBarLocked(model.status_bar);
                state.hall_status_bar = model.status_bar;
                state.hall_status_bar_valid = true;
                lvgl::RequestDisplayRefresh(state.display);
            }
            state.status_layer_ui.RaisePerformanceOverlayLocked();
            SetHostPointerEnabledLocked(state, true);
            esp_lv_adapter_unlock();
            state.hall_action_sink = action_sink;
            state.hall_action_context = action_context;
            state.input_router.BindTouchSink(HostPointerTouchSink, &state);
            state.input_router.BindSystemActionSink(action_sink, action_context);
            ESP_LOGI(kTag, "App Hall resumed after status layer: redraw=no");
            return {};
        }
        esp_lv_adapter_unlock();
    }
    if (catalog_signature != state.hall_catalog_signature) {
        state.hall_catalog_signature = catalog_signature;
        state.hall_scroll_offset = 0;
        state.hall_idle_cover_sources.fill({});
    }
    state.hall_cover_cache.BeginCatalog(model, visible_count, catalog_signature);
    state.hall_app_count = visible_count;
    state.hall_cover_sources.fill({});
    state.hall_app_presentations.fill({});
    for (uint32_t index = 0U; index < visible_count; ++index) {
        state.hall_cover_sources[index] = model.apps[index].cover;
        if (!model.apps[index].running) {
            state.hall_idle_cover_sources[index] = model.apps[index].cover;
        }
        HallAppPresentation& presentation = state.hall_app_presentations[index];
        (void)std::snprintf(presentation.app_id.data(), presentation.app_id.size(), "%s",
                            model.apps[index].app_id != nullptr ? model.apps[index].app_id : "");
        (void)std::snprintf(presentation.display_name.data(), presentation.display_name.size(), "%s",
                            model.apps[index].display_name != nullptr ? model.apps[index].display_name : "");
        presentation.running = model.apps[index].running;
    }
    state.hall_scroll_offset = HallCarousel::ClampOffset(visible_count, state.hall_scroll_offset);
    const bool hall_was_visible = state.host_smoke != nullptr && state.hall_action_context != nullptr;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    SetHostPointerEnabledLocked(state, false);
    state.touch_input.ClearSmokeUi();
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    lv_obj_set_pos(state.host_smoke, 0, 0);
    lv_obj_set_style_opa(state.host_smoke, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};

    state.hall_scene_ui.DrawLocked(state.host_smoke, ui_profile::kHallSceneLayout, model, visible_count,
                                   {.carousel_event = HallCarouselEvent,
                                    .carousel_context = &state,
                                    .action_sink = action_sink,
                                    .action_context = action_context});

    const auto& scene_objects = state.hall_scene_ui.objects();
    lv_obj_update_layout(scene_objects.carousel_viewport);
    lv_obj_scroll_to_x(scene_objects.carousel_viewport, state.hall_scroll_offset, LV_ANIM_OFF);
    UpdateHallCarouselLocked(state, lv_obj_get_scroll_x(scene_objects.carousel_viewport));
    uint32_t running_index = host_ui::kMaxHallApps;
    for (uint32_t index = 0U; index < visible_count; ++index) {
        if (model.apps[index].running) {
            running_index = index;
            break;
        }
    }
    lv_obj_t* guest_frame = state.guest_graphics.FrameLocked();
    const bool transition_candidate = running_index < visible_count && guest_frame != nullptr &&
                                      state.guest_transition_pixels != nullptr && !state.guest_snapshot_in_hall;
    const int64_t background_started_us = esp_timer_get_time();
    bool background_ready = false;
    bool background_updated_region = false;
    if (running_index < visible_count && state.system_transition.HasBackground() &&
        state.hall_cards[running_index] != nullptr) {
        const HallCardBounds card = HallCard(running_index, state.hall_scroll_offset);
        background_ready = state.system_transition.UpdateBackgroundRegionLocked(
            state.hall_cards[running_index], {.x = card.x, .y = card.y, .width = card.width, .height = card.height});
        background_updated_region = background_ready;
    }
    if (!background_ready && running_index < visible_count) {
        background_ready = PrepareCleanHallBackgroundLocked(state);
        if (background_ready && state.hall_cards[running_index] != nullptr) {
            const HallCardBounds card = HallCard(running_index, state.hall_scroll_offset);
            background_ready = state.system_transition.UpdateBackgroundRegionLocked(
                state.hall_cards[running_index],
                {.x = card.x, .y = card.y, .width = card.width, .height = card.height});
            background_updated_region = background_ready;
        }
    } else if (!background_ready) {
        background_ready = state.system_transition.PrepareBackgroundLocked(state.host_smoke);
    }
    const uint32_t background_elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - background_started_us);
    ESP_LOGI(kTag, "Hall transition background prepared: mode=%s elapsed=%" PRIu32 " us",
             background_updated_region ? "card-region" : "full", background_elapsed_us);
    const bool transition_ready = transition_candidate && background_ready;
    if (transition_ready) {
        // Keep the already-visible paused Guest on top until the PPA compositor
        // submits its first frame. The Hall exists only as an off-screen static
        // background at this point, so there is no one-frame Hall flash.
        lv_obj_move_foreground(guest_frame);
    } else {
        lv_obj_move_foreground(state.host_smoke);
    }
    if (state.status_layer_ui.PerformanceOverlayVisibleLocked()) {
        // Rebinding the Hall after its status layer closes rebuilds this root.
        // Preserve the final-display HUD z-order in the same locked update so
        // the Hall cannot cover it for one refresh before the next sample.
        state.status_layer_ui.RaisePerformanceOverlayLocked();
    }
    if (!transition_ready) {
        lvgl::RequestDisplayRefresh(state.display);
    }
    SetHostPointerEnabledLocked(state, true);
    esp_lv_adapter_unlock();

    if (transition_ready) {
        const HallCardBounds card = HallCard(running_index, state.hall_scroll_offset);
        const bool animated = state.system_transition.Animate(
            state.guest_transition_pixels,
            lvgl::SystemTransitionRect{.x = card.x, .y = card.y, .width = card.width, .height = card.width},
            lvgl::SystemTransitionDirection::kToHall, kGuestTransitionDurationMs, model.transition_trigger_us);
        heap_caps_free(state.guest_transition_pixels);
        state.guest_transition_pixels = nullptr;
        state.guest_snapshot_in_hall = true;
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_obj_set_pos(state.host_smoke, 0, 0);
            lv_obj_set_style_opa(state.host_smoke, LV_OPA_COVER, 0);
            lv_obj_move_foreground(state.host_smoke);
            if (state.status_layer_ui.PerformanceOverlayVisibleLocked()) {
                state.status_layer_ui.RaisePerformanceOverlayLocked();
            }
            lvgl::RequestDisplayRefresh(state.display);
            esp_lv_adapter_unlock();
        }
        if (!animated) {
            ESP_LOGW(kTag, "PPA Guest-to-Hall transition failed; Hall restored directly");
        }
    } else {
        if (state.guest_transition_pixels != nullptr) {
            state.system_transition.CancelPreparedToHall();
            heap_caps_free(state.guest_transition_pixels);
            state.guest_transition_pixels = nullptr;
            state.guest_snapshot_in_hall = true;
            state.system_transition.ClearBackground();
        }
        if (!hall_was_visible) {
            ESP_LOGI(kTag, "App Hall presented without a root transition");
        }
    }

    state.hall_action_sink = action_sink;
    state.hall_action_context = action_context;
    state.hall_firmware_update_available = model.firmware_update_available;
    state.hall_launch_enabled = model.launch_enabled;
    state.hall_status_bar = model.status_bar;
    state.hall_status_bar_valid = true;
    state.input_router.BindTouchSink(HostPointerTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    ESP_LOGI(kTag, "App Hall visible: apps=%" PRIu32 " status=%u detail=%" PRIu32, visible_count,
             static_cast<unsigned>(model.status), model.detail);
    return {};
}

void UpdateHallStatusBarImpl(MetalioClaw4PlatformState& state, const host_ui::HallStatusBarModel& model) {
    if (state.display == nullptr || state.host_smoke == nullptr || state.hall_action_context == nullptr ||
        esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.hall_scene_ui.UpdateStatusBarLocked(model);
    state.hall_status_bar = model;
    state.hall_status_bar_valid = true;
    lvgl::RequestDisplayRefresh(state.display);
    esp_lv_adapter_unlock();
}

void PauseHallCoverLoadingImpl(MetalioClaw4PlatformState& state) { state.hall_cover_cache.Pause(); }

void LeaveHallImpl(MetalioClaw4PlatformState& state) {
    PauseHallCoverLoadingImpl(state);
    const uint32_t running_index = RunningHallAppIndex(state);
    const uint32_t hall_app_count = state.hall_app_count;
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(state.hall_action_context);
    state.hall_action_sink = nullptr;
    state.hall_action_context = nullptr;
    state.hall_launch_enabled = false;
    state.hall_retained_for_status = false;
    state.hall_status_bar_valid = false;
    if (state.display == nullptr || state.host_smoke == nullptr ||
        !state.guest_graphics.RefreshSynchronizationAvailable()) {
        state.hall_app_count = 0U;
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        state.hall_app_count = 0U;
        return;
    }
    SetHostPointerEnabledLocked(state, false);
    state.guest_graphics.DrainRefreshReady();
    const lvgl::square_common::HallLaunchBackgroundPlan background_plan = lvgl::square_common::PlanHallLaunchBackground(
        running_index < hall_app_count, state.system_transition.HasBackground());
    if (background_plan == lvgl::square_common::HallLaunchBackgroundPlan::kReuseCleanBaseline) {
        // The clean baseline was preserved when this suspended App entered the
        // Hall (and refreshed after scrolling). Do not synchronously snapshot
        // its RUNNING card on the A-to-B launch path.
        ESP_LOGI(kTag, "reusing clean Hall transition baseline before App switch: running=%" PRIu32, running_index);
    } else {
        const int64_t background_started_us = esp_timer_get_time();
        const bool prepared = background_plan == lvgl::square_common::HallLaunchBackgroundPlan::kPrepareCleanBaseline
                                  ? PrepareCleanHallBackgroundLocked(state, running_index)
                                  : state.system_transition.PrepareBackgroundLocked(state.host_smoke);
        if (prepared) {
            ESP_LOGI(kTag, "Hall transition background refreshed before App launch: mode=%s elapsed=%" PRIu32 " us",
                     background_plan == lvgl::square_common::HallLaunchBackgroundPlan::kPrepareCleanBaseline
                         ? "clean"
                         : "visible",
                     static_cast<uint32_t>(esp_timer_get_time() - background_started_us));
        } else {
            ESP_LOGW(kTag, "could not refresh Hall transition background before App launch");
        }
    }
    state.hall_app_count = 0U;
    lv_obj_clean(state.host_smoke);
    lv_obj_set_pos(state.host_smoke, 0, 0);
    lv_obj_set_style_opa(state.host_smoke, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};
    lvgl::RequestDisplayRefresh(state.display);
    esp_lv_adapter_unlock();

    // Retire the LVGL descriptors before FirmwareApp unmaps Flash covers. The
    // native PSRAM thumbnails remain cached for the next Hall visit.
    state.guest_graphics.WaitForRefreshReady();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ResetHallImageDescriptorsLocked(state);
        esp_lv_adapter_unlock();
    }
}

std::expected<void, host_ui::SystemUiError> RestoreGuestViewImpl(MetalioClaw4PlatformState& state) {
    uint32_t transition_index = host_ui::kMaxHallApps;
    for (uint32_t index = 0U; index < host_ui::kMaxHallApps; ++index) {
        if (state.hall_app_running[index]) {
            transition_index = index;
            break;
        }
    }
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(state.hall_action_context);
    state.hall_action_sink = nullptr;
    state.hall_action_context = nullptr;
    state.hall_app_count = 0U;
    state.hall_launch_enabled = false;
    lv_obj_t* guest_frame = state.guest_graphics.FrameLocked();
    if (state.display == nullptr || state.host_smoke == nullptr || guest_frame == nullptr ||
        !state.guest_graphics.RefreshSynchronizationAvailable()) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    auto* transition_fullscreen =
        static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, kGuestTransitionBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* transition_half = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        kPpaBufferAlignment, kGuestTransitionHalfAllocationBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    lv_draw_buf_t transition_snapshot{};
    bool transition_ready =
        transition_fullscreen != nullptr && transition_half != nullptr && transition_index < host_ui::kMaxHallApps &&
        state.guest_snapshot_in_hall &&
        lv_draw_buf_init(&transition_snapshot, kWidth, kHeight, LV_COLOR_FORMAT_RGB888, kGuestTransitionStride,
                         transition_fullscreen, kGuestTransitionBytes) == LV_RESULT_OK;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(transition_half);
        heap_caps_free(transition_fullscreen);
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(state, false);
    state.guest_graphics.DrainRefreshReady();
    if (transition_ready) {
        transition_ready =
            lv_snapshot_take_to_draw_buf(guest_frame, LV_COLOR_FORMAT_RGB888, &transition_snapshot) == LV_RESULT_OK;
        if (transition_ready) {
            lv_draw_buf_flush_cache(&transition_snapshot, nullptr);
            const int64_t background_started_us = esp_timer_get_time();
            transition_ready = state.system_transition.RefreshBackgroundLocked(state.host_smoke);
            if (transition_ready) {
                ESP_LOGI(kTag, "Hall transition background refreshed before App resume: elapsed=%" PRIu32 " us",
                         static_cast<uint32_t>(esp_timer_get_time() - background_started_us));
            }
        }
    }
    esp_lv_adapter_unlock();

    if (transition_ready) {
        transition_ready = state.system_transition.ScaleFullscreenToHalf(transition_fullscreen, transition_half,
                                                                         kGuestTransitionHalfAllocationBytes);
    }
    heap_caps_free(transition_fullscreen);
    if (transition_ready) {
        const HallCardBounds card = HallCard(transition_index, state.hall_scroll_offset);
        transition_ready = state.system_transition.Animate(
            transition_half,
            lvgl::SystemTransitionRect{.x = card.x, .y = card.y, .width = card.width, .height = card.width},
            lvgl::SystemTransitionDirection::kToGuest, kGuestTransitionDurationMs);
    } else {
        state.system_transition.ClearBackground();
    }
    heap_caps_free(transition_half);
    state.guest_snapshot_in_hall = false;

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    // Delete the Hall and reveal the retained Guest tree in one LVGL update.
    // A successful PPA transition already left the physical panel on the same
    // full-size frame. The retained tree replaces it without a black interval.
    lv_obj_delete(state.host_smoke);
    state.host_smoke = nullptr;
    state.launch_image_descriptor = {};
    lv_obj_move_foreground(guest_frame);
    lvgl::RequestDisplayRefresh(state.display);
    esp_lv_adapter_unlock();

    state.guest_graphics.WaitForRefreshReady();
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ResetHallImageDescriptorsLocked(state);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(kTag, "retained Guest view restored from App Hall: transition=%s", transition_ready ? "PPA" : "direct");
    return {};
}

std::expected<void, host_ui::SystemUiError> ShowSystemMenuImpl(MetalioClaw4PlatformState& state,
                                                               const host_ui::SystemMenuModel& model,
                                                               host_ui::SystemUiActionSink action_sink,
                                                               void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(state.hall_action_context);
    state.hall_action_sink = nullptr;
    state.hall_action_context = nullptr;
    state.hall_app_count = 0U;
    state.hall_launch_enabled = false;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};
    auto result = state.system_menu_ui.ShowLocked(state.host_smoke, state.display, ui_profile::kSystemMenuLayout, model,
                                                  action_sink, action_context);
    if (result.has_value() && state.status_layer_ui.PerformanceOverlayVisibleLocked()) {
        state.status_layer_ui.RaisePerformanceOverlayLocked();
    }
    SetHostPointerEnabledLocked(state, result.has_value());
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        state.system_menu_ui.Deactivate();
        return result;
    }

    state.input_router.BindTouchSink(HostPointerTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    return {};
}

void UpdateSystemMenuImpl(MetalioClaw4PlatformState& state, const host_ui::SystemMenuModel& model) {
    state.system_menu_ui.Update(model);
}

void LeaveSystemMenuImpl(MetalioClaw4PlatformState& state) {
    if (!state.system_menu_ui.Active()) {
        return;
    }
    void* action_context = state.system_menu_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(action_context);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(state, false);
        esp_lv_adapter_unlock();
    }
    state.system_menu_ui.Deactivate();
}

std::expected<void, host_ui::SystemUiError> ShowSystemInformationImpl(MetalioClaw4PlatformState& state,
                                                                      const host_ui::SystemInformationModel& model,
                                                                      host_ui::SystemUiActionSink action_sink,
                                                                      void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void* menu_action_context = state.system_menu_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(menu_action_context);
    state.system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    auto result =
        state.system_detail_ui.ShowSystemInformationLocked(state.host_smoke, model, action_sink, action_context);
    if (!result.has_value()) {
        state.system_detail_ui.LeaveSystemInformation();
        esp_lv_adapter_unlock();
        return result;
    }
    SetHostPointerEnabledLocked(state, true);
    esp_lv_adapter_unlock();
    state.input_router.BindTouchSink(HostPointerTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    return {};
}

void UpdateSystemInformationImpl(MetalioClaw4PlatformState& state, const host_ui::SystemInformationModel& model) {
    if (!state.system_detail_ui.SystemInformationVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.system_detail_ui.UpdateSystemInformationLocked(model);
    esp_lv_adapter_unlock();
}

void LeaveSystemInformationImpl(MetalioClaw4PlatformState& state) {
    if (!state.system_detail_ui.SystemInformationVisible()) {
        return;
    }
    void* action_context = state.system_detail_ui.SystemInformationActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(action_context);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(state, false);
        esp_lv_adapter_unlock();
    }
    state.system_detail_ui.LeaveSystemInformation();
}

std::expected<void, host_ui::SystemUiError> ShowPowerManagementImpl(MetalioClaw4PlatformState& state,
                                                                    const host_ui::PowerManagementModel& model,
                                                                    host_ui::SystemUiActionSink action_sink,
                                                                    void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void* menu_action_context = state.system_menu_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(menu_action_context);
    state.system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    auto result =
        state.system_detail_ui.ShowPowerManagementLocked(state.host_smoke, model, action_sink, action_context);
    if (!result.has_value()) {
        state.system_detail_ui.LeavePowerManagement();
        esp_lv_adapter_unlock();
        return result;
    }
    SetHostPointerEnabledLocked(state, true);
    esp_lv_adapter_unlock();
    state.input_router.BindTouchSink(HostPointerTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    return {};
}

void UpdatePowerManagementImpl(MetalioClaw4PlatformState& state, const host_ui::PowerManagementModel& model) {
    if (!state.system_detail_ui.PowerManagementVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.system_detail_ui.UpdatePowerManagementLocked(model);
    esp_lv_adapter_unlock();
}

void LeavePowerManagementImpl(MetalioClaw4PlatformState& state) {
    if (!state.system_detail_ui.PowerManagementVisible()) {
        return;
    }
    void* action_context = state.system_detail_ui.PowerManagementActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(action_context);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(state, false);
        esp_lv_adapter_unlock();
    }
    state.system_detail_ui.LeavePowerManagement();
}

std::expected<void, host_ui::SystemUiError> ShowRemoteControlImpl(MetalioClaw4PlatformState& state,
                                                                  const host_ui::RemoteControlModel& model,
                                                                  host_ui::SystemUiActionSink action_sink,
                                                                  void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void* menu_action_context = state.system_menu_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(menu_action_context);
    state.system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    auto result = state.system_detail_ui.ShowRemoteControlLocked(state.host_smoke, model, action_sink, action_context);
    if (!result.has_value()) {
        state.system_detail_ui.LeaveRemoteControl();
        esp_lv_adapter_unlock();
        return result;
    }
    SetHostPointerEnabledLocked(state, true);
    esp_lv_adapter_unlock();
    state.input_router.BindTouchSink(HostPointerTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    return {};
}

void UpdateRemoteControlImpl(MetalioClaw4PlatformState& state, const host_ui::RemoteControlModel& model) {
    if (!state.system_detail_ui.RemoteControlVisible() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.system_detail_ui.UpdateRemoteControlLocked(model);
    esp_lv_adapter_unlock();
}

void LeaveRemoteControlImpl(MetalioClaw4PlatformState& state) {
    if (!state.system_detail_ui.RemoteControlVisible()) {
        return;
    }
    void* action_context = state.system_detail_ui.RemoteControlActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(action_context);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(state, false);
        esp_lv_adapter_unlock();
    }
    state.system_detail_ui.LeaveRemoteControl();
}

std::expected<void, host_ui::SystemUiError> ShowAppManagementImpl(MetalioClaw4PlatformState& state,
                                                                  const host_ui::AppManagementModel& model,
                                                                  host_ui::SystemUiActionSink action_sink,
                                                                  void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void* menu_action_context = state.system_menu_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(menu_action_context);
    state.system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    auto result = state.system_detail_ui.ShowAppManagementLocked(state.host_smoke, model, action_sink, action_context);
    if (!result.has_value()) {
        state.system_detail_ui.LeaveAppManagement();
        esp_lv_adapter_unlock();
        return result;
    }
    SetHostPointerEnabledLocked(state, true);
    esp_lv_adapter_unlock();
    state.input_router.BindTouchSink(HostPointerTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    return {};
}

void LeaveAppManagementImpl(MetalioClaw4PlatformState& state) {
    if (!state.system_detail_ui.AppManagementVisible()) {
        return;
    }
    void* action_context = state.system_detail_ui.AppManagementActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(action_context);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(state, false);
        esp_lv_adapter_unlock();
    }
    state.system_detail_ui.LeaveAppManagement();
}

std::expected<void, host_ui::SystemUiError> ShowWifiSettingsImpl(MetalioClaw4PlatformState& state,
                                                                 const host_ui::WifiSettingsModel& model,
                                                                 host_ui::SystemUiActionSink action_sink,
                                                                 void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void* menu_action_context = state.system_menu_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(menu_action_context);
    state.system_menu_ui.Deactivate();
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    ResetHallImageDescriptorsLocked(state);
    auto result = state.wifi_settings_ui.ShowLocked(
        state.host_smoke, state.display, ui_profile::kSystemPageLayout, model, action_sink, action_context,
        [](void* context) {
            static_cast<lvgl::square_common::StatusLayerUi*>(context)->RaisePerformanceOverlayLocked();
        },
        &state.status_layer_ui);
    if (!result.has_value()) {
        esp_lv_adapter_unlock();
        return result;
    }
    SetHostPointerEnabledLocked(state, true);
    esp_lv_adapter_unlock();

    state.input_router.BindTouchSink(HostPointerTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    return {};
}

void UpdateWifiSettingsImpl(MetalioClaw4PlatformState& state, const host_ui::WifiSettingsModel& model) {
    state.wifi_settings_ui.Update(model, HostPointerBusy(state));
}

void LeaveWifiSettingsImpl(MetalioClaw4PlatformState& state) {
    if (!state.wifi_settings_ui.Visible()) {
        return;
    }
    void* action_context = state.wifi_settings_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(action_context);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetHostPointerEnabledLocked(state, false);
        esp_lv_adapter_unlock();
    }
    state.wifi_settings_ui.Leave();
}

std::expected<void, host_ui::SystemUiError> ShowStatusLayerImpl(MetalioClaw4PlatformState& state,
                                                                const host_ui::StatusLayerModel& model,
                                                                uint64_t trigger_timestamp_us,
                                                                host_ui::SystemUiActionSink action_sink,
                                                                void* action_context) {
    state.hall_retained_for_status = false;
    if (state.display != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
        const auto& objects = state.hall_scene_ui.objects();
        state.hall_retained_for_status =
            state.host_smoke != nullptr && lv_obj_is_valid(state.host_smoke) && objects.carousel_content != nullptr &&
            lv_obj_is_valid(objects.carousel_content) && state.hall_action_context != nullptr;
        esp_lv_adapter_unlock();
    }
    auto result =
        lvgl::square_common::PresentStatusLayer(state.display, state.status_layer_ui, state.system_transition, model,
                                                trigger_timestamp_us, action_sink, action_context, true);
    if (!result.has_value()) {
        state.hall_retained_for_status = false;
        return std::unexpected(result.error());
    }
    state.input_router.BindTouchSink(lvgl::square_common::StatusLayerUi::TouchSink, &state.status_layer_ui);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    return {};
}

void UpdateStatusLayerImpl(MetalioClaw4PlatformState& state, const host_ui::StatusLayerModel& model) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.status_layer_ui.UpdateLocked(model);
    esp_lv_adapter_unlock();
}

void LeaveStatusLayerImpl(MetalioClaw4PlatformState& state, uint64_t trigger_timestamp_us) {
    void* action_context = state.status_layer_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state.status_layer_ui);
    state.input_router.ClearSystemActionSink(action_context);
    (void)lvgl::square_common::DismissStatusLayer(state.display, state.status_layer_ui, state.system_transition,
                                                  trigger_timestamp_us, true);
}

void UpdatePerformanceOverlayImpl(MetalioClaw4PlatformState& state, bool enabled, uint8_t cpu_percent) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.status_layer_ui.UpdatePerformanceOverlayLocked(enabled, cpu_percent,
                                                         state.guest_graphics.DisplayRefreshSequence());
    esp_lv_adapter_unlock();
}

common::GraphicsOperations MakeGraphicsOperations(MetalioClaw4PlatformState& state) {
    return {
        .context = &state,
        .available =
            [](void* context) { return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.Available(); },
        .get_info =
            [](void* context, micropixel_graphics_info_t& info) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.GetInfo(info);
            },
        .begin_frame =
            [](void* context) { return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.BeginFrame(); },
        .submit =
            [](void* context, const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.Submit(bytes, length, textures);
            },
        .commit_frame =
            [](void* context, const device::TextureAccess& textures) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.CommitFrame(textures);
            },
        .cancel_frame =
            [](void* context) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.CancelFrame();
            },
        .load_font =
            [](void* context, const device::FontResourceView& resource, micropixel_font_info_t& info) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.LoadFont(resource, info);
            },
        .release_font =
            [](void* context, micropixel_font_handle_t font) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.ReleaseFont(font);
            },
        .measure_text =
            [](void* context, micropixel_font_handle_t font, const char* text, uint32_t text_length,
               micropixel_text_metrics_t& metrics) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.MeasureText(
                    font, text, text_length, metrics);
            },
        .begin_bitmap_update_frame =
            [](void* context) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.BeginBitmapUpdateFrame();
            },
        .update_bitmap =
            [](void* context, const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               const uint8_t* pixels, uint32_t stride) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.UpdateBitmap(
                    bitmap, x, y, width, height, pixels, stride);
            },
        .commit_bitmap_update_frame =
            [](void* context) {
                return static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.CommitBitmapUpdateFrame();
            },
        .show_launch_bitmap =
            [](void* context, const device::BitmapView& bitmap) {
                return ShowLaunchBitmapImpl(*static_cast<MetalioClaw4PlatformState*>(context), bitmap);
            },
        .dismiss_launch_bitmap =
            [](void* context) { DismissLaunchBitmapImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .release_guest_resources =
            [](void* context) { static_cast<MetalioClaw4PlatformState*>(context)->guest_graphics.Release(); },
    };
}

common::SystemUiOperations MakeSystemUiOperations(MetalioClaw4PlatformState& state) {
    return {
        .context = &state,
        .show_hall =
            [](void* context, const host_ui::HallModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowHallImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                    action_context);
            },
        .update_hall_status_bar =
            [](void* context, const host_ui::HallStatusBarModel& model) {
                UpdateHallStatusBarImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .pause_hall_cover_loading =
            [](void* context) { PauseHallCoverLoadingImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .leave_hall = [](void* context) { LeaveHallImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .restore_guest_view =
            [](void* context) { return RestoreGuestViewImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .watch_guest_actions =
            [](void* context, host_ui::SystemUiActionSink action_sink, void* action_context) {
                static_cast<MetalioClaw4PlatformState*>(context)->input_router.BindSystemActionSink(action_sink,
                                                                                                    action_context);
            },
        .stop_watching_guest_actions =
            [](void* context, void* action_context) {
                static_cast<MetalioClaw4PlatformState*>(context)->input_router.ClearSystemActionSink(action_context);
            },
        .capture_guest_frame =
            [](void* context, uint32_t hall_app_index, uint64_t trigger_timestamp_us) {
                return CaptureGuestFrameImpl(*static_cast<MetalioClaw4PlatformState*>(context), hall_app_index,
                                             trigger_timestamp_us);
            },
        .capture_screen_jpeg =
            [](void* context) {
                auto& state = *static_cast<MetalioClaw4PlatformState*>(context);
                return metalio_claw4::CaptureScreenJpeg(state.display, state.hardware.Panel(), kWidth, kHeight);
            },
        .release_guest_snapshot =
            [](void* context) { ReleaseGuestSnapshotImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_system_menu =
            [](void* context, const host_ui::SystemMenuModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowSystemMenuImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                          action_context);
            },
        .update_system_menu =
            [](void* context, const host_ui::SystemMenuModel& model) {
                UpdateSystemMenuImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .leave_system_menu =
            [](void* context) { LeaveSystemMenuImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_system_information =
            [](void* context, const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowSystemInformationImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                                 action_context);
            },
        .update_system_information =
            [](void* context, const host_ui::SystemInformationModel& model) {
                UpdateSystemInformationImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .leave_system_information =
            [](void* context) { LeaveSystemInformationImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_power_management =
            [](void* context, const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowPowerManagementImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                               action_context);
            },
        .update_power_management =
            [](void* context, const host_ui::PowerManagementModel& model) {
                UpdatePowerManagementImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .leave_power_management =
            [](void* context) { LeavePowerManagementImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_remote_control =
            [](void* context, const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowRemoteControlImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                             action_context);
            },
        .update_remote_control =
            [](void* context, const host_ui::RemoteControlModel& model) {
                UpdateRemoteControlImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .leave_remote_control =
            [](void* context) { LeaveRemoteControlImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_app_management =
            [](void* context, const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowAppManagementImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                             action_context);
            },
        .leave_app_management =
            [](void* context) { LeaveAppManagementImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_wifi_settings =
            [](void* context, const host_ui::WifiSettingsModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowWifiSettingsImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                            action_context);
            },
        .update_wifi_settings =
            [](void* context, const host_ui::WifiSettingsModel& model) {
                UpdateWifiSettingsImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .leave_wifi_settings =
            [](void* context) { LeaveWifiSettingsImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_status_layer =
            [](void* context, const host_ui::StatusLayerModel& model, uint64_t trigger_timestamp_us,
               host_ui::SystemUiActionSink action_sink, void* action_context) {
                return ShowStatusLayerImpl(*static_cast<MetalioClaw4PlatformState*>(context), model,
                                           trigger_timestamp_us, action_sink, action_context);
            },
        .update_status_layer =
            [](void* context, const host_ui::StatusLayerModel& model) {
                UpdateStatusLayerImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .leave_status_layer =
            [](void* context, uint64_t trigger_timestamp_us) {
                LeaveStatusLayerImpl(*static_cast<MetalioClaw4PlatformState*>(context), trigger_timestamp_us);
            },
        .update_performance_overlay =
            [](void* context, bool enabled, uint8_t cpu_percent) {
                UpdatePerformanceOverlayImpl(*static_cast<MetalioClaw4PlatformState*>(context), enabled, cpu_percent);
            },
        .apply_brightness =
            [](void* context, uint8_t percent) {
                const uint8_t safe_percent = percent <= 100U ? percent : 100U;
                const uint32_t output = metalio_claw4::PerceptualBrightnessOutputPerTenThousand(safe_percent);
                const esp_err_t status =
                    static_cast<MetalioClaw4PlatformState*>(context)->display_pipeline.SetBrightness(output);
                if (status != ESP_OK) {
                    ESP_LOGW(kTag, "could not set backlight brightness: %s", esp_err_to_name(status));
                }
            },
        .apply_volume =
            [](void*, uint8_t percent) { metalio_claw4::ConfiguredAudioBackend().SetMasterVolumePercent(percent); },
        .show_shutdown =
            [](void* context) { return ShowShutdownImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
    };
}

class MetalioClaw4Platform final : public Platform, public device::PowerBackend, public device::HardwareInfoBackend {
   public:
    MetalioClaw4Platform() : graphics_(MakeGraphicsOperations(state_)), system_ui_(MakeSystemUiOperations(state_)) {
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
                [](void* context, lv_obj_t* guest_frame, bool created_guest_frame, bool& needs_present) {
                    auto& state = *static_cast<MetalioClaw4PlatformState*>(context);
                    if (state.status_layer_ui.PerformanceOverlayVisibleLocked()) {
                        if (created_guest_frame) {
                            state.status_layer_ui.RaisePerformanceOverlayLocked();
                        }
                    } else if (created_guest_frame || state.host_smoke != nullptr) {
                        lv_obj_move_foreground(guest_frame);
                    }
                    if (DismissHostSmokeLocked(state)) {
                        needs_present = true;
                    }
                },
        });
    }

    [[nodiscard]] esp_err_t Initialize() override { return InitializePlatformImpl(state_); }
    [[nodiscard]] device::GraphicsBackend& graphics() override { return graphics_; }
    [[nodiscard]] device::InputBackend& input() override { return state_.input_router; }
    [[nodiscard]] device::AudioBackend& audio() override { return metalio_claw4::ConfiguredAudioBackend(); }
    [[nodiscard]] device::BatteryBackend& battery() override { return state_.battery; }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }
    [[nodiscard]] device::WifiBackend& wifi() override { return wifi_; }
    [[nodiscard]] device::PowerBackend& power() override { return *this; }
    [[nodiscard]] device::LocalControlBackend& local_control() override {
        return metalio_claw4::UsbLocalControlBackend();
    }
    [[nodiscard]] device::DeviceCatalogBackend& devices() override { return state_.devices; }
    [[nodiscard]] device::SensorBackend& sensors() override { return state_.sensors; }
    [[nodiscard]] device::GpioBackend& gpio() override { return state_.gpio; }
    [[nodiscard]] device::HapticsBackend& haptics() override { return state_.haptics; }
    [[nodiscard]] device::HardwareInfoBackend& hardware_info() override { return *this; }
    [[nodiscard]] host_ui::SystemUiBackend& system_ui() override { return system_ui_; }
    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.hall_cover_cache.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

    void SetPowerButtonSink(device::PowerButtonSink sink, void* context) override {
        state_.power_key.SetKeyPressSink(sink, context);
    }

    void SetPowerOffButtonSink(device::PowerOffButtonSink sink, void* context) override {
        state_.power_key.SetPowerOffSink(sink, context);
    }

    [[nodiscard]] std::expected<void, device::PowerError> EnterLowPower() override { return EnterLowPowerImpl(state_); }

    [[noreturn]] void PowerOff() override { state_.power_key.PowerOff(); }

    [[nodiscard]] device::HardwareInfo Snapshot() const override {
        return {
            .board = "Metalio-Claw4",
            .host_chip = "ESP32-P4",
            .wifi_coprocessor = "ESP32-C5",
            .touch_controller = "GT911",
            .display =
                {
                    .driver = "NV3051F",
                    .interface = "MIPI-DSI / 2 lanes",
                    .pixel_format = "RGB888",
                    .width_pixels = static_cast<uint32_t>(kWidth),
                    .height_pixels = static_cast<uint32_t>(kHeight),
                    .refresh_rate_hz = 60U,
                },
        };
    }

   private:
    MetalioClaw4PlatformState state_{};
    common::GraphicsAdapter graphics_;
    radio::EspHostedRadio wifi_radio_{};
    common::wifi::WifiBackend wifi_{wifi_radio_};
    common::SystemUiAdapter system_ui_;
};

}  // namespace

Platform& ConfiguredPlatform() {
    static MetalioClaw4Platform platform;
    return platform;
}

}  // namespace micropixel::platform
