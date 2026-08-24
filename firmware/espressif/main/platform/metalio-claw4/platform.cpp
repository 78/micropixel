#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "device/graphics.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
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
#include "platform/metalio-claw4/board_hardware.hpp"
#include "platform/metalio-claw4/display/png_cover_decoder.hpp"
#include "platform/metalio-claw4/display/screen_capture.hpp"
#include "platform/metalio-claw4/graphics_adapter.hpp"
#include "platform/metalio-claw4/guest_graphics_engine.hpp"
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
constexpr int32_t kHallCardWidth = 202;
constexpr int32_t kHallCardRadius = 22;
constexpr int32_t kHallCardBorderWidth = 1;
constexpr uint32_t kHallCoverBytesPerPixel = 3U;
constexpr uint32_t kHallCoverNativeSize = static_cast<uint32_t>(kHallCardWidth);
constexpr uint32_t kHallCoverNativeStride = kHallCoverNativeSize * kHallCoverBytesPerPixel;
constexpr uint32_t kHallCoverNativeBytes = kHallCoverNativeStride * kHallCoverNativeSize;
#if CONFIG_MICROPIXEL_LVGL_PPA_ACCEL
constexpr bool kEnablePpaAccel = true;
#else
constexpr bool kEnablePpaAccel = false;
#endif
constexpr esp_lv_adapter_tear_avoid_mode_t kTearAvoidMode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL;
constexpr const char* kTearAvoidModeName = "double-partial";
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_33;

