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
#include "platform/audio_backend.hpp"
#include "platform/configured_backends.hpp"
#include "platform/metalio-claw4/battery_backend.hpp"
#include "platform/metalio-claw4/board_hardware.hpp"
#include "platform/metalio-claw4/display/png_cover_decoder.hpp"
#include "platform/metalio-claw4/display/screen_capture.hpp"
#include "platform/metalio-claw4/display/system_transition_compositor.hpp"
#include "platform/metalio-claw4/graphics_adapter.hpp"
#include "platform/metalio-claw4/guest_graphics_engine.hpp"
#include "platform/metalio-claw4/hall_carousel.hpp"
#include "platform/metalio-claw4/icons/wifi_status_icons.hpp"
#include "platform/metalio-claw4/input/gt911_input.hpp"
#include "platform/metalio-claw4/input/tca9555_power_key.hpp"
#include "platform/metalio-claw4/perceptual_control.hpp"
#include "platform/metalio-claw4/status_layer_ui.hpp"
#include "platform/metalio-claw4/system_detail_ui.hpp"
#include "platform/metalio-claw4/system_menu_ui.hpp"
#include "platform/metalio-claw4/system_ui_adapter.hpp"
#include "platform/metalio-claw4/wifi_backend.hpp"
#include "platform/metalio-claw4/wifi_settings_ui.hpp"
#include "platform/platform.hpp"
#include "task_policy.hpp"

namespace micropixel::platform {
namespace {

constexpr char kTag[] = "micropixel_platform";
constexpr int kWidth = 720;
constexpr int kHeight = 720;
constexpr BaseType_t kLvglTaskCore = 1;
constexpr uint32_t kRefreshPeriodMs = 1000;
constexpr uint32_t kLvglTaskMinDelayMs = portTICK_PERIOD_MS;
constexpr uint32_t kHostPointerQueueCapacity = 32U;
constexpr uint32_t kThemeAccentColor = 0x287ee8U;
constexpr int32_t kHallCardWidth = metalio_claw4::HallCarousel::kCardWidth;
constexpr int32_t kHallCardRadius = 22;
constexpr int32_t kHallCardBorderWidth = 1;
constexpr uint32_t kHallCoverBytesPerPixel = 3U;
constexpr uint32_t kHallCoverNativeSize = static_cast<uint32_t>(kHallCardWidth);
constexpr uint32_t kHallCoverNativeStride = kHallCoverNativeSize * kHallCoverBytesPerPixel;
constexpr uint32_t kHallCoverNativeBytes = kHallCoverNativeStride * kHallCoverNativeSize;
constexpr uint32_t kHallCoverJobQueueCapacity = 8U;
constexpr uint32_t kHallCoverWorkerStackBytes = 8192U;
constexpr int32_t kHallScrollTrackWidth = 140;
constexpr int32_t kHallScrollTrackHeight = 6;
constexpr uint32_t kGuestTransitionStride = static_cast<uint32_t>(kWidth) * kHallCoverBytesPerPixel;
constexpr uint32_t kGuestTransitionBytes = kGuestTransitionStride * static_cast<uint32_t>(kHeight);
constexpr uint32_t kGuestTransitionHalfSize = 360U;
constexpr uint32_t kGuestTransitionHalfBytes =
    kGuestTransitionHalfSize * kGuestTransitionHalfSize * kHallCoverBytesPerPixel;
constexpr uint32_t kPpaBufferAlignment = 128U;
constexpr uint32_t kGuestTransitionHalfAllocationBytes =
    (kGuestTransitionHalfBytes + kPpaBufferAlignment - 1U) / kPpaBufferAlignment * kPpaBufferAlignment;
constexpr uint32_t kStatusTransitionDurationMs = 100U;
constexpr uint32_t kGuestTransitionDurationMs = 100U;
constexpr uint32_t kUiTransitionFrameCount = 6U;
constexpr uint16_t kTransitionComplete = 1000U;
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
    if (destination == nullptr) {
        return;
    }
    // Bake the four small corner masks into the retained bitmap instead of
    // making LVGL allocate a card-sized rounded clipping layer on every redraw.
    // The cover is drawn full-bleed below the card's post-drawn border, so its
    // baked corner mask must use the same outer radius as the card.
    constexpr uint32_t kRadius = static_cast<uint32_t>(kHallCardRadius);
    constexpr int32_t kCenter = static_cast<int32_t>(kRadius - 1U);
    constexpr int32_t kRadiusSquared = kCenter * kCenter;
    const auto set_pixel = [destination](uint32_t x, uint32_t y, uint32_t rgb) {
        uint8_t* pixel = destination + static_cast<size_t>(y) * kHallCoverNativeStride +
                         static_cast<size_t>(x) * kHallCoverBytesPerPixel;
        pixel[0] = static_cast<uint8_t>(rgb);
        pixel[1] = static_cast<uint8_t>(rgb >> 8U);
        pixel[2] = static_cast<uint8_t>(rgb >> 16U);
    };
    for (uint32_t y = 0U; y < kRadius; ++y) {
        for (uint32_t x = 0U; x < kRadius; ++x) {
            const int32_t dx = static_cast<int32_t>(x) - kCenter;
            const int32_t dy = static_cast<int32_t>(y) - kCenter;
            if (dx * dx + dy * dy <= kRadiusSquared) {
                continue;
            }
            set_pixel(x, y, 0x08111fU);
            set_pixel(kHallCoverNativeSize - 1U - x, y, 0x08111fU);
            set_pixel(x, kHallCoverNativeSize - 1U - y, 0x111f32U);
            set_pixel(kHallCoverNativeSize - 1U - x, kHallCoverNativeSize - 1U - y, 0x111f32U);
        }
    }
}

void ScaleRgb888Cover(const uint8_t* source, uint32_t source_width, uint32_t source_height, uint32_t source_stride,
                      uint8_t* destination) {
    if (source == nullptr || destination == nullptr || source_width == 0U || source_height == 0U) {
        return;
    }
    const uint32_t crop_size = source_width < source_height ? source_width : source_height;
    const uint32_t crop_x = (source_width - crop_size) / 2U;
    const uint32_t crop_y = (source_height - crop_size) / 2U;
    for (uint32_t y = 0U; y < kHallCoverNativeSize; ++y) {
        const uint32_t source_y =
            crop_y + static_cast<uint32_t>(static_cast<uint64_t>(y) * crop_size / kHallCoverNativeSize);
        uint8_t* destination_row = destination + static_cast<size_t>(y) * kHallCoverNativeStride;
        for (uint32_t x = 0U; x < kHallCoverNativeSize; ++x) {
            const uint32_t source_x =
                crop_x + static_cast<uint32_t>(static_cast<uint64_t>(x) * crop_size / kHallCoverNativeSize);
            std::memcpy(destination_row + static_cast<size_t>(x) * kHallCoverBytesPerPixel,
                        source + static_cast<size_t>(source_y) * source_stride +
                            static_cast<size_t>(source_x) * kHallCoverBytesPerPixel,
                        kHallCoverBytesPerPixel);
        }
    }
    MaskHallCoverCorners(destination);
}

struct HallCoverCacheEntry final {
    uint8_t* pixels{};
    uint64_t key{};
    uint32_t app_index{host_ui::kMaxHallApps};
};

struct HallCoverJob final {
    host_ui::HallCoverModel source{};
    uint64_t catalog_signature{};
    uint32_t app_index{};
    uint32_t request_generation{};
};

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
    metalio_claw4::BatteryBackend battery{};
    lv_display_t* display{};
    lv_obj_t* host_smoke{};
    lv_obj_t* hall_time_status_label{};
    lv_obj_t* hall_cellular_status_container{};
    std::array<lv_obj_t*, 4> hall_cellular_signal_bars{};
    lv_obj_t* hall_wifi_status_image{};
    lv_obj_t* hall_battery_status_label{};
    lv_obj_t* hall_battery_percent_label{};
    metalio_claw4::GuestGraphicsEngine guest_graphics{kWidth, kHeight};
    metalio_claw4::SystemTransitionCompositor system_transition{};
    lv_obj_t* hall_settings_press_overlay{};
    lv_obj_t* hall_update_press_overlay{};
    lv_obj_t* hall_carousel_viewport{};
    lv_obj_t* hall_carousel_content{};
    lv_obj_t* hall_scroll_track{};
    lv_obj_t* hall_scroll_thumb{};
    lv_image_dsc_t launch_image_descriptor{};
    lv_image_dsc_t hall_cover_descriptors[host_ui::kMaxHallApps]{};
    std::array<HallCoverCacheEntry, metalio_claw4::HallCarousel::kMaximumCachedCovers> hall_cover_cache{};
    std::array<host_ui::HallCoverModel, host_ui::kMaxHallApps> hall_cover_sources{};
    std::array<HallAppPresentation, host_ui::kMaxHallApps> hall_app_presentations{};
    lv_obj_t* hall_cover_images[host_ui::kMaxHallApps]{};
    lv_obj_t* hall_cover_placeholders[host_ui::kMaxHallApps]{};
    QueueHandle_t hall_cover_job_queue{};
    StaticQueue_t hall_cover_job_queue_storage{};
    std::array<uint8_t, sizeof(HallCoverJob) * kHallCoverJobQueueCapacity> hall_cover_job_queue_bytes{};
    TaskHandle_t hall_cover_worker_task{};
    std::atomic_uint32_t hall_cover_request_generation{1U};
    std::atomic_bool hall_cover_worker_active{};
    uint32_t hall_cover_window_first{};
    uint32_t hall_cover_window_last{};
    uint32_t hall_card_window_first{};
    uint32_t hall_card_window_last{};
    lv_obj_t* hall_cards[host_ui::kMaxHallApps]{};
    lv_obj_t* hall_card_press_overlays[host_ui::kMaxHallApps]{};
    metalio_claw4::Tca9555PowerKey power_key{};
    metalio_claw4::Gt911Input touch_input{kWidth, kHeight, kTouchInterrupt};
    host_ui::SystemGestureRouter input_router{touch_input, kWidth, kHeight};
    metalio_claw4::SystemDetailUi system_detail_ui{};
    metalio_claw4::SystemMenuUi system_menu_ui{};
    metalio_claw4::StatusLayerUi status_layer_ui{};
    metalio_claw4::WifiSettingsUi wifi_settings_ui{};
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
    bool hall_firmware_update_available{};
    bool hall_launch_enabled{};
    bool hall_app_running[host_ui::kMaxHallApps]{};
    bool guest_snapshot_in_hall{};
};