void* AllocateImageBuffer(size_t size, lv_color_format_t) {
    return heap_caps_aligned_alloc(64U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void FreeImageBuffer(void* buffer) { heap_caps_free(buffer); }

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

    const uint32_t line_bytes = (static_cast<uint32_t>(line_width) *
                                     lv_color_format_get_bpp(static_cast<lv_color_format_t>(destination->header.cf)) +
                                 7U) >>
                                3U;
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
};

// All board-owned display/input state is stored inside the selected Platform
// object. Callbacks receive this state explicitly instead of reaching through
// file-level aliases.
struct MetalioClaw4PlatformState final {
    metalio_claw4::BoardHardware hardware{kWidth, kHeight};
    lv_display_t* display{};
    lv_obj_t* host_smoke{};
    metalio_claw4::GuestGraphicsEngine guest_graphics{kWidth, kHeight};
    lv_obj_t* hall_settings_press_overlay{};
    lv_image_dsc_t launch_image_descriptor{};
    lv_image_dsc_t hall_cover_descriptors[host_ui::kMaxHallApps]{};
    HallCoverCacheEntry hall_cover_cache[host_ui::kMaxHallApps]{};
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
    host_ui::SystemUiActionSink hall_action_sink{};
    void* hall_action_context{};
    uint32_t hall_app_count{};
    uint32_t hall_touch_id{};
    uint32_t hall_touch_card{host_ui::kMaxHallApps};
    int32_t hall_touch_down_x{};
    int32_t hall_touch_down_y{};
    uint64_t hall_touch_down_us{};
    bool hall_touch_close{};
    bool hall_touch_settings{};
    bool hall_app_running[host_ui::kMaxHallApps]{};
    bool hall_touch_active{};
};

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
    lv_draw_buf_handlers_init(lv_draw_buf_get_image_handlers(), AllocateImageBuffer, FreeImageBuffer, CopyImageBuffer,
                              AlignImageBuffer, nullptr, nullptr, ImageWidthToStride);

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
    for (uint32_t index = 0U; index < host_ui::kMaxHallApps; ++index) {
        state.hall_card_press_overlays[index] = nullptr;
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
            return "App stopped after an error";
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

struct HallCardBounds final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

constexpr HallCardBounds kHallSettingsButtonBounds{.x = 496, .y = 20, .width = 200, .height = 100};

HallCardBounds HallCard(uint32_t index) {
    constexpr int32_t kCardLeft = 40;
    constexpr int32_t kCardTop = 164;
    constexpr int32_t kCardHeight = 254;
    constexpr int32_t kCardGap = 17;
    return HallCardBounds{.x = kCardLeft + static_cast<int32_t>(index) * (kHallCardWidth + kCardGap),
                          .y = kCardTop,
                          .width = kHallCardWidth,
                          .height = kCardHeight};
}

HallCardBounds HallCloseButton(uint32_t index) {
    const HallCardBounds card = HallCard(index);
    // Reserve a generous target in the card's top-right corner. A near miss
    // must not fall through to the running card's Resume action.
    constexpr int32_t kCloseSize = 96;
    constexpr int32_t kCloseInset = 0;
    return HallCardBounds{.x = card.x + card.width - kCloseInset - kCloseSize,
                          .y = card.y + kCloseInset,
                          .width = kCloseSize,
                          .height = kCloseSize};
}

bool PointInside(const HallCardBounds& bounds, uint16_t x, uint16_t y) {
    return static_cast<int32_t>(x) >= bounds.x && static_cast<int32_t>(x) < bounds.x + bounds.width &&
           static_cast<int32_t>(y) >= bounds.y && static_cast<int32_t>(y) < bounds.y + bounds.height;
}

void SetHallCardPressed(MetalioClaw4PlatformState& state, uint32_t index, bool pressed) {
    if (index >= host_ui::kMaxHallApps || state.hall_card_press_overlays[index] == nullptr ||
        state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    const bool already_pressed = !lv_obj_has_flag(state.hall_card_press_overlays[index], LV_OBJ_FLAG_HIDDEN);
    if (already_pressed == pressed) {
        esp_lv_adapter_unlock();
        return;
    }
    if (pressed) {
        lv_obj_remove_flag(state.hall_card_press_overlays[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(state.hall_card_press_overlays[index]);
    } else {
        lv_obj_add_flag(state.hall_card_press_overlays[index], LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();
}

void SetHallSettingsPressed(MetalioClaw4PlatformState& state, bool pressed) {
    lv_obj_t* overlay = state.hall_settings_press_overlay;
    if (overlay == nullptr || state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    const bool already_pressed = !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    if (already_pressed != pressed) {
        if (pressed) {
            lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(overlay);
        } else {
            lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        }
        lv_timer_ready(lv_display_get_refr_timer(state.display));
    }
    esp_lv_adapter_unlock();
}

bool HallTouchSink(void* context, const device::TouchSample& sample) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(context);
    if (state == nullptr) {
        return false;
    }
    if (sample.phase == device::TouchPhase::kDown) {
        if (state->hall_touch_active) {
            if (state->hall_touch_settings) {
                SetHallSettingsPressed(*state, false);
            } else {
                SetHallCardPressed(*state, state->hall_touch_card, false);
            }
        }
        state->hall_touch_active = false;
        state->hall_touch_card = host_ui::kMaxHallApps;
        state->hall_touch_close = false;
        state->hall_touch_settings = PointInside(kHallSettingsButtonBounds, sample.x, sample.y);
        if (state->hall_touch_settings) {
            state->hall_touch_id = sample.id;
            state->hall_touch_down_x = sample.x;
            state->hall_touch_down_y = sample.y;
            state->hall_touch_down_us = sample.timestamp_us;
            state->hall_touch_active = true;
            SetHallSettingsPressed(*state, true);
        } else {
            for (uint32_t index = 0U; index < state->hall_app_count; ++index) {
                if (PointInside(HallCard(index), sample.x, sample.y)) {
                    state->hall_touch_id = sample.id;
                    state->hall_touch_card = index;
                    state->hall_touch_down_x = sample.x;
                    state->hall_touch_down_y = sample.y;
                    state->hall_touch_down_us = sample.timestamp_us;
                    state->hall_touch_close =
                        state->hall_app_running[index] && PointInside(HallCloseButton(index), sample.x, sample.y);
                    state->hall_touch_active = true;
                    SetHallCardPressed(*state, index, true);
                    break;
                }
            }
        }
        return true;
    }
    if (!state->hall_touch_active || state->hall_touch_id != sample.id) {
        return true;
    }
    if (sample.phase == device::TouchPhase::kMove) {
        const bool header_action = state->hall_touch_settings;
        const bool inside_target =
            state->hall_touch_settings ? PointInside(kHallSettingsButtonBounds, sample.x, sample.y)
            : state->hall_touch_close  ? PointInside(HallCloseButton(state->hall_touch_card), sample.x, sample.y)
                                       : PointInside(HallCard(state->hall_touch_card), sample.x, sample.y);
        if (header_action) {
            SetHallSettingsPressed(*state, inside_target);
        } else {
            SetHallCardPressed(*state, state->hall_touch_card, inside_target);
        }
        return true;
    }
    if (sample.phase == device::TouchPhase::kCancel) {
        if (state->hall_touch_settings) {
            SetHallSettingsPressed(*state, false);
        } else {
            SetHallCardPressed(*state, state->hall_touch_card, false);
        }
        state->hall_touch_active = false;
        state->hall_touch_card = host_ui::kMaxHallApps;
        state->hall_touch_close = false;
        state->hall_touch_settings = false;
        return true;
    }
    if (sample.phase != device::TouchPhase::kUp) {
        return true;
    }

    constexpr uint64_t kTapTimeoutUs = 800000U;
    const uint32_t card = state->hall_touch_card;
    const bool close = state->hall_touch_close;
    const bool settings = state->hall_touch_settings;
    const uint64_t elapsed_us = sample.timestamp_us >= state->hall_touch_down_us
                                    ? sample.timestamp_us - state->hall_touch_down_us
                                    : kTapTimeoutUs + 1U;
    const bool header_action = settings;
    const bool inside_target = settings ? PointInside(kHallSettingsButtonBounds, sample.x, sample.y)
                               : close  ? PointInside(HallCloseButton(card), sample.x, sample.y)
                                        : PointInside(HallCard(card), sample.x, sample.y);
    const bool accepted =
        (header_action || card < state->hall_app_count) && inside_target && elapsed_us <= kTapTimeoutUs;
    if (header_action) {
        SetHallSettingsPressed(*state, false);
    } else {
        SetHallCardPressed(*state, card, false);
    }
    state->hall_touch_active = false;
    state->hall_touch_card = host_ui::kMaxHallApps;
    state->hall_touch_close = false;
    state->hall_touch_settings = false;
    if (accepted && state->hall_action_sink != nullptr) {
        if (header_action) {
            ESP_LOGI(kTag, "App Hall accepted Settings tap: id=%" PRIu32 " elapsed=%" PRIu64 " us", sample.id,
                     elapsed_us);
            state->hall_action_sink(state->hall_action_context,
                                    host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kOpenSystemMenu});
            return true;
        }
        ESP_LOGI(kTag,
                 "App Hall accepted %s: card=%" PRIu32 " id=%" PRIu32 " down=(%u,%u) up=(%u,%u) elapsed=%" PRIu64 " us",
                 close ? "close" : "tap", card, sample.id, state->hall_touch_down_x, state->hall_touch_down_y, sample.x,
                 sample.y, elapsed_us);
        state->hall_action_sink(state->hall_action_context,
                                host_ui::SystemUiAction{.type = close ? host_ui::SystemUiActionType::kStopApp
                                                                      : host_ui::SystemUiActionType::kLaunchApp,
                                                        .app_index = card});
    }
    return true;
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
    constexpr uint32_t kCacheAlignment = 64U;
    constexpr uint32_t kAllocationBytes =
        (kHallCoverNativeBytes + kCacheAlignment - 1U) / kCacheAlignment * kCacheAlignment;

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

    HallCoverCacheEntry& cache = state.hall_cover_cache[index];
    if (cache.pixels != nullptr && cache.key == source.cache_key) {
        prepared = {.data = cache.pixels,
                    .size = kHallCoverNativeBytes,
                    .width = kHallCoverNativeSize,
                    .height = kHallCoverNativeSize,
                    .stride = kHallCoverNativeStride};
        return true;
    }
    if (cache.pixels != nullptr) {
        heap_caps_free(cache.pixels);
        cache = {};
    }

    auto* pixels = static_cast<uint8_t*>(
        heap_caps_aligned_calloc(kCacheAlignment, kAllocationBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        return false;
    }
    lv_draw_buf_t draw_buf{};
    if (lv_draw_buf_init(&draw_buf, kHallCoverNativeSize, kHallCoverNativeSize, LV_COLOR_FORMAT_RGB888,
                         kHallCoverNativeStride, pixels, kHallCoverNativeBytes) != LV_RESULT_OK) {
        heap_caps_free(pixels);
        return false;
    }
    const int64_t started_us = esp_timer_get_time();
    const bool prepared_ok =
        source.format == host_ui::HallCoverFormat::kPng
            ? metalio_claw4::DecodePngCoverRgb888(source.data, source.size, source.width, source.height, pixels,
                                                  kHallCoverNativeSize, kHallCoverNativeSize, kHallCoverNativeStride,
                                                  0x182d48U)
            : (ScaleRgb888Cover(source.data, source.width, source.height, source.stride, pixels), true);
    if (!prepared_ok) {
        heap_caps_free(pixels);
        ESP_LOGE(kTag, "could not decode Hall PNG: source=%" PRIu32 "x%" PRIu32 " bytes=%" PRIu32, source.width,
                 source.height, source.size);
        return false;
    }
    if (source.format == host_ui::HallCoverFormat::kPng) {
        MaskHallCoverCorners(pixels);
    }
    const uint32_t elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us);
    lv_draw_buf_flush_cache(&draw_buf, nullptr);
    cache = {.pixels = pixels, .key = source.cache_key};
    prepared = {.data = pixels,
                .size = kHallCoverNativeBytes,
                .width = kHallCoverNativeSize,
                .height = kHallCoverNativeSize,
                .stride = kHallCoverNativeStride};
    ESP_LOGI(kTag,
             "prepared native Hall cover: source=%" PRIu32 "x%" PRIu32 " format=%s thumbnail=%" PRIu32 "x%" PRIu32
             " bytes=%" PRIu32 " cpu=%" PRIu32 " us",
             source.width, source.height, source.format == host_ui::HallCoverFormat::kPng ? "PNG" : "RGB888",
             kHallCoverNativeSize, kHallCoverNativeSize, kHallCoverNativeBytes, elapsed_us);
    return true;
}

void DrawHallCard(MetalioClaw4PlatformState& state, lv_obj_t* parent, const host_ui::HallAppModel& app,
                  uint32_t index) {
    constexpr uint32_t kCoverColors[] = {0xff9f43U, 0x4dd6a4U, 0x69a7ffU};
    const HallCardBounds bounds = HallCard(index);
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, bounds.x, bounds.y);
    lv_obj_set_size(card, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_radius(card, kHallCardRadius, 0);
    lv_obj_set_style_border_width(card, kHallCardBorderWidth, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2e4562U), 0);
    lv_obj_set_style_border_post(card, true, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x111f32U), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

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

    host_ui::HallCoverModel prepared_cover{};
    if (ValidHallCover(app.cover) && PrepareNativeHallCover(state, app.cover, index, prepared_cover)) {
        auto& descriptor = state.hall_cover_descriptors[index];
        descriptor = {};
        descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        descriptor.header.cf = LV_COLOR_FORMAT_RGB888;
        descriptor.header.w = prepared_cover.width;
        descriptor.header.h = prepared_cover.height;
        descriptor.header.stride = prepared_cover.stride;
        descriptor.data_size = prepared_cover.size;
        descriptor.data = prepared_cover.data;
        lv_obj_t* image = lv_image_create(cover);
        lv_image_set_src(image, &descriptor);
        lv_obj_center(image);
    } else {
        lv_obj_t* cover_mark = lv_obj_create(cover);
        lv_obj_set_size(cover_mark, 126, 96);
        lv_obj_set_style_radius(cover_mark, 28, 0);
        lv_obj_set_style_border_width(cover_mark, 0, 0);
        lv_obj_set_style_bg_color(cover_mark, lv_color_hex(kCoverColors[index]), 0);
        lv_obj_center(cover_mark);
        lv_obj_t* cover_label = lv_label_create(cover_mark);
        lv_label_set_text(cover_label, AppShortName(app.app_id));
        lv_obj_set_style_text_font(cover_label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(cover_label, lv_color_hex(0x08111fU), 0);
        lv_obj_center(cover_label);
    }

    const char* display_name =
        app.display_name != nullptr && app.display_name[0] != '\0' ? app.display_name : app.app_id;
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
        lv_obj_set_style_bg_opa(close, LV_OPA_90, 0);
        lv_obj_remove_flag(close, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(close, LV_OBJ_FLAG_CLICKABLE);
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

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrameImpl(MetalioClaw4PlatformState& state) {
#if CONFIG_MICROPIXEL_SYSTEM_SHELL_SNAPSHOT
    lv_obj_t* guest_frame = state.guest_graphics.FrameLocked();
    if (state.display == nullptr || guest_frame == nullptr || state.guest_snapshot_pixels != nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    constexpr uint32_t kBytesPerPixel = 3U;
    constexpr uint32_t kSourceStride = static_cast<uint32_t>(kWidth) * kBytesPerPixel;
    constexpr uint32_t kSourceBytes = kSourceStride * static_cast<uint32_t>(kHeight);
    auto* pixels =
        static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, kSourceBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        ESP_LOGE(kTag, "suspended-App snapshot could not allocate %" PRIu32 " PSRAM bytes", kSourceBytes);
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }

    lv_draw_buf_t snapshot{};
    bool captured = lv_draw_buf_init(&snapshot, kWidth, kHeight, LV_COLOR_FORMAT_RGB888, kSourceStride, pixels,
                                     kSourceBytes) == LV_RESULT_OK;
    if (captured && esp_lv_adapter_lock(-1) == ESP_OK) {
        captured = lv_snapshot_take_to_draw_buf(guest_frame, LV_COLOR_FORMAT_RGB888, &snapshot) == LV_RESULT_OK;
        esp_lv_adapter_unlock();
    } else {
        captured = false;
    }
    if (!captured || snapshot.header.w != kWidth || snapshot.header.h != kHeight ||
        snapshot.header.stride != kSourceStride) {
        heap_caps_free(pixels);
        ESP_LOGE(kTag, "LVGL suspended-App snapshot failed");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    // This is a one-time transition snapshot, so compact it on the CPU to the
    // exact Hall viewport. It avoids retaining a transformed image or asking
    // LVGL to scale every refresh.
    constexpr uint32_t kThumbnailWidth = kHallCoverNativeSize;
    constexpr uint32_t kThumbnailHeight = kHallCoverNativeSize;
    constexpr uint32_t kThumbnailStride = kHallCoverNativeStride;
    constexpr uint32_t kThumbnailBytes = kHallCoverNativeBytes;
    constexpr uint32_t kCacheAlignment = 64U;
    constexpr uint32_t kThumbnailAllocationBytes =
        (kThumbnailBytes + kCacheAlignment - 1U) / kCacheAlignment * kCacheAlignment;
    auto* thumbnail = static_cast<uint8_t*>(
        heap_caps_aligned_calloc(kCacheAlignment, kThumbnailAllocationBytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (thumbnail == nullptr) {
        heap_caps_free(pixels);
        ESP_LOGE(kTag, "suspended-App thumbnail could not allocate %" PRIu32 " PSRAM bytes", kThumbnailAllocationBytes);
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }

    lv_draw_buf_t thumbnail_draw_buf{};
    if (lv_draw_buf_init(&thumbnail_draw_buf, kThumbnailWidth, kThumbnailHeight, LV_COLOR_FORMAT_RGB888,
                         kThumbnailStride, thumbnail, kThumbnailBytes) != LV_RESULT_OK) {
        heap_caps_free(thumbnail);
        heap_caps_free(pixels);
        ESP_LOGE(kTag, "could not initialize suspended-App thumbnail draw buffer");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    const int64_t scale_started_us = esp_timer_get_time();
    ScaleRgb888Cover(pixels, kWidth, kHeight, kSourceStride, thumbnail);
    const uint32_t scale_elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - scale_started_us);
    heap_caps_free(pixels);
    lv_draw_buf_flush_cache(&thumbnail_draw_buf, nullptr);

    state.guest_snapshot_pixels = thumbnail;
    ESP_LOGI(kTag,
             "captured suspended-App snapshot: source=%dx%d RGB888 thumbnail=%" PRIu32 "x%" PRIu32 " bytes=%" PRIu32
             " cpu=%" PRIu32 " us",
             kWidth, kHeight, kHallCoverNativeSize, kHallCoverNativeSize, kHallCoverNativeBytes, scale_elapsed_us);
    return host_ui::HallCoverModel{.data = thumbnail,
                                   .size = kHallCoverNativeBytes,
                                   .width = kHallCoverNativeSize,
                                   .height = kHallCoverNativeSize,
                                   .stride = kHallCoverNativeStride};
#else
    (void)state;
    return std::unexpected(host_ui::SystemUiError::kUnavailable);
#endif
}

void ReleaseGuestSnapshotImpl(MetalioClaw4PlatformState& state) {
    if (state.guest_snapshot_pixels == nullptr) {
        return;
    }
    heap_caps_free(state.guest_snapshot_pixels);
    state.guest_snapshot_pixels = nullptr;
    ESP_LOGI(kTag, "released suspended-App snapshot");
}

std::expected<void, host_ui::SystemUiError> ShowHallImpl(MetalioClaw4PlatformState& state,
                                                         const host_ui::HallModel& model,
                                                         host_ui::SystemUiActionSink action_sink,
                                                         void* action_context) {
    if (state.display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    state.touch_input.ClearSmokeUi();
    if (state.host_smoke == nullptr) {
        state.host_smoke = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.host_smoke, 0x08111fU);
    }
    state.hall_settings_press_overlay = nullptr;
    lv_obj_clean(state.host_smoke);
    ResetHallImageDescriptorsLocked(state);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};

    lv_obj_t* brand = lv_obj_create(state.host_smoke);
    lv_obj_set_pos(brand, 40, 40);
    lv_obj_set_size(brand, 18, 44);
    lv_obj_set_style_radius(brand, 9, 0);
    lv_obj_set_style_border_width(brand, 0, 0);
    lv_obj_set_style_bg_color(brand, lv_color_hex(0xff9f43U), 0);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_CLICKABLE);

    (void)CreateHallLabel(state.host_smoke, "App Hall", &lv_font_montserrat_32, 0xf2f7ffU, 76, 36);
    (void)CreateHallLabel(state.host_smoke, "Tap a card to launch", &lv_font_montserrat_18, 0x91a4bdU, 76, 78);
    (void)CreateHallLabel(state.host_smoke, "INSTALLED APPS", &lv_font_montserrat_18, 0x91a4bdU, 40, 128);

    lv_obj_t* settings_button = lv_obj_create(state.host_smoke);
    lv_obj_set_pos(settings_button, 504, 42);
    lv_obj_set_size(settings_button, 176, 60);
    lv_obj_set_style_pad_all(settings_button, 0, 0);
    lv_obj_set_style_radius(settings_button, 20, 0);
    lv_obj_set_style_border_width(settings_button, 1, 0);
    lv_obj_set_style_border_color(settings_button, lv_color_hex(0x42607fU), 0);
    lv_obj_set_style_bg_color(settings_button, lv_color_hex(0x111f32U), 0);
    lv_obj_set_style_bg_opa(settings_button, LV_OPA_COVER, 0);
    lv_obj_remove_flag(settings_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(settings_button, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* settings_icon = lv_label_create(settings_button);
    lv_label_set_text(settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(settings_icon, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_align(settings_icon, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t* settings_label = lv_label_create(settings_button);
    lv_label_set_text(settings_label, "Settings");
    lv_obj_set_style_text_font(settings_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(settings_label, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_align(settings_label, LV_ALIGN_LEFT_MID, 52, 0);

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
    lv_obj_add_flag(state.hall_settings_press_overlay, LV_OBJ_FLAG_HIDDEN);

    const uint32_t visible_count = model.app_count < host_ui::kMaxHallApps ? model.app_count : host_ui::kMaxHallApps;
    for (uint32_t index = 0U; index < visible_count; ++index) {
        DrawHallCard(state, state.host_smoke, model.apps[index], index);
    }
    if (visible_count == 0U) {
        (void)CreateHallLabel(state.host_smoke, "No readable Bundle in App Store", &lv_font_montserrat_24, 0xf2f7ffU,
                              40, 210);
    }

    if (model.status != host_ui::HallStatus::kReady) {
        (void)CreateHallLabel(state.host_smoke, HallStatusText(model.status), &lv_font_montserrat_18,
                              HallStatusColor(model.status), 40, 604);
    }
    if (model.status_app_id != nullptr && model.status_app_id[0] != '\0') {
        (void)CreateHallLabel(state.host_smoke, model.status_app_id, &lv_font_montserrat_18, 0x91a4bdU, 40, 638);
    }
    lv_obj_move_foreground(state.host_smoke);
    if (state.status_layer_ui.PerformanceOverlayVisibleLocked()) {
        // Rebinding the Hall after its status layer closes rebuilds this root.
        // Preserve the final-display HUD z-order in the same locked update so
        // the Hall cannot cover it for one refresh before the next sample.
        state.status_layer_ui.RaisePerformanceOverlayLocked();
    }
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();

    state.hall_action_sink = action_sink;
    state.hall_action_context = action_context;
    state.hall_app_count = model.launch_enabled ? visible_count : 0U;
    state.hall_touch_active = false;
    state.hall_touch_card = host_ui::kMaxHallApps;
    state.hall_touch_close = false;
    state.hall_touch_settings = false;
    state.input_router.BindTouchSink(HallTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    ESP_LOGI(kTag, "App Hall visible: apps=%" PRIu32 " status=%u detail=%" PRIu32, visible_count,
             static_cast<unsigned>(model.status), model.detail);
    return {};
}

void UpdateHallWifiImpl(MetalioClaw4PlatformState& state, const host_ui::HallWifiModel& model) {
    (void)state;
    (void)model;
}

void LeaveHallImpl(MetalioClaw4PlatformState& state) {
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(state.hall_action_context);
    state.hall_action_sink = nullptr;
    state.hall_action_context = nullptr;
    state.hall_app_count = 0U;
    state.hall_touch_active = false;
    state.hall_touch_card = host_ui::kMaxHallApps;
    state.hall_touch_close = false;
    state.hall_touch_settings = false;
    state.hall_settings_press_overlay = nullptr;
    if (state.display == nullptr || state.host_smoke == nullptr ||
        !state.guest_graphics.RefreshSynchronizationAvailable() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    state.guest_graphics.DrainRefreshReady();
    lv_obj_clean(state.host_smoke);
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
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(state.hall_action_context);
    state.hall_action_sink = nullptr;
    state.hall_action_context = nullptr;
    state.hall_app_count = 0U;
    state.hall_touch_active = false;
    state.hall_touch_card = host_ui::kMaxHallApps;
    state.hall_touch_close = false;
    state.hall_touch_settings = false;
    state.hall_settings_press_overlay = nullptr;
    lv_obj_t* guest_frame = state.guest_graphics.FrameLocked();
    if (state.display == nullptr || state.host_smoke == nullptr || guest_frame == nullptr ||
        !state.guest_graphics.RefreshSynchronizationAvailable() || esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    state.guest_graphics.DrainRefreshReady();
    // Delete the Hall and reveal the retained Guest tree in one LVGL update.
    // Rendering an empty opaque Host container first caused a black interval
    // until an event-driven Guest happened to submit its next frame.
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
    ESP_LOGI(kTag, "retained Guest view restored directly from App Hall");
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
    state.hall_touch_active = false;
    state.hall_touch_settings = false;
    state.hall_settings_press_overlay = nullptr;
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
                                                                host_ui::SystemUiActionSink action_sink,
                                                                void* action_context) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    auto result = state.status_layer_ui.ShowLocked(model, action_sink, action_context);
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        return result;
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

void LeaveStatusLayerImpl(MetalioClaw4PlatformState& state) {
    void* action_context = state.status_layer_ui.ActionContext();
    state.input_router.UnbindTouchSink(&state.status_layer_ui);
    state.input_router.ClearSystemActionSink(action_context);
    state.status_layer_ui.Deactivate();
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
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
        .update_hall_wifi =
            [](void* context, const host_ui::HallWifiModel& model) {
                UpdateHallWifiImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
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
            [](void* context) { return CaptureGuestFrameImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .release_guest_snapshot =
            [](void* context) { ReleaseGuestSnapshotImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_system_menu =
            [](void* context, const host_ui::SystemMenuModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowSystemMenuImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                          action_context);
            },
        .leave_system_menu =
            [](void* context) { LeaveSystemMenuImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .show_system_information =
            [](void* context, const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowSystemInformationImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                                 action_context);
            },
        .leave_system_information =
            [](void* context) { LeaveSystemInformationImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
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
            [](void* context, const host_ui::StatusLayerModel& model, host_ui::SystemUiActionSink action_sink,
               void* action_context) {
                return ShowStatusLayerImpl(*static_cast<MetalioClaw4PlatformState*>(context), model, action_sink,
                                           action_context);
            },
        .update_status_layer =
            [](void* context, const host_ui::StatusLayerModel& model) {
                UpdateStatusLayerImpl(*static_cast<MetalioClaw4PlatformState*>(context), model);
            },
        .leave_status_layer =
            [](void* context) { LeaveStatusLayerImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
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