void SyncHallCardWindowLocked(MetalioClaw4PlatformState& state);
void RequestHallCoverWindowLocked(MetalioClaw4PlatformState& state, bool force = false);
void HallCarouselEvent(lv_event_t* event);
void HallHeaderButtonEvent(lv_event_t* event);
void HallCardEvent(lv_event_t* event);
void HallCloseButtonEvent(lv_event_t* event);

uint16_t EaseInCubic(uint16_t progress) {
    const uint64_t value = progress;
    return static_cast<uint16_t>(value * value * value / 1000000U);
}

uint16_t EaseOutCubic(uint16_t progress) {
    return static_cast<uint16_t>(kTransitionComplete - EaseInCubic(kTransitionComplete - progress));
}

template <typename Apply>
void RunUiTransition(MetalioClaw4PlatformState& state, bool entering, Apply&& apply) {
    const TickType_t frame_period =
        std::max<TickType_t>(1U, pdMS_TO_TICKS(kStatusTransitionDurationMs / kUiTransitionFrameCount));
    TickType_t next_frame = xTaskGetTickCount();
    for (uint32_t frame = 0U; frame <= kUiTransitionFrameCount; ++frame) {
        const uint16_t linear = static_cast<uint16_t>(frame * kTransitionComplete / kUiTransitionFrameCount);
        const uint16_t progress = entering ? EaseOutCubic(linear) : kTransitionComplete - EaseInCubic(linear);
        if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
            break;
        }
        apply(progress);
        lv_timer_ready(lv_display_get_refr_timer(state.display));
        esp_lv_adapter_unlock();
        if (frame != kUiTransitionFrameCount) {
            vTaskDelayUntil(&next_frame, frame_period);
        }
    }
}

void RunStatusLayerTransition(MetalioClaw4PlatformState& state, bool entering) {
    RunUiTransition(state, entering,
                    [&state](uint16_t progress) { state.status_layer_ui.SetTransitionProgressLocked(progress); });
}

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
        lv_timer_ready(lv_display_get_refr_timer(state->display));
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
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_center(label);
}

esp_err_t InitializeLvgl(MetalioClaw4PlatformState& state) {
    esp_lv_adapter_config_t adapter_config{};
    adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_config.stack_in_psram = false;
    adapter_config.task_priority = task_policy::kDisplayPriority;
    adapter_config.task_core_id = kLvglTaskCore;
    adapter_config.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
    // A sub-tick timeout is rounded down to zero by pdMS_TO_TICKS().  LVGL's
    // animation timer can then keep this high-priority task in a busy loop and
    // starve IDLE1 until the task watchdog fires.
    adapter_config.task_min_delay_ms = kLvglTaskMinDelayMs;
    adapter_config.task_max_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS;
    adapter_config.auto_sleep.enable = false;
    adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED;
    adapter_config.auto_sleep.idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS;
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
    display_config.panel = state.hardware.Panel();
    display_config.panel_io = state.hardware.PanelIo();
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
    status = state.guest_graphics.Initialize(state.display, state.hardware.Panel());
    if (status != ESP_OK) {
        return status;
    }
    status = state.system_transition.Initialize(state.display, state.hardware.Panel(), kWidth, kHeight);
    if (status != ESP_OK) {
        return status;
    }
    lv_timer_set_period(lv_display_get_refr_timer(state.display), kRefreshPeriodMs);

    state.host_pointer_indev = lv_indev_create();
    if (state.host_pointer_indev == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(state.host_pointer_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(state.host_pointer_indev, HostPointerRead);
    lv_indev_set_user_data(state.host_pointer_indev, &state);
    lv_indev_set_display(state.host_pointer_indev, state.display);
    SetHostPointerEnabledLocked(state, false);

    status = state.touch_input.Start(state.display);
    if (status != ESP_OK) {
        return status;
    }
    CreateStartingScreen(state);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
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
        state.battery.Initialize(state.hardware.I2cBus(), state.hardware.IoExpander());
        state.power_key.SetPowerInputChangeSink(
            [](void* context) { static_cast<metalio_claw4::BatteryBackend*>(context)->NotifyExternalPowerChanged(); },
            &state.battery);
        status = state.power_key.Initialize(state.hardware.IoExpander());
    }
    if (status == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        status = state.touch_input.Initialize(state.hardware.I2cBus());
    }
    if (status == ESP_OK) {
        status = InitializeLvgl(state);
    }
    if (status == ESP_OK) {
        status = state.hardware.SetBacklight(true);
    }
    if (status == ESP_OK) {
        status = ConfiguredAudioBackend().Initialize(state.hardware.IoExpander());
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

void ResetHallImageDescriptorsLocked(MetalioClaw4PlatformState& state) {
    state.hall_time_status_label = nullptr;
    state.hall_cellular_status_container = nullptr;
    state.hall_cellular_signal_bars.fill(nullptr);
    state.hall_wifi_status_image = nullptr;
    state.hall_battery_status_label = nullptr;
    state.hall_battery_percent_label = nullptr;
    state.hall_carousel_viewport = nullptr;
    state.hall_carousel_content = nullptr;
    state.hall_scroll_track = nullptr;
    state.hall_scroll_thumb = nullptr;
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
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
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
        lv_timer_ready(lv_display_get_refr_timer(state.display));
    }
    esp_lv_adapter_unlock();
}

const char* HallStatusText(host_ui::HallStatus status) {
    switch (status) {
        case host_ui::HallStatus::kReady:
            return "Choose an app";
        case host_ui::HallStatus::kNoApps:
            return "No installed apps";
        case host_ui::HallStatus::kAppExited:
            return "App closed - choose another";
        case host_ui::HallStatus::kAppFailed:
            return "App failed";
        case host_ui::HallStatus::kRuntimeUnavailable:
            return "Runtime unavailable";
        case host_ui::HallStatus::kHostFailure:
            return "Host could not start the app";
    }
    return "Unavailable";
}

uint32_t HallStatusColor(host_ui::HallStatus status) {
    return status == host_ui::HallStatus::kReady || status == host_ui::HallStatus::kAppExited ? 0x4dd6a4U : 0xff8b73U;
}

lv_obj_t* CreateHallLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int32_t x,
                          int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

const lv_image_dsc_t* HallWifiImage(const host_ui::HallWifiModel& model) {
    if (!model.available || !model.enabled || !model.connected) {
        return nullptr;
    }
    if (model.rssi < -75) {
        return &micropixel_wifi_status_weak;
    }
    if (model.rssi < -60) {
        return &micropixel_wifi_status_medium;
    }
    return &micropixel_wifi_status_full;
}

const char* HallBatterySymbol(const host_ui::HallBatteryModel& model) {
    if (model.percent <= 10U) {
        return LV_SYMBOL_BATTERY_EMPTY;
    }
    if (model.percent <= 35U) {
        return LV_SYMBOL_BATTERY_1;
    }
    if (model.percent <= 60U) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (model.percent <= 85U) {
        return LV_SYMBOL_BATTERY_3;
    }
    return LV_SYMBOL_BATTERY_FULL;
}

void UpdateHallStatusBarLocked(MetalioClaw4PlatformState& state, const host_ui::HallStatusBarModel& model) {
    if (state.hall_time_status_label == nullptr || state.hall_cellular_status_container == nullptr ||
        state.hall_wifi_status_image == nullptr || state.hall_battery_status_label == nullptr ||
        state.hall_battery_percent_label == nullptr) {
        return;
    }

    lv_label_set_text(state.hall_time_status_label, model.time_text.data());

    if (model.cellular.available) {
        const uint32_t active_bars = model.cellular.enabled && model.cellular.connected
                                         ? std::min<uint32_t>(model.cellular.signal_bars, 4U)
                                         : 0U;
        for (uint32_t index = 0U; index < state.hall_cellular_signal_bars.size(); ++index) {
            lv_obj_set_style_bg_opa(state.hall_cellular_signal_bars[index], index < active_bars ? LV_OPA_COVER : 72, 0);
        }
        lv_obj_remove_flag(state.hall_cellular_status_container, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state.hall_cellular_status_container, LV_OBJ_FLAG_HIDDEN);
    }

    if (const lv_image_dsc_t* wifi_image = HallWifiImage(model.wifi); wifi_image != nullptr) {
        lv_image_set_src(state.hall_wifi_status_image, wifi_image);
        lv_obj_remove_flag(state.hall_wifi_status_image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state.hall_wifi_status_image, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(state.hall_battery_status_label,
                      model.battery.available ? HallBatterySymbol(model.battery) : LV_SYMBOL_BATTERY_EMPTY);
    char battery_text[8]{};
    if (model.battery.available) {
        (void)std::snprintf(battery_text, sizeof(battery_text), "%u%%", static_cast<unsigned>(model.battery.percent));
    } else {
        (void)std::snprintf(battery_text, sizeof(battery_text), "--");
    }
    lv_label_set_text(state.hall_battery_percent_label, battery_text);
    lv_obj_set_style_text_color(state.hall_battery_status_label, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_set_style_text_color(state.hall_battery_percent_label, lv_color_hex(0xf2f7ffU), 0);
}

void DrawHallStatusBarLocked(MetalioClaw4PlatformState& state, const host_ui::HallStatusBarModel& model) {
    state.hall_time_status_label =
        CreateHallLabel(state.host_smoke, model.time_text.data(), &lv_font_montserrat_18, 0xf2f7ffU, 40, 8);
    lv_obj_set_width(state.hall_time_status_label, 72);

    state.hall_cellular_status_container = lv_obj_create(state.host_smoke);
    lv_obj_set_pos(state.hall_cellular_status_container, 548, 12);
    lv_obj_set_size(state.hall_cellular_status_container, 22, 16);
    lv_obj_set_style_pad_all(state.hall_cellular_status_container, 0, 0);
    lv_obj_set_style_border_width(state.hall_cellular_status_container, 0, 0);
    lv_obj_set_style_bg_opa(state.hall_cellular_status_container, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(state.hall_cellular_status_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state.hall_cellular_status_container, LV_OBJ_FLAG_CLICKABLE);
    constexpr std::array<int32_t, 4> kCellularBarHeights{4, 7, 10, 13};
    for (uint32_t index = 0U; index < state.hall_cellular_signal_bars.size(); ++index) {
        lv_obj_t* bar = lv_obj_create(state.hall_cellular_status_container);
        const int32_t height = kCellularBarHeights[index];
        lv_obj_set_pos(bar, static_cast<int32_t>(index) * 5, 16 - height);
        lv_obj_set_size(bar, 3, height);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xf2f7ffU), 0);
        state.hall_cellular_signal_bars[index] = bar;
    }

    state.hall_wifi_status_image = lv_image_create(state.host_smoke);
    lv_obj_set_pos(state.hall_wifi_status_image, 584, 13);
    lv_obj_set_style_image_recolor(state.hall_wifi_status_image, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_set_style_image_recolor_opa(state.hall_wifi_status_image, LV_OPA_COVER, 0);

    state.hall_battery_status_label = CreateHallLabel(state.host_smoke, "", &lv_font_montserrat_24, 0xf2f7ffU, 622, 7);
    lv_obj_set_width(state.hall_battery_status_label, 30);
    lv_obj_set_style_text_align(state.hall_battery_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_scale_x(state.hall_battery_status_label, 282, 0);

    state.hall_battery_percent_label =
        CreateHallLabel(state.host_smoke, "", &lv_font_montserrat_14, 0xf2f7ffU, 657, 12);
    lv_obj_set_width(state.hall_battery_percent_label, 38);
    lv_obj_set_style_text_align(state.hall_battery_percent_label, LV_TEXT_ALIGN_RIGHT, 0);
    UpdateHallStatusBarLocked(state, model);
}

struct HallCardBounds final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

HallCardBounds HallCard(uint32_t index, int32_t scroll_offset) {
    return HallCardBounds{.x = metalio_claw4::HallCarousel::CardX(index, scroll_offset),
                          .y = metalio_claw4::HallCarousel::kTop,
                          .width = kHallCardWidth,
                          .height = metalio_claw4::HallCarousel::kCardHeight};
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
    state.hall_scroll_offset = metalio_claw4::HallCarousel::ClampOffset(state.hall_app_count, offset);
    if (state.hall_scroll_thumb != nullptr) {
        const int32_t thumb_width =
            metalio_claw4::HallCarousel::ScrollThumbWidth(kHallScrollTrackWidth, state.hall_app_count);
        lv_obj_set_x(state.hall_scroll_thumb,
                     metalio_claw4::HallCarousel::ScrollThumbX(kHallScrollTrackWidth, thumb_width, state.hall_app_count,
                                                               state.hall_scroll_offset));
    }
    SyncHallCardWindowLocked(state);
    // Decoding runs on the bounded background worker. Submit the newly visible
    // window immediately, including during drag and inertia, so covers can
    // appear before motion finishes instead of leaving placeholders behind.
    RequestHallCoverWindowLocked(state);
}

const char* AppShortName(const char* app_id) {
    if (app_id == nullptr) {
        return "APP";
    }
    const char* separator = std::strrchr(app_id, '.');
    return separator != nullptr && separator[1] != '\0' ? separator + 1 : app_id;
}

bool ValidHallCover(const host_ui::HallCoverModel& cover) {
    if (cover.data == nullptr || (!esp_ptr_in_drom(cover.data) && !esp_ptr_external_ram(cover.data)) ||
        cover.width == 0U || cover.height == 0U || cover.size == 0U) {
        return false;
    }
    if (cover.format == host_ui::HallCoverFormat::kPng) {
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
    const auto cached = std::find_if(
        state.hall_cover_cache.begin(), state.hall_cover_cache.end(),
        [&](const HallCoverCacheEntry& entry) { return entry.pixels != nullptr && entry.key == source.cache_key; });
    if (cached == state.hall_cover_cache.end()) {
        return false;
    }
    prepared = {.data = cached->pixels,
                .size = kHallCoverNativeBytes,
                .width = kHallCoverNativeSize,
                .height = kHallCoverNativeSize,
                .stride = kHallCoverNativeStride};
    return true;
}

bool DecodeNativeHallCover(const host_ui::HallCoverModel& source, uint8_t* pixels) {
    if (pixels == nullptr) {
        return false;
    }
    const bool decoded =
        source.format == host_ui::HallCoverFormat::kPng
            ? metalio_claw4::DecodePngCoverRgb888(source.data, source.size, source.width, source.height, pixels,
                                                  kHallCoverNativeSize, kHallCoverNativeSize, kHallCoverNativeStride,
                                                  0x182d48U)
            : (ScaleRgb888Cover(source.data, source.width, source.height, source.stride, pixels), true);
    if (decoded && source.format == host_ui::HallCoverFormat::kPng) {
        MaskHallCoverCorners(pixels);
    }
    if (!decoded) {
        ESP_LOGE(kTag, "could not decode Hall cover: source=%" PRIu32 "x%" PRIu32 " bytes=%" PRIu32, source.width,
                 source.height, source.size);
    }
    return decoded;
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

void ReleaseHallCoverCacheEntryLocked(MetalioClaw4PlatformState& state, HallCoverCacheEntry& entry) {
    if (entry.app_index < host_ui::kMaxHallApps) {
        ShowHallCoverPlaceholderLocked(state, entry.app_index);
        auto& descriptor = state.hall_cover_descriptors[entry.app_index];
        if (descriptor.data != nullptr) {
            lv_image_cache_drop(&descriptor);
            lv_image_header_cache_drop(&descriptor);
            descriptor = {};
        }
    }
    heap_caps_free(entry.pixels);
    entry = {};
}

void HallCoverWorker(void* context) {
    auto& state = *static_cast<MetalioClaw4PlatformState*>(context);
    HallCoverJob job{};
    for (;;) {
        if (xQueueReceive(state.hall_cover_job_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint32_t current_generation = state.hall_cover_request_generation.load(std::memory_order_acquire);
        if (job.request_generation != current_generation) {
            state.hall_cover_worker_active.store(false, std::memory_order_release);
            continue;
        }
        state.hall_cover_worker_active.store(true, std::memory_order_release);
        constexpr uint32_t kCacheAlignment = 64U;
        constexpr uint32_t kAllocationBytes =
            (kHallCoverNativeBytes + kCacheAlignment - 1U) / kCacheAlignment * kCacheAlignment;
        auto* pixels = static_cast<uint8_t*>(
            heap_caps_aligned_calloc(kCacheAlignment, kAllocationBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        const int64_t started_us = esp_timer_get_time();
        const bool decoded = pixels != nullptr && DecodeNativeHallCover(job.source, pixels);
        if (decoded) {
            lv_draw_buf_t draw_buf{};
            if (lv_draw_buf_init(&draw_buf, kHallCoverNativeSize, kHallCoverNativeSize, LV_COLOR_FORMAT_RGB888,
                                 kHallCoverNativeStride, pixels, kHallCoverNativeBytes) == LV_RESULT_OK) {
                lv_draw_buf_flush_cache(&draw_buf, nullptr);
            }
        }
        if (decoded && esp_lv_adapter_lock(-1) == ESP_OK) {
            const bool current =
                job.request_generation == state.hall_cover_request_generation.load(std::memory_order_acquire) &&
                job.catalog_signature == state.hall_catalog_signature &&
                job.app_index >= state.hall_cover_window_first && job.app_index < state.hall_cover_window_last &&
                job.app_index < state.hall_app_count &&
                state.hall_cover_sources[job.app_index].cache_key == job.source.cache_key;
            if (current) {
                auto existing = std::find_if(state.hall_cover_cache.begin(), state.hall_cover_cache.end(),
                                             [&](const HallCoverCacheEntry& entry) {
                                                 return entry.pixels != nullptr && entry.key == job.source.cache_key;
                                             });
                if (existing == state.hall_cover_cache.end()) {
                    auto slot = std::find_if(state.hall_cover_cache.begin(), state.hall_cover_cache.end(),
                                             [](const HallCoverCacheEntry& entry) { return entry.pixels == nullptr; });
                    if (slot == state.hall_cover_cache.end()) {
                        slot = std::find_if(state.hall_cover_cache.begin(), state.hall_cover_cache.end(),
                                            [&](const HallCoverCacheEntry& entry) {
                                                return entry.app_index < state.hall_cover_window_first ||
                                                       entry.app_index >= state.hall_cover_window_last;
                                            });
                    }
                    if (slot != state.hall_cover_cache.end()) {
                        ReleaseHallCoverCacheEntryLocked(state, *slot);
                        *slot = {.pixels = pixels, .key = job.source.cache_key, .app_index = job.app_index};
                        pixels = nullptr;
                        existing = slot;
                    }
                }
                if (existing != state.hall_cover_cache.end()) {
                    existing->app_index = job.app_index;
                    AttachHallCoverLocked(state, job.app_index,
                                          host_ui::HallCoverModel{.data = existing->pixels,
                                                                  .size = kHallCoverNativeBytes,
                                                                  .width = kHallCoverNativeSize,
                                                                  .height = kHallCoverNativeSize,
                                                                  .stride = kHallCoverNativeStride});
                    lv_obj_invalidate(state.hall_cover_images[job.app_index]);
                    lv_timer_ready(lv_display_get_refr_timer(state.display));
                }
            }
            esp_lv_adapter_unlock();
        }
        heap_caps_free(pixels);
        const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
        ESP_LOGI(kTag, "Hall cover worker: app=%" PRIu32 " current=%s cpu=%" PRIu32 " us", job.app_index,
                 job.request_generation == state.hall_cover_request_generation.load(std::memory_order_acquire) ? "yes"
                                                                                                               : "no",
                 elapsed_us);
        state.hall_cover_worker_active.store(false, std::memory_order_release);
    }
}

bool EnsureHallCoverWorker(MetalioClaw4PlatformState& state) {
    if (state.hall_cover_job_queue == nullptr) {
        state.hall_cover_job_queue =
            xQueueCreateStatic(kHallCoverJobQueueCapacity, sizeof(HallCoverJob),
                               state.hall_cover_job_queue_bytes.data(), &state.hall_cover_job_queue_storage);
    }
    if (state.hall_cover_job_queue == nullptr) {
        return false;
    }
    if (state.hall_cover_worker_task == nullptr) {
        if (xTaskCreatePinnedToCore(HallCoverWorker, "hall_cover", kHallCoverWorkerStackBytes, &state,
                                    task_policy::kAssetWorkerPriority, &state.hall_cover_worker_task, 0) != pdPASS) {
            state.hall_cover_worker_task = nullptr;
        }
    }
    return state.hall_cover_worker_task != nullptr;
}

void RequestHallCoverWindowLocked(MetalioClaw4PlatformState& state, bool force) {
    if (state.hall_app_count == 0U || state.hall_carousel_content == nullptr || !EnsureHallCoverWorker(state)) {
        return;
    }
    const uint32_t first =
        metalio_claw4::HallCarousel::CoverWindowFirst(state.hall_app_count, state.hall_scroll_offset);
    const uint32_t last = metalio_claw4::HallCarousel::CoverWindowLast(state.hall_app_count, state.hall_scroll_offset);
    if (!force && first == state.hall_cover_window_first && last == state.hall_cover_window_last) {
        return;
    }
    state.hall_cover_window_first = first;
    state.hall_cover_window_last = last;
    const uint32_t generation = state.hall_cover_request_generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    (void)xQueueReset(state.hall_cover_job_queue);
    for (uint32_t index = 0U; index < state.hall_app_count; ++index) {
        if (index < first || index >= last) {
            ShowHallCoverPlaceholderLocked(state, index);
        }
    }
    for (uint32_t index = first; index < last; ++index) {
        const host_ui::HallCoverModel& source = state.hall_cover_sources[index];
        if (!ValidHallCover(source)) {
            continue;
        }
        host_ui::HallCoverModel prepared{};
        if (PrepareNativeHallCover(state, source, index, prepared)) {
            AttachHallCoverLocked(state, index, prepared);
            continue;
        }
        ShowHallCoverPlaceholderLocked(state, index);
        const HallCoverJob job{.source = source,
                               .catalog_signature = state.hall_catalog_signature,
                               .app_index = index,
                               .request_generation = generation};
        (void)xQueueSend(state.hall_cover_job_queue, &job, 0U);
    }
}

void DrawHallCard(MetalioClaw4PlatformState& state, lv_obj_t* parent, const HallAppPresentation& app, uint32_t index) {
    constexpr uint32_t kCoverColors[] = {0xff9f43U, 0x4dd6a4U, 0x69a7ffU};
    const HallCardBounds bounds = HallCard(index, 0);
    lv_obj_t* card = lv_obj_create(parent);
    state.hall_cards[index] = card;
    lv_obj_set_pos(card, bounds.x - metalio_claw4::HallCarousel::kLeft, 0);
    lv_obj_set_size(card, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_radius(card, kHallCardRadius, 0);
    lv_obj_set_style_border_width(card, kHallCardBorderWidth, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2e4562U), 0);
    lv_obj_set_style_border_post(card, true, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111f32U), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, HallCardEvent, LV_EVENT_ALL, &state);

    lv_obj_t* cover = lv_obj_create(card);
    // Child coordinates start inside the border. Offset by the border width so
    // the cover and its baked mask share the card's outer bounds.
    lv_obj_set_pos(cover, -kHallCardBorderWidth, -kHallCardBorderWidth);
    lv_obj_set_size(cover, bounds.width, bounds.width);
    lv_obj_set_style_radius(cover, kHallCardRadius, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x182d48U), 0);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cover_mark = lv_obj_create(cover);
    state.hall_cover_placeholders[index] = cover_mark;
    lv_obj_set_size(cover_mark, 126, 96);
    lv_obj_set_style_radius(cover_mark, 28, 0);
    lv_obj_set_style_border_width(cover_mark, 0, 0);
    lv_obj_set_style_bg_color(cover_mark, lv_color_hex(kCoverColors[index % std::size(kCoverColors)]), 0);
    lv_obj_remove_flag(cover_mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(cover_mark);
    lv_obj_t* cover_label = lv_label_create(cover_mark);
    lv_label_set_text(cover_label, AppShortName(app.app_id.data()));
    lv_obj_set_style_text_font(cover_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cover_label, lv_color_hex(0x08111fU), 0);
    lv_obj_center(cover_label);

    lv_obj_t* image = lv_image_create(cover);
    state.hall_cover_images[index] = image;
    lv_obj_center(image);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);

    host_ui::HallCoverModel prepared_cover{};
    const host_ui::HallCoverModel& source = state.hall_cover_sources[index];
    if (ValidHallCover(source) && PrepareNativeHallCover(state, source, index, prepared_cover)) {
        AttachHallCoverLocked(state, index, prepared_cover);
    }

    const char* display_name = app.display_name[0] != '\0' ? app.display_name.data() : app.app_id.data();
    lv_obj_t* app_label = CreateHallLabel(card, display_name, &lv_font_montserrat_18, 0xf2f7ffU, 12, bounds.width + 12);
    lv_obj_set_width(app_label, bounds.width - 24);
    lv_obj_set_height(app_label, 24);
    lv_obj_set_style_text_align(app_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(app_label, LV_LABEL_LONG_DOT);

    if (app.running) {
        lv_obj_t* running = lv_obj_create(card);
        lv_obj_set_pos(running, 10, 10);
        lv_obj_set_size(running, 112, 44);
        lv_obj_set_style_radius(running, 22, 0);
        lv_obj_set_style_border_width(running, 0, 0);
        lv_obj_set_style_bg_color(running, lv_color_hex(0x08111fU), 0);
        lv_obj_set_style_bg_opa(running, LV_OPA_90, 0);
        lv_obj_remove_flag(running, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(running, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* running_label = lv_label_create(running);
        lv_label_set_text(running_label, "RUNNING");
        lv_obj_set_style_text_font(running_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(running_label, lv_color_hex(0x4dd6a4U), 0);
        lv_obj_set_style_text_opa(running_label, LV_OPA_COVER, 0);
        lv_obj_center(running_label);

        lv_obj_t* close = lv_obj_create(card);
        lv_obj_set_pos(close, bounds.width - 56, 10);
        lv_obj_set_size(close, 46, 46);
        lv_obj_set_style_radius(close, 23, 0);
        lv_obj_set_style_border_width(close, 0, 0);
        lv_obj_set_style_bg_color(close, lv_color_hex(0x08111fU), 0);
        lv_obj_set_style_bg_color(close, lv_color_hex(0x182d48U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_set_style_bg_opa(close, LV_OPA_90, 0);
        lv_obj_remove_flag(close, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(close, HallCloseButtonEvent, LV_EVENT_SHORT_CLICKED, &state);
        lv_obj_t* stop_icon = lv_obj_create(close);
        lv_obj_set_size(stop_icon, 18, 18);
        lv_obj_set_style_radius(stop_icon, 3, 0);
        lv_obj_set_style_border_width(stop_icon, 0, 0);
        lv_obj_set_style_bg_color(stop_icon, lv_color_hex(0xff5c5cU), 0);
        lv_obj_set_style_bg_opa(stop_icon, LV_OPA_COVER, 0);
        lv_obj_remove_flag(stop_icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(stop_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(stop_icon);
    }

    lv_obj_t* press_overlay = lv_obj_create(card);
    lv_obj_set_pos(press_overlay, -kHallCardBorderWidth, -kHallCardBorderWidth);
    lv_obj_set_size(press_overlay, bounds.width, bounds.height);
    lv_obj_set_style_radius(press_overlay, kHallCardRadius, 0);
    lv_obj_set_style_border_width(press_overlay, 0, 0);
    lv_obj_set_style_bg_color(press_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(press_overlay, LV_OPA_40, 0);
    lv_obj_remove_flag(press_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(press_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(press_overlay, LV_OBJ_FLAG_HIDDEN);
    state.hall_card_press_overlays[index] = press_overlay;
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

void SyncHallCardWindowLocked(MetalioClaw4PlatformState& state) {
    if (state.hall_carousel_content == nullptr) {
        return;
    }
    const uint32_t first =
        metalio_claw4::HallCarousel::CoverWindowFirst(state.hall_app_count, state.hall_scroll_offset);
    const uint32_t last = metalio_claw4::HallCarousel::CoverWindowLast(state.hall_app_count, state.hall_scroll_offset);
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
            DrawHallCard(state, state.hall_carousel_content, state.hall_app_presentations[index], index);
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
    if (state == nullptr || viewport == nullptr || viewport != state->hall_carousel_viewport) {
        return;
    }
    UpdateHallCarouselLocked(*state, lv_obj_get_scroll_x(viewport));
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
        RequestHallCoverWindowLocked(*state, true);
    }
    if (state->display != nullptr) {
        lv_timer_ready(lv_display_get_refr_timer(state->display));
    }
}

void HallHeaderButtonEvent(lv_event_t* event) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(lv_event_get_user_data(event));
    lv_obj_t* button = lv_event_get_current_target_obj(event);
    if (state == nullptr || button == nullptr) {
        return;
    }
    lv_obj_t* overlay = nullptr;
    bool update = false;
    if (state->hall_settings_press_overlay != nullptr &&
        lv_obj_get_parent(state->hall_settings_press_overlay) == button) {
        overlay = state->hall_settings_press_overlay;
    } else if (state->hall_update_press_overlay != nullptr &&
               lv_obj_get_parent(state->hall_update_press_overlay) == button) {
        overlay = state->hall_update_press_overlay;
        update = true;
    } else {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        SetHallPressOverlayLocked(overlay, true);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        SetHallPressOverlayLocked(overlay, false);
    } else if (code == LV_EVENT_SHORT_CLICKED && state->hall_action_sink != nullptr) {
        ESP_LOGI(kTag, "App Hall accepted native %s tap", update ? "Update" : "Settings");
        state->hall_action_sink(
            state->hall_action_context,
            host_ui::SystemUiAction{.type = update ? host_ui::SystemUiActionType::kOpenFirmwareUpdate
                                                   : host_ui::SystemUiActionType::kOpenSystemMenu});
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
        metalio_claw4::SystemTransitionRect{
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

    const metalio_claw4::SystemTransitionRect cover_region{
        .x = target_card.x, .y = target_card.y, .width = target_card.width, .height = target_card.width};
    const bool background_patched = state.system_transition.UpdateBackgroundPixels(thumbnail, cover_region);
    const bool animated =
        background_patched &&
        state.system_transition.Animate(half, cover_region, metalio_claw4::SystemTransitionDirection::kToHall,
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

std::expected<void, host_ui::SystemUiError> ShowHallImpl(MetalioClaw4PlatformState& state,
                                                         const host_ui::HallModel& model,
                                                         host_ui::SystemUiActionSink action_sink,
                                                         void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    state.hall_cover_request_generation.fetch_add(1U, std::memory_order_acq_rel);
    if (state.hall_cover_job_queue != nullptr) {
        (void)xQueueReset(state.hall_cover_job_queue);
    }
    const uint32_t visible_count = std::min(model.app_count, host_ui::kMaxHallApps);
    const uint64_t catalog_signature = HallCatalogSignature(model, visible_count);
    if (catalog_signature != state.hall_catalog_signature) {
        state.hall_catalog_signature = catalog_signature;
        state.hall_scroll_offset = 0;
    }
    state.hall_app_count = visible_count;
    state.hall_cover_sources.fill({});
    state.hall_app_presentations.fill({});
    for (uint32_t index = 0U; index < visible_count; ++index) {
        state.hall_cover_sources[index] = model.apps[index].cover;
        HallAppPresentation& presentation = state.hall_app_presentations[index];
        (void)std::snprintf(presentation.app_id.data(), presentation.app_id.size(), "%s",
                            model.apps[index].app_id != nullptr ? model.apps[index].app_id : "");
        (void)std::snprintf(presentation.display_name.data(), presentation.display_name.size(), "%s",
                            model.apps[index].display_name != nullptr ? model.apps[index].display_name : "");
        presentation.running = model.apps[index].running;
    }
    state.hall_cover_window_first = host_ui::kMaxHallApps;
    state.hall_cover_window_last = host_ui::kMaxHallApps;
    state.hall_scroll_offset = metalio_claw4::HallCarousel::ClampOffset(visible_count, state.hall_scroll_offset);
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
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    lv_obj_set_pos(state.host_smoke, 0, 0);
    lv_obj_set_style_opa(state.host_smoke, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};

    lv_obj_t* brand = lv_obj_create(state.host_smoke);
    lv_obj_set_pos(brand, 40, 60);
    lv_obj_set_size(brand, 18, 44);
    lv_obj_set_style_radius(brand, 9, 0);
    lv_obj_set_style_border_width(brand, 0, 0);
    lv_obj_set_style_bg_color(brand, lv_color_hex(0xff9f43U), 0);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_CLICKABLE);

    (void)CreateHallLabel(state.host_smoke, "App Hall", &lv_font_montserrat_32, 0xf2f7ffU, 76, 56);
    (void)CreateHallLabel(state.host_smoke,
                          visible_count > metalio_claw4::HallCarousel::kFullyVisibleCards
                              ? "Swipe to browse - tap to launch"
                              : "Tap a card to launch",
                          &lv_font_montserrat_18, 0x91a4bdU, 76, 98);
    (void)CreateHallLabel(state.host_smoke, "INSTALLED APPS", &lv_font_montserrat_18, 0x91a4bdU, 40, 148);
    char app_count_text[24]{};
    (void)std::snprintf(app_count_text, sizeof(app_count_text), "%" PRIu32 " %s", visible_count,
                        visible_count == 1U ? "app" : "apps");
    lv_obj_t* app_count_label =
        CreateHallLabel(state.host_smoke, app_count_text, &lv_font_montserrat_18, 0x91a4bdU, 560, 148);
    lv_obj_set_width(app_count_label, 120);
    lv_obj_set_style_text_align(app_count_label, LV_TEXT_ALIGN_RIGHT, 0);
    DrawHallStatusBarLocked(state, model.status_bar);

    lv_obj_t* settings_button = lv_obj_create(state.host_smoke);
    lv_obj_set_pos(settings_button, 520, 62);
    lv_obj_set_size(settings_button, 160, 60);
    lv_obj_set_style_pad_all(settings_button, 0, 0);
    lv_obj_set_style_radius(settings_button, 20, 0);
    lv_obj_set_style_border_width(settings_button, 1, 0);
    lv_obj_set_style_border_color(settings_button, lv_color_hex(0x42607fU), 0);
    lv_obj_set_style_bg_color(settings_button, lv_color_hex(0x111f32U), 0);
    lv_obj_set_style_bg_opa(settings_button, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(settings_button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(settings_button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(settings_button, 8, 0);
    lv_obj_remove_flag(settings_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(settings_button, HallHeaderButtonEvent, LV_EVENT_ALL, &state);

    lv_obj_t* settings_icon = lv_label_create(settings_button);
    lv_label_set_text(settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(settings_icon, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_t* settings_label = lv_label_create(settings_button);
    lv_label_set_text(settings_label, "Settings");
    lv_obj_set_style_text_font(settings_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(settings_label, lv_color_hex(0xf2f7ffU), 0);
    state.hall_settings_press_overlay = lv_obj_create(settings_button);
    lv_obj_set_pos(state.hall_settings_press_overlay, 0, 0);
    lv_obj_set_size(state.hall_settings_press_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(state.hall_settings_press_overlay, 0, 0);
    lv_obj_set_style_radius(state.hall_settings_press_overlay, 20, 0);
    lv_obj_set_style_border_width(state.hall_settings_press_overlay, 0, 0);
    lv_obj_set_style_bg_color(state.hall_settings_press_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(state.hall_settings_press_overlay, LV_OPA_40, 0);
    lv_obj_remove_flag(state.hall_settings_press_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state.hall_settings_press_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(state.hall_settings_press_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(state.hall_settings_press_overlay, LV_OBJ_FLAG_HIDDEN);

    if (model.firmware_update_available) {
        lv_obj_t* update_button = lv_obj_create(state.host_smoke);
        lv_obj_set_pos(update_button, 338, 62);
        lv_obj_set_size(update_button, 160, 60);
        lv_obj_set_style_pad_all(update_button, 0, 0);
        lv_obj_set_style_radius(update_button, 20, 0);
        lv_obj_set_style_border_width(update_button, 1, 0);
        lv_obj_set_style_border_color(update_button, lv_color_hex(0x5e9f59U), 0);
        lv_obj_set_style_bg_color(update_button, lv_color_hex(0x152a2aU), 0);
        lv_obj_set_style_bg_opa(update_button, LV_OPA_COVER, 0);
        lv_obj_remove_flag(update_button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(update_button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(update_button, HallHeaderButtonEvent, LV_EVENT_ALL, &state);

        lv_obj_t* update_icon = lv_label_create(update_button);
        lv_label_set_text(update_icon, LV_SYMBOL_REFRESH);
        lv_obj_set_style_text_font(update_icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(update_icon, lv_color_hex(0xcdf5c6U), 0);
        lv_obj_align(update_icon, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_t* update_label = lv_label_create(update_button);
        lv_label_set_text(update_label, "Update");
        lv_obj_set_style_text_font(update_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(update_label, lv_color_hex(0xcdf5c6U), 0);
        lv_obj_align(update_label, LV_ALIGN_LEFT_MID, 50, 0);
        lv_obj_t* update_dot = lv_obj_create(update_button);
        lv_obj_set_pos(update_dot, 141, 8);
        lv_obj_set_size(update_dot, 10, 10);
        lv_obj_set_style_pad_all(update_dot, 0, 0);
        lv_obj_set_style_radius(update_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(update_dot, 0, 0);
        lv_obj_set_style_bg_color(update_dot, lv_color_hex(0xef5d4fU), 0);
        lv_obj_remove_flag(update_dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(update_dot, LV_OBJ_FLAG_CLICKABLE);

        state.hall_update_press_overlay = lv_obj_create(update_button);
        lv_obj_set_pos(state.hall_update_press_overlay, 0, 0);
        lv_obj_set_size(state.hall_update_press_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(state.hall_update_press_overlay, 0, 0);
        lv_obj_set_style_radius(state.hall_update_press_overlay, 20, 0);
        lv_obj_set_style_border_width(state.hall_update_press_overlay, 0, 0);
        lv_obj_set_style_bg_color(state.hall_update_press_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(state.hall_update_press_overlay, LV_OPA_40, 0);
        lv_obj_remove_flag(state.hall_update_press_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(state.hall_update_press_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(state.hall_update_press_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    state.hall_carousel_viewport = lv_obj_create(state.host_smoke);
    lv_obj_set_pos(state.hall_carousel_viewport, metalio_claw4::HallCarousel::kLeft, metalio_claw4::HallCarousel::kTop);
    lv_obj_set_size(state.hall_carousel_viewport, metalio_claw4::HallCarousel::kViewportWidth,
                    metalio_claw4::HallCarousel::kCardHeight);
    lv_obj_set_style_pad_all(state.hall_carousel_viewport, 0, 0);
    lv_obj_set_style_border_width(state.hall_carousel_viewport, 0, 0);
    lv_obj_set_style_bg_color(state.hall_carousel_viewport, lv_color_hex(0x08111fU), 0);
    lv_obj_set_style_bg_opa(state.hall_carousel_viewport, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(state.hall_carousel_viewport, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(state.hall_carousel_viewport, LV_SCROLLBAR_MODE_OFF);
    constexpr lv_obj_flag_t kHallScrollFlags = static_cast<lv_obj_flag_t>(
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLLABLE) | static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM) |
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_add_flag(state.hall_carousel_viewport, kHallScrollFlags);
    lv_obj_add_flag(state.hall_carousel_viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(state.hall_carousel_viewport, HallCarouselEvent, LV_EVENT_SCROLL, &state);
    lv_obj_add_event_cb(state.hall_carousel_viewport, HallCarouselEvent, LV_EVENT_SCROLL_END, &state);

    state.hall_carousel_content = lv_obj_create(state.hall_carousel_viewport);
    lv_obj_set_pos(state.hall_carousel_content, 0, 0);
    lv_obj_set_size(state.hall_carousel_content,
                    metalio_claw4::HallCarousel::ContentWidth(visible_count) +
                        (visible_count == 0U ? 0 : metalio_claw4::HallCarousel::kLeft),
                    metalio_claw4::HallCarousel::kCardHeight);
    lv_obj_set_style_pad_all(state.hall_carousel_content, 0, 0);
    lv_obj_set_style_border_width(state.hall_carousel_content, 0, 0);
    lv_obj_set_style_bg_opa(state.hall_carousel_content, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(state.hall_carousel_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state.hall_carousel_content, LV_OBJ_FLAG_CLICKABLE);
    if (visible_count > metalio_claw4::HallCarousel::kFullyVisibleCards) {
        state.hall_scroll_track = lv_obj_create(state.host_smoke);
        lv_obj_set_pos(state.hall_scroll_track, (kWidth - kHallScrollTrackWidth) / 2, 462);
        lv_obj_set_size(state.hall_scroll_track, kHallScrollTrackWidth, kHallScrollTrackHeight);
        lv_obj_set_style_pad_all(state.hall_scroll_track, 0, 0);
        lv_obj_set_style_radius(state.hall_scroll_track, kHallScrollTrackHeight / 2, 0);
        lv_obj_set_style_border_width(state.hall_scroll_track, 0, 0);
        lv_obj_set_style_bg_color(state.hall_scroll_track, lv_color_hex(0x21364eU), 0);
        lv_obj_remove_flag(state.hall_scroll_track, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(state.hall_scroll_track, LV_OBJ_FLAG_CLICKABLE);

        state.hall_scroll_thumb = lv_obj_create(state.hall_scroll_track);
        lv_obj_set_pos(state.hall_scroll_thumb, 0, 0);
        lv_obj_set_size(state.hall_scroll_thumb,
                        metalio_claw4::HallCarousel::ScrollThumbWidth(kHallScrollTrackWidth, visible_count),
                        kHallScrollTrackHeight);
        lv_obj_set_style_pad_all(state.hall_scroll_thumb, 0, 0);
        lv_obj_set_style_radius(state.hall_scroll_thumb, kHallScrollTrackHeight / 2, 0);
        lv_obj_set_style_border_width(state.hall_scroll_thumb, 0, 0);
        lv_obj_set_style_bg_color(state.hall_scroll_thumb, lv_color_hex(0x69a7ffU), 0);
        lv_obj_remove_flag(state.hall_scroll_thumb, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(state.hall_scroll_thumb, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_update_layout(state.hall_carousel_viewport);
    lv_obj_scroll_to_x(state.hall_carousel_viewport, state.hall_scroll_offset, LV_ANIM_OFF);
    UpdateHallCarouselLocked(state, lv_obj_get_scroll_x(state.hall_carousel_viewport));
    if (visible_count == 0U) {
        (void)CreateHallLabel(state.host_smoke, "No readable Bundle in App Store", &lv_font_montserrat_24, 0xf2f7ffU,
                              40, 210);
    }

    if (model.status == host_ui::HallStatus::kAppFailed) {
        (void)CreateHallLabel(state.host_smoke, HallStatusText(model.status), &lv_font_montserrat_18,
                              HallStatusColor(model.status), 40, 560);
        if (model.status_app_id != nullptr && model.status_app_id[0] != '\0') {
            lv_obj_t* app_id =
                CreateHallLabel(state.host_smoke, model.status_app_id, &lv_font_montserrat_14, 0x91a4bdU, 40, 592);
            lv_obj_set_width(app_id, 640);
            lv_label_set_long_mode(app_id, LV_LABEL_LONG_DOT);
        }
        char error_identity[128]{};
        const char* phase = model.status_error_phase != nullptr ? model.status_error_phase : "unknown";
        const char* code = model.status_error_code != nullptr ? model.status_error_code : "app_failed";
        if (model.status_has_exit_code) {
            (void)std::snprintf(error_identity, sizeof(error_identity), "%s / %s / exit=%" PRId32, phase, code,
                                model.status_exit_code);
        } else {
            (void)std::snprintf(error_identity, sizeof(error_identity), "%s / %s", phase, code);
        }
        lv_obj_t* identity =
            CreateHallLabel(state.host_smoke, error_identity, &lv_font_montserrat_14, 0xffb29fU, 40, 618);
        lv_obj_set_width(identity, 640);
        lv_label_set_long_mode(identity, LV_LABEL_LONG_DOT);
        if (model.status_error_detail != nullptr && model.status_error_detail[0] != '\0') {
            lv_obj_t* detail = CreateHallLabel(state.host_smoke, model.status_error_detail, &lv_font_montserrat_14,
                                               0xc8d5e5U, 40, 646);
            lv_obj_set_width(detail, 640);
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        }
    } else {
        if (model.status != host_ui::HallStatus::kReady) {
            (void)CreateHallLabel(state.host_smoke, HallStatusText(model.status), &lv_font_montserrat_18,
                                  HallStatusColor(model.status), 40, 604);
        }
        if (model.status_app_id != nullptr && model.status_app_id[0] != '\0') {
            (void)CreateHallLabel(state.host_smoke, model.status_app_id, &lv_font_montserrat_18, 0x91a4bdU, 40, 638);
        }
    }

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
    if (!background_ready) {
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
        lv_timer_ready(lv_display_get_refr_timer(state.display));
    }
    SetHostPointerEnabledLocked(state, true);
    esp_lv_adapter_unlock();

    if (transition_ready) {
        const HallCardBounds card = HallCard(running_index, state.hall_scroll_offset);
        const bool animated = state.system_transition.Animate(
            state.guest_transition_pixels,
            metalio_claw4::SystemTransitionRect{.x = card.x, .y = card.y, .width = card.width, .height = card.width},
            metalio_claw4::SystemTransitionDirection::kToHall, kGuestTransitionDurationMs, model.transition_trigger_us);
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
            lv_timer_ready(lv_display_get_refr_timer(state.display));
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
    UpdateHallStatusBarLocked(state, model);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();
}

void PauseHallCoverLoadingImpl(MetalioClaw4PlatformState& state) {
    state.hall_cover_request_generation.fetch_add(1U, std::memory_order_acq_rel);
    if (state.hall_cover_job_queue != nullptr) {
        (void)xQueueReset(state.hall_cover_job_queue);
    }
    while (state.hall_cover_worker_active.load(std::memory_order_acquire)) {
        vTaskDelay(1U);
    }
}

void LeaveHallImpl(MetalioClaw4PlatformState& state) {
    PauseHallCoverLoadingImpl(state);
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(state.hall_action_context);
    state.hall_action_sink = nullptr;
    state.hall_action_context = nullptr;
    state.hall_app_count = 0U;
    state.hall_launch_enabled = false;
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
    if (state.display == nullptr || state.host_smoke == nullptr ||
        !state.guest_graphics.RefreshSynchronizationAvailable()) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    SetHostPointerEnabledLocked(state, false);
    state.guest_graphics.DrainRefreshReady();
    const int64_t background_started_us = esp_timer_get_time();
    if (state.system_transition.PrepareBackgroundLocked(state.host_smoke)) {
        ESP_LOGI(kTag, "Hall transition background refreshed before App launch: elapsed=%" PRIu32 " us",
                 static_cast<uint32_t>(esp_timer_get_time() - background_started_us));
    } else {
        ESP_LOGW(kTag, "could not refresh Hall transition background before App launch");
    }
    lv_obj_clean(state.host_smoke);
    lv_obj_set_pos(state.host_smoke, 0, 0);
    lv_obj_set_style_opa(state.host_smoke, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};
    lv_timer_ready(lv_display_get_refr_timer(state.display));
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
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
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
            transition_ready = state.system_transition.PrepareBackgroundLocked(state.host_smoke);
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
            metalio_claw4::SystemTransitionRect{.x = card.x, .y = card.y, .width = card.width, .height = card.width},
            metalio_claw4::SystemTransitionDirection::kToGuest, kGuestTransitionDurationMs);
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
    lv_timer_ready(lv_display_get_refr_timer(state.display));
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
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    SetHostPointerEnabledLocked(state, false);

    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};
    auto result = state.system_menu_ui.ShowLocked(state.host_smoke, state.display, model, action_sink, action_context);
    if (result.has_value() && state.status_layer_ui.PerformanceOverlayVisibleLocked()) {
        state.status_layer_ui.RaisePerformanceOverlayLocked();
    }
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        state.system_menu_ui.Deactivate();
        return result;
    }

    state.input_router.BindTouchSink(metalio_claw4::SystemMenuUi::TouchSink, &state.system_menu_ui);
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
    state.input_router.UnbindTouchSink(&state.system_menu_ui);
    state.input_router.ClearSystemActionSink(action_context);
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
    state.input_router.UnbindTouchSink(&state.system_menu_ui);
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
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
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

std::expected<void, host_ui::SystemUiError> ShowRemoteControlImpl(MetalioClaw4PlatformState& state,
                                                                  const host_ui::RemoteControlModel& model,
                                                                  host_ui::SystemUiActionSink action_sink,
                                                                  void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    void* menu_action_context = state.system_menu_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state.system_menu_ui);
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
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
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
    state.input_router.UnbindTouchSink(&state.system_menu_ui);
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
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
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
    state.input_router.UnbindTouchSink(&state.system_menu_ui);
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
    state.hall_settings_press_overlay = nullptr;
    state.hall_update_press_overlay = nullptr;
    auto result = state.wifi_settings_ui.ShowLocked(
        state.host_smoke, state.display, model, action_sink, action_context,
        [](void* context) { static_cast<metalio_claw4::StatusLayerUi*>(context)->RaisePerformanceOverlayLocked(); },
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
    const bool hardware_started = state.system_transition.BeginStatusLayerTransition(
        true, metalio_claw4::StatusLayerUi::kTransitionScrimRgb, metalio_claw4::StatusLayerUi::kTransitionScrimOpacity,
        trigger_timestamp_us);
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        if (hardware_started) {
            state.system_transition.CancelStatusLayerTransition();
        }
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    const auto log_lvgl_memory = [](const char* phase) {
        lv_mem_monitor_t memory{};
        lv_mem_monitor(&memory);
        ESP_LOGI(kTag, "LVGL pool %s: total=%zu free=%zu largest=%zu used=%u%% frag=%u%% max-used=%zu", phase,
                 memory.total_size, memory.free_size, memory.free_biggest_size, static_cast<unsigned>(memory.used_pct),
                 static_cast<unsigned>(memory.frag_pct), memory.max_used);
    };
    log_lvgl_memory("before status layer");
    auto result = state.status_layer_ui.ShowLocked(model, action_sink, action_context);
    log_lvgl_memory("after status layer");
    bool hardware_finished = false;
    if (result.has_value() && hardware_started) {
        const bool animated = state.system_transition.AnimateStatusLayerLocked(
            state.status_layer_ui.TransitionDialogLocked(), metalio_claw4::StatusLayerUi::kTransitionDialogVisibleY,
            metalio_claw4::StatusLayerUi::kTransitionDialogHiddenY, true, kStatusTransitionDurationMs,
            trigger_timestamp_us);
        if (animated) {
            state.status_layer_ui.SetTransitionProgressLocked(kTransitionComplete);
            hardware_finished = state.system_transition.FinishStatusLayerTransition(true);
        }
        if (!animated || !hardware_finished) {
            state.system_transition.CancelStatusLayerTransition();
        }
    }
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        if (hardware_started) {
            state.system_transition.CancelStatusLayerTransition();
        }
        return result;
    }
    if (!hardware_finished) {
        RunStatusLayerTransition(state, true);
    }
    state.input_router.BindTouchSink(metalio_claw4::StatusLayerUi::TouchSink, &state.status_layer_ui);
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
    state.status_layer_ui.Deactivate();
    const bool hardware_started = state.system_transition.BeginStatusLayerTransition(
        false, metalio_claw4::StatusLayerUi::kTransitionScrimRgb, metalio_claw4::StatusLayerUi::kTransitionScrimOpacity,
        trigger_timestamp_us);
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        if (hardware_started) {
            state.system_transition.CancelStatusLayerTransition();
        }
        return;
    }
    if (hardware_started) {
        const bool animated = state.system_transition.AnimateStatusLayerLocked(
            state.status_layer_ui.TransitionDialogLocked(), metalio_claw4::StatusLayerUi::kTransitionDialogVisibleY,
            metalio_claw4::StatusLayerUi::kTransitionDialogHiddenY, false, kStatusTransitionDurationMs,
            trigger_timestamp_us);
        if (animated) {
            state.status_layer_ui.LeaveLocked();
            const bool finished = state.system_transition.FinishStatusLayerTransition(false);
            if (!finished) {
                state.system_transition.CancelStatusLayerTransition();
            }
            esp_lv_adapter_unlock();
            return;
        }
        state.system_transition.CancelStatusLayerTransition();
    }
    esp_lv_adapter_unlock();
    RunStatusLayerTransition(state, false);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.status_layer_ui.LeaveLocked();
    esp_lv_adapter_unlock();
}

void UpdatePerformanceOverlayImpl(MetalioClaw4PlatformState& state, bool enabled, uint8_t cpu_percent) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.status_layer_ui.UpdatePerformanceOverlayLocked(enabled, cpu_percent,
                                                         state.guest_graphics.DisplayRefreshSequence());
    esp_lv_adapter_unlock();
}

metalio_claw4::GraphicsOperations MakeGraphicsOperations(MetalioClaw4PlatformState& state) {
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

metalio_claw4::SystemUiOperations MakeSystemUiOperations(MetalioClaw4PlatformState& state) {
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
                    static_cast<MetalioClaw4PlatformState*>(context)->hardware.SetBacklightOutputPerTenThousand(output);
                if (status != ESP_OK) {
                    ESP_LOGW(kTag, "could not set backlight brightness: %s", esp_err_to_name(status));
                }
            },
        .apply_volume = [](void*, uint8_t percent) { ConfiguredAudioBackend().SetMasterVolumePercent(percent); },
    };
}

class MetalioClaw4Platform final : public Platform {
   public:
    MetalioClaw4Platform() : graphics_(MakeGraphicsOperations(state_)), system_ui_(MakeSystemUiOperations(state_)) {
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
    [[nodiscard]] device::AudioBackend& audio() override { return ConfiguredAudioBackend(); }
    [[nodiscard]] device::BatteryBackend& battery() override { return state_.battery; }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }
    [[nodiscard]] device::WifiBackend& wifi() override { return wifi_; }
    [[nodiscard]] host_ui::SystemUiBackend& system_ui() override { return system_ui_; }

   private:
    MetalioClaw4PlatformState state_{};
    metalio_claw4::GraphicsAdapter graphics_;
    metalio_claw4::WifiBackend wifi_{};
    metalio_claw4::SystemUiAdapter system_ui_;
};

}  // namespace

Platform& ConfiguredPlatform() {
    static MetalioClaw4Platform platform;
    return platform;
}

}  // namespace micropixel::platform
