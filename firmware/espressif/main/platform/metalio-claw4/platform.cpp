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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host_ui/system_gesture_router.hpp"
#include "host_ui/system_ui.hpp"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#if CONFIG_MICROPIXEL_SYSTEM_SHELL_SNAPSHOT
#include "src/draw/snapshot/lv_snapshot.h"
#endif
#include "platform/audio_backend.hpp"
#include "platform/configured_backends.hpp"
#include "platform/graphics/command_stream.hpp"
#include "platform/metalio-claw4/board_hardware.hpp"
#include "platform/metalio-claw4/display/dirty_region_coalescer.hpp"
#include "platform/metalio-claw4/display/png_cover_decoder.hpp"
#include "platform/metalio-claw4/display/retained_scene.hpp"
#include "platform/metalio-claw4/display/screen_capture.hpp"
#include "platform/metalio-claw4/graphics_adapter.hpp"
#include "platform/metalio-claw4/input/gt911_input.hpp"
#include "platform/metalio-claw4/input/tca9555_power_key.hpp"
#include "platform/metalio-claw4/system_ui_adapter.hpp"
#include "platform/platform.hpp"
#include "task_policy.hpp"

namespace micropixel::platform {
namespace {

constexpr char kTag[] = "micropixel_platform";
constexpr int kWidth = 720;
constexpr int kHeight = 720;
constexpr BaseType_t kLvglTaskCore = 1;
constexpr uint32_t kRefreshPeriodMs = 1000;
constexpr uint32_t kBitmapDamageCapacity = 16U;
constexpr uint32_t kMaxSceneTextures = MICROPIXEL_GRAPHICS_MAX_DRAW_OPERATIONS;
constexpr int32_t kHallCardWidth = 218;
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

struct BitmapDamage final {
    const uint8_t* data{};
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

struct HallCoverCacheEntry final {
    uint8_t* pixels{};
    uint64_t key{};
};

enum class StatusTouchTarget : uint8_t {
    kNone,
    kScrim,
    kWifi,
    kCellular,
    kPerformance,
    kBrightness,
    kVolume,
};

// All board-owned display/input state is stored inside the selected Platform
// object. Callbacks receive this state explicitly instead of reaching through
// file-level aliases.
struct MetalioClaw4PlatformState final {
    metalio_claw4::BoardHardware hardware{kWidth, kHeight};
    lv_display_t* display{};
    lv_obj_t* host_smoke{};
    lv_obj_t* guest_frame{};
    lv_obj_t* status_layer{};
    lv_obj_t* status_dialog{};
    lv_obj_t* status_quick_panels[3]{};
    lv_obj_t* status_quick_detail_labels[3]{};
    lv_obj_t* status_quick_press_overlays[3]{};
    lv_obj_t* status_slider_value_labels[2]{};
    lv_obj_t* status_slider_fills[2]{};
    lv_obj_t* status_slider_knobs[2]{};
    lv_obj_t* performance_overlay{};
    lv_obj_t* performance_cpu_label{};
    lv_obj_t* performance_fps_label{};
    lv_image_dsc_t launch_image_descriptor{};
    lv_image_dsc_t hall_cover_descriptors[host_ui::kMaxHallApps]{};
    HallCoverCacheEntry hall_cover_cache[host_ui::kMaxHallApps]{};
    lv_obj_t* hall_card_press_overlays[host_ui::kMaxHallApps]{};
    metalio_claw4::DirtyRegionCoalescer dirty_region_coalescer{};
    metalio_claw4::RetainedScene retained_scene{kWidth, kHeight};
    metalio_claw4::Tca9555PowerKey power_key{};
    metalio_claw4::Gt911Input touch_input{kWidth, kHeight, kTouchInterrupt};
    host_ui::SystemGestureRouter input_router{touch_input, kWidth, kHeight};
    uint8_t* guest_snapshot_pixels{};
    BitmapDamage bitmap_damage[kBitmapDamageCapacity]{};
    uint64_t bitmap_frame_started_us{};
    uint64_t bitmap_frame_bytes{};
    uint32_t bitmap_frame_updates{};
    uint32_t bitmap_frame_sequence{};
    uint32_t bitmap_damage_count{};
    uint8_t* graphics_frame_bytes{};
    uint32_t graphics_frame_length{};
    uint32_t graphics_frame_commands{};
    micropixel_texture_handle_t graphics_frame_textures[kMaxSceneTextures]{};
    uint32_t graphics_frame_texture_count{};
    device::TextureAccess graphics_frame_texture_access{};
    micropixel_texture_handle_t scene_textures[kMaxSceneTextures]{};
    uint32_t scene_texture_count{};
    device::TextureAccess scene_texture_access{};
    uint32_t graphics_frame_sequence{};
    uint32_t presented_guest_frame_sequence{};
    uint32_t performance_last_frame_sequence{};
    uint32_t performance_fps{};
    int64_t performance_last_sample_us{};
    uint32_t display_refresh_sequence{};
    int64_t display_refresh_started_us{};
    StaticSemaphore_t display_refresh_ready_storage{};
    SemaphoreHandle_t display_refresh_ready{};
    host_ui::SystemUiActionSink hall_action_sink{};
    void* hall_action_context{};
    host_ui::SystemUiActionSink status_action_sink{};
    void* status_action_context{};
    uint32_t hall_app_count{};
    uint32_t hall_touch_id{};
    uint32_t hall_touch_card{host_ui::kMaxHallApps};
    int32_t hall_touch_down_x{};
    int32_t hall_touch_down_y{};
    uint64_t hall_touch_down_us{};
    bool hall_touch_close{};
    bool hall_app_running[host_ui::kMaxHallApps]{};
    uint32_t status_touch_id{};
    StatusTouchTarget status_touch_target{StatusTouchTarget::kNone};
    int32_t status_touch_down_x{};
    int32_t status_touch_down_y{};
    uint64_t status_touch_down_us{};
    uint64_t status_slider_last_emit_us{};
    uint8_t status_slider_values[2]{};
    uint8_t status_slider_last_emitted_values[2]{0xffU, 0xffU};
    bool hall_touch_active{};
    bool status_touch_active{};
    bool status_button_pressed{};
    bool bitmap_update_frame_active{};
    bool graphics_frame_active{};
};

bool SameTextureAccess(const device::TextureAccess& left, const device::TextureAccess& right) {
    return left.context == right.context && left.resolve == right.resolve && left.retain == right.retain &&
           left.release == right.release;
}

void ReleaseTextures(const device::TextureAccess& access, const micropixel_texture_handle_t* textures, uint32_t count) {
    if (access.release == nullptr) {
        return;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        access.release(access.context, textures[index]);
    }
}

bool RetainTextures(const device::TextureAccess& access, const micropixel_texture_handle_t* textures, uint32_t count) {
    if (count != 0U && (access.retain == nullptr || access.release == nullptr)) {
        return false;
    }
    uint32_t retained = 0U;
    for (; retained < count; ++retained) {
        if (!access.retain(access.context, textures[retained])) {
            break;
        }
    }
    if (retained == count) {
        return true;
    }
    ReleaseTextures(access, textures, retained);
    return false;
}

void ClearPendingFrameTextures(MetalioClaw4PlatformState& state, bool release) {
    if (release) {
        ReleaseTextures(state.graphics_frame_texture_access, state.graphics_frame_textures,
                        state.graphics_frame_texture_count);
    }
    state.graphics_frame_texture_count = 0U;
    state.graphics_frame_texture_access = {};
}

bool AddPendingFrameTextures(MetalioClaw4PlatformState& state, const micropixel_texture_handle_t* textures,
                             uint32_t count, const device::TextureAccess& access) {
    if (state.graphics_frame_texture_count != 0U && !SameTextureAccess(state.graphics_frame_texture_access, access)) {
        return false;
    }
    const uint32_t original_count = state.graphics_frame_texture_count;
    for (uint32_t index = 0U; index < count; ++index) {
        bool exists = false;
        for (uint32_t current = 0U; current < state.graphics_frame_texture_count; ++current) {
            if (state.graphics_frame_textures[current] == textures[index]) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        if (state.graphics_frame_texture_count >= kMaxSceneTextures || access.retain == nullptr ||
            !access.retain(access.context, textures[index])) {
            ReleaseTextures(access, state.graphics_frame_textures + original_count,
                            state.graphics_frame_texture_count - original_count);
            state.graphics_frame_texture_count = original_count;
            return false;
        }
        state.graphics_frame_textures[state.graphics_frame_texture_count++] = textures[index];
    }
    if (original_count == 0U) {
        state.graphics_frame_texture_access = access;
    }
    return true;
}

bool AccumulateDamage(BitmapDamage* damages, uint32_t& damage_count, const uint8_t* data, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height) {
    for (uint32_t index = 0U; index < damage_count; ++index) {
        BitmapDamage& damage = damages[index];
        if (damage.data != data) {
            continue;
        }
        const uint32_t right = x + width;
        const uint32_t bottom = y + height;
        const uint32_t damage_right = damage.x + damage.width;
        const uint32_t damage_bottom = damage.y + damage.height;
        const uint32_t union_left = x < damage.x ? x : damage.x;
        const uint32_t union_top = y < damage.y ? y : damage.y;
        const uint32_t union_right = right > damage_right ? right : damage_right;
        const uint32_t union_bottom = bottom > damage_bottom ? bottom : damage_bottom;
        damage.x = union_left;
        damage.y = union_top;
        damage.width = union_right - union_left;
        damage.height = union_bottom - union_top;
        return true;
    }
    if (damage_count >= kBitmapDamageCapacity) {
        return false;
    }
    damages[damage_count++] = BitmapDamage{data, x, y, width, height};
    return true;
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

void DisplayRefreshStartEvent(lv_event_t* event) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(lv_event_get_user_data(event));
    if (state != nullptr) {
        state->dirty_region_coalescer.Coalesce(state->display);
        state->display_refresh_started_us = esp_timer_get_time();
    }
}

void DisplayRefreshReadyEvent(lv_event_t* event) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(lv_event_get_user_data(event));
    if (state != nullptr) {
        if (state->display_refresh_started_us != 0) {
            const uint32_t sequence = ++state->display_refresh_sequence;
            const uint32_t duration_us =
                static_cast<uint32_t>(esp_timer_get_time() - state->display_refresh_started_us);
            state->display_refresh_started_us = 0;
            if (sequence <= 4U || (sequence % 60U) == 0U) {
                ESP_LOGI(kTag, "display refresh #%" PRIu32 ": total=%" PRIu32 " us", sequence, duration_us);
            }
        }
        if (state->display_refresh_ready != nullptr) {
            (void)xSemaphoreGive(state->display_refresh_ready);
        }
    }
}

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
    state.display_refresh_ready = xSemaphoreCreateBinaryStatic(&state.display_refresh_ready_storage);
    if (state.display_refresh_ready == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_lv_adapter_config_t adapter_config{};
    adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_config.stack_in_psram = false;
    adapter_config.task_priority = task_policy::kDisplayPriority;
    adapter_config.task_core_id = kLvglTaskCore;
    adapter_config.tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS;
    adapter_config.task_min_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS;
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
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    state.retained_scene.BindSurface(state.display, state.hardware.Panel());
#endif
    lv_display_add_event_cb(state.display, DisplayRefreshStartEvent, LV_EVENT_REFR_START, &state);
    lv_display_add_event_cb(state.display, DisplayRefreshReadyEvent, LV_EVENT_REFR_READY, &state);
    lv_timer_set_period(lv_display_get_refr_timer(state.display), kRefreshPeriodMs);

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

bool GraphicsBackendAvailableImpl() { return true; }

int32_t GraphicsGetInfoImpl(const MetalioClaw4PlatformState& state, micropixel_graphics_info_t& info) {
    if (state.display == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    info = {};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    info.width = kWidth;
    info.height = kHeight;
    info.pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888;
    info.capabilities = 0U;
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    info.capabilities |= MICROPIXEL_GRAPHICS_CAP_RETAINED_TRANSLATION;
#endif
    info.capabilities |= MICROPIXEL_GRAPHICS_CAP_MULTI_SUBMIT_FRAME;
    info.max_command_bytes = MICROPIXEL_GRAPHICS_MAX_COMMAND_BYTES;
    info.max_commands = MICROPIXEL_GRAPHICS_MAX_COMMANDS;
    info.max_draw_operations = MICROPIXEL_GRAPHICS_MAX_DRAW_OPERATIONS;
    info.max_frame_commands = MICROPIXEL_GRAPHICS_MAX_FRAME_COMMANDS;
    return MICROPIXEL_STATUS_OK;
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

HallCardBounds HallCard(uint32_t index) {
    constexpr int32_t kCardLeft = 17;
    constexpr int32_t kCardTop = 164;
    constexpr int32_t kCardHeight = 270;
    constexpr int32_t kCardGap = 16;
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

bool HallTouchSink(void* context, const device::TouchSample& sample) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(context);
    if (state == nullptr) {
        return false;
    }
    if (sample.phase == device::TouchPhase::kDown) {
        if (state->hall_touch_active) {
            SetHallCardPressed(*state, state->hall_touch_card, false);
        }
        state->hall_touch_active = false;
        state->hall_touch_card = host_ui::kMaxHallApps;
        state->hall_touch_close = false;
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
        return true;
    }
    if (!state->hall_touch_active || state->hall_touch_id != sample.id) {
        return true;
    }
    if (sample.phase == device::TouchPhase::kMove) {
        const bool inside_target = state->hall_touch_close
                                       ? PointInside(HallCloseButton(state->hall_touch_card), sample.x, sample.y)
                                       : PointInside(HallCard(state->hall_touch_card), sample.x, sample.y);
        SetHallCardPressed(*state, state->hall_touch_card, inside_target);
        return true;
    }
    if (sample.phase == device::TouchPhase::kCancel) {
        SetHallCardPressed(*state, state->hall_touch_card, false);
        state->hall_touch_active = false;
        state->hall_touch_card = host_ui::kMaxHallApps;
        state->hall_touch_close = false;
        return true;
    }
    if (sample.phase != device::TouchPhase::kUp) {
        return true;
    }

    constexpr uint64_t kTapTimeoutUs = 800000U;
    const uint32_t card = state->hall_touch_card;
    const bool close = state->hall_touch_close;
    const uint64_t elapsed_us = sample.timestamp_us >= state->hall_touch_down_us
                                    ? sample.timestamp_us - state->hall_touch_down_us
                                    : kTapTimeoutUs + 1U;
    const bool inside_target = close ? PointInside(HallCloseButton(card), sample.x, sample.y)
                                     : PointInside(HallCard(card), sample.x, sample.y);
    const bool accepted = card < state->hall_app_count && inside_target && elapsed_us <= kTapTimeoutUs;
    SetHallCardPressed(*state, card, false);
    state->hall_touch_active = false;
    state->hall_touch_card = host_ui::kMaxHallApps;
    state->hall_touch_close = false;
    if (accepted && state->hall_action_sink != nullptr) {
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
        lv_obj_set_pos(running, 12, 12);
        lv_obj_set_size(running, 132, 48);
        lv_obj_set_style_radius(running, 24, 0);
        lv_obj_set_style_border_width(running, 0, 0);
        lv_obj_set_style_bg_color(running, lv_color_hex(0x08111fU), 0);
        lv_obj_set_style_bg_opa(running, LV_OPA_90, 0);
        lv_obj_remove_flag(running, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(running, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* running_label = lv_label_create(running);
        lv_label_set_text(running_label, "RUNNING");
        lv_obj_set_style_text_font(running_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(running_label, lv_color_hex(0x4dd6a4U), 0);
        lv_obj_set_style_text_opa(running_label, LV_OPA_COVER, 0);
        lv_obj_center(running_label);

        lv_obj_t* close = lv_obj_create(card);
        lv_obj_set_pos(close, bounds.width - 62, 12);
        lv_obj_set_size(close, 50, 50);
        lv_obj_set_style_radius(close, 25, 0);
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
    if (state.display == nullptr || state.guest_frame == nullptr || state.guest_snapshot_pixels != nullptr) {
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
        captured = lv_snapshot_take_to_draw_buf(state.guest_frame, LV_COLOR_FORMAT_RGB888, &snapshot) == LV_RESULT_OK;
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
    if (state.performance_overlay != nullptr && !lv_obj_has_flag(state.performance_overlay, LV_OBJ_FLAG_HIDDEN)) {
        // Rebinding the Hall after its status layer closes rebuilds this root.
        // Preserve the final-display HUD z-order in the same locked update so
        // the Hall cannot cover it for one refresh before the next sample.
        lv_obj_move_foreground(state.performance_overlay);
    }
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();

    state.hall_action_sink = action_sink;
    state.hall_action_context = action_context;
    state.hall_app_count = model.launch_enabled ? visible_count : 0U;
    state.hall_touch_active = false;
    state.hall_touch_card = host_ui::kMaxHallApps;
    state.hall_touch_close = false;
    state.input_router.BindTouchSink(HallTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    ESP_LOGI(kTag, "App Hall visible: apps=%" PRIu32 " status=%u detail=%" PRIu32, visible_count,
             static_cast<unsigned>(model.status), model.detail);
    return {};
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
    if (state.display == nullptr || state.host_smoke == nullptr || state.display_refresh_ready == nullptr ||
        esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    while (xSemaphoreTake(state.display_refresh_ready, 0U) == pdTRUE) {
    }
    lv_obj_clean(state.host_smoke);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(0x08111fU), 0);
    state.launch_image_descriptor = {};
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();

    // Retire the LVGL descriptors before FirmwareApp unmaps Flash covers. The
    // native PSRAM thumbnails remain cached for the next Hall visit.
    (void)xSemaphoreTake(state.display_refresh_ready, portMAX_DELAY);
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
    if (state.display == nullptr || state.host_smoke == nullptr || state.guest_frame == nullptr ||
        state.display_refresh_ready == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    while (xSemaphoreTake(state.display_refresh_ready, 0U) == pdTRUE) {
    }
    // Delete the Hall and reveal the retained Guest tree in one LVGL update.
    // Rendering an empty opaque Host container first caused a black interval
    // until an event-driven Guest happened to submit its next frame.
    lv_obj_delete(state.host_smoke);
    state.host_smoke = nullptr;
    state.launch_image_descriptor = {};
    lv_obj_move_foreground(state.guest_frame);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();

    (void)xSemaphoreTake(state.display_refresh_ready, portMAX_DELAY);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ResetHallImageDescriptorsLocked(state);
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(kTag, "retained Guest view restored directly from App Hall");
    return {};
}

constexpr HallCardBounds kStatusDialogBounds{.x = 24, .y = 36, .width = 672, .height = 508};
constexpr uint32_t kStatusControlColor = 0x68a7ffU;

HallCardBounds StatusTargetBounds(StatusTouchTarget target) {
    switch (target) {
        case StatusTouchTarget::kWifi:
            return {.x = 48, .y = 52, .width = 196, .height = 108};
        case StatusTouchTarget::kCellular:
            return {.x = 262, .y = 52, .width = 196, .height = 108};
        case StatusTouchTarget::kPerformance:
            return {.x = 476, .y = 52, .width = 196, .height = 108};
        case StatusTouchTarget::kBrightness:
            return {.x = 48, .y = 184, .width = 624, .height = 80};
        case StatusTouchTarget::kVolume:
            return {.x = 48, .y = 284, .width = 624, .height = 80};
        case StatusTouchTarget::kScrim:
        case StatusTouchTarget::kNone:
            break;
    }
    return {};
}

HallCardBounds StatusDialogRelative(const HallCardBounds& bounds) {
    return {.x = bounds.x - kStatusDialogBounds.x,
            .y = bounds.y - kStatusDialogBounds.y,
            .width = bounds.width,
            .height = bounds.height};
}

int32_t StatusQuickIndex(StatusTouchTarget target) {
    switch (target) {
        case StatusTouchTarget::kWifi:
            return 0;
        case StatusTouchTarget::kCellular:
            return 1;
        case StatusTouchTarget::kPerformance:
            return 2;
        default:
            return -1;
    }
}

int32_t StatusSliderIndex(StatusTouchTarget target) {
    switch (target) {
        case StatusTouchTarget::kBrightness:
            return 0;
        case StatusTouchTarget::kVolume:
            return 1;
        default:
            return -1;
    }
}

StatusTouchTarget FindStatusTouchTarget(uint16_t x, uint16_t y) {
    constexpr StatusTouchTarget kTargets[] = {
        StatusTouchTarget::kPerformance,
        StatusTouchTarget::kBrightness,
        StatusTouchTarget::kVolume,
    };
    for (StatusTouchTarget target : kTargets) {
        if (PointInside(StatusTargetBounds(target), x, y)) {
            return target;
        }
    }
    return PointInside(kStatusDialogBounds, x, y) ? StatusTouchTarget::kNone : StatusTouchTarget::kScrim;
}

void EmitStatusAction(MetalioClaw4PlatformState& state, host_ui::SystemUiActionType type, uint32_t value = 0U) {
    if (state.status_action_sink != nullptr) {
        state.status_action_sink(state.status_action_context, host_ui::SystemUiAction{.type = type, .value = value});
    }
}

uint8_t StatusSliderMinimum(StatusTouchTarget target) {
    return target == StatusTouchTarget::kBrightness ? host_ui::kMinimumBrightnessPercent : 0U;
}

uint8_t ClampStatusSliderValue(StatusTouchTarget target, uint8_t percent) {
    const uint8_t minimum = StatusSliderMinimum(target);
    const uint8_t maximum_clamped = percent <= 100U ? percent : 100U;
    return maximum_clamped >= minimum ? maximum_clamped : minimum;
}

uint32_t StatusSliderPositionPercent(StatusTouchTarget target, uint8_t percent) {
    const uint32_t minimum = StatusSliderMinimum(target);
    const uint32_t clamped = ClampStatusSliderValue(target, percent);
    return (clamped - minimum) * 100U / (100U - minimum);
}

uint32_t StatusSliderValue(StatusTouchTarget target, uint16_t x) {
    constexpr uint32_t kSliderLeft = 68U;
    constexpr uint32_t kSliderWidth = 584U;
    const uint32_t clamped_x =
        x < kSliderLeft ? kSliderLeft : (x > kSliderLeft + kSliderWidth ? kSliderLeft + kSliderWidth : x);
    const uint32_t position_percent = (clamped_x - kSliderLeft) * 100U / kSliderWidth;
    const uint32_t minimum = StatusSliderMinimum(target);
    return minimum + position_percent * (100U - minimum) / 100U;
}

void EmitStatusTarget(MetalioClaw4PlatformState& state, StatusTouchTarget target, uint16_t x) {
    switch (target) {
        case StatusTouchTarget::kWifi:
        case StatusTouchTarget::kCellular:
            // Connectivity is read-only until a real network service owns
            // these controls; do not emit a pretend toggle action.
            break;
        case StatusTouchTarget::kPerformance:
            EmitStatusAction(state, host_ui::SystemUiActionType::kTogglePerformanceOverlay);
            break;
        case StatusTouchTarget::kBrightness:
            EmitStatusAction(state, host_ui::SystemUiActionType::kSetBrightness, StatusSliderValue(target, x));
            break;
        case StatusTouchTarget::kVolume:
            EmitStatusAction(state, host_ui::SystemUiActionType::kSetVolume, StatusSliderValue(target, x));
            break;
        case StatusTouchTarget::kScrim:
            EmitStatusAction(state, host_ui::SystemUiActionType::kCloseStatusLayer);
            break;
        case StatusTouchTarget::kNone:
            break;
    }
}

void SetStatusButtonPressedLocked(MetalioClaw4PlatformState& state, StatusTouchTarget target, bool pressed) {
    const int32_t index = StatusQuickIndex(target);
    if (index < 0 || state.status_quick_press_overlays[index] == nullptr) {
        return;
    }
    if (pressed) {
        lv_obj_remove_flag(state.status_quick_press_overlays[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(state.status_quick_press_overlays[index]);
    } else {
        lv_obj_add_flag(state.status_quick_press_overlays[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void SetStatusButtonPressed(MetalioClaw4PlatformState& state, StatusTouchTarget target, bool pressed) {
    if (StatusQuickIndex(target) < 0 || state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    SetStatusButtonPressedLocked(state, target, pressed);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();
}

void UpdateStatusSliderLocked(MetalioClaw4PlatformState& state, StatusTouchTarget target, uint8_t percent,
                              bool pressed) {
    constexpr int32_t kTrackWidth = 584;
    const int32_t index = StatusSliderIndex(target);
    if (index < 0 || state.status_slider_value_labels[index] == nullptr ||
        state.status_slider_fills[index] == nullptr || state.status_slider_knobs[index] == nullptr) {
        return;
    }
    const uint8_t clamped_percent = ClampStatusSliderValue(target, percent);
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_label_set_text(state.status_slider_value_labels[index], value);
    const int32_t fill_width =
        static_cast<int32_t>(StatusSliderPositionPercent(target, clamped_percent)) * kTrackWidth / 100;
    lv_obj_set_width(state.status_slider_fills[index], fill_width > 0 ? fill_width : 1);
    const HallCardBounds bounds = StatusDialogRelative(StatusTargetBounds(target));
    lv_obj_set_x(state.status_slider_knobs[index], bounds.x + 20 + fill_width - 14);
    (void)pressed;
    lv_obj_set_style_border_width(state.status_slider_knobs[index], 3, 0);
    state.status_slider_values[index] = clamped_percent;
}

void UpdateStatusSlider(MetalioClaw4PlatformState& state, StatusTouchTarget target, uint8_t percent, bool pressed) {
    if (StatusSliderIndex(target) < 0 || state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    UpdateStatusSliderLocked(state, target, percent, pressed);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();
}

void EmitStatusSliderValue(MetalioClaw4PlatformState& state, StatusTouchTarget target, uint8_t percent,
                           uint64_t timestamp_us, bool force) {
    constexpr uint64_t kHardwareUpdatePeriodUs = 50000U;
    const int32_t index = StatusSliderIndex(target);
    if (index < 0) {
        return;
    }
    const uint64_t now_us = timestamp_us != 0U ? timestamp_us : static_cast<uint64_t>(esp_timer_get_time());
    const bool changed = state.status_slider_last_emitted_values[index] != percent;
    const bool period_elapsed = now_us < state.status_slider_last_emit_us ||
                                now_us - state.status_slider_last_emit_us >= kHardwareUpdatePeriodUs;
    if (!force && (!changed || !period_elapsed)) {
        return;
    }
    state.status_slider_last_emitted_values[index] = percent;
    state.status_slider_last_emit_us = now_us;
    EmitStatusAction(state,
                     target == StatusTouchTarget::kBrightness ? host_ui::SystemUiActionType::kSetBrightness
                                                              : host_ui::SystemUiActionType::kSetVolume,
                     percent);
}

bool StatusTouchSink(void* context, const device::TouchSample& sample) {
    auto* state = static_cast<MetalioClaw4PlatformState*>(context);
    if (state == nullptr) {
        return false;
    }
    if (sample.phase == device::TouchPhase::kDown) {
        state->status_touch_target = FindStatusTouchTarget(sample.x, sample.y);
        state->status_touch_id = sample.id;
        state->status_touch_down_x = sample.x;
        state->status_touch_down_y = sample.y;
        state->status_touch_down_us = sample.timestamp_us;
        state->status_touch_active = true;
        state->status_button_pressed = StatusQuickIndex(state->status_touch_target) >= 0;
        if (state->status_button_pressed) {
            SetStatusButtonPressed(*state, state->status_touch_target, true);
        }
        const int32_t slider_index = StatusSliderIndex(state->status_touch_target);
        if (slider_index >= 0) {
            const uint8_t percent = static_cast<uint8_t>(StatusSliderValue(state->status_touch_target, sample.x));
            UpdateStatusSlider(*state, state->status_touch_target, percent, true);
            EmitStatusSliderValue(*state, state->status_touch_target, percent, sample.timestamp_us, true);
        }
        return true;
    }
    if (!state->status_touch_active || state->status_touch_id != sample.id) {
        return true;
    }

    const StatusTouchTarget target = state->status_touch_target;
    const int32_t delta_x = static_cast<int32_t>(sample.x) - state->status_touch_down_x;
    const int32_t delta_y = static_cast<int32_t>(sample.y) - state->status_touch_down_y;
    if (sample.phase == device::TouchPhase::kCancel) {
        if (state->status_button_pressed) {
            SetStatusButtonPressed(*state, target, false);
        }
        const int32_t slider_index = StatusSliderIndex(target);
        if (slider_index >= 0) {
            UpdateStatusSlider(*state, target, state->status_slider_values[slider_index], false);
            EmitStatusSliderValue(*state, target, state->status_slider_values[slider_index], sample.timestamp_us, true);
        }
        state->status_touch_active = false;
        state->status_button_pressed = false;
        state->status_touch_target = StatusTouchTarget::kNone;
        return true;
    }
    if (sample.phase == device::TouchPhase::kMove) {
        const int32_t quick_index = StatusQuickIndex(target);
        if (quick_index >= 0) {
            const bool pressed = PointInside(StatusTargetBounds(target), sample.x, sample.y);
            if (pressed != state->status_button_pressed) {
                state->status_button_pressed = pressed;
                SetStatusButtonPressed(*state, target, pressed);
            }
        }
        const int32_t slider_index = StatusSliderIndex(target);
        if (slider_index >= 0) {
            const uint8_t percent = static_cast<uint8_t>(StatusSliderValue(target, sample.x));
            if (state->status_slider_values[slider_index] != percent) {
                UpdateStatusSlider(*state, target, percent, true);
            }
            EmitStatusSliderValue(*state, target, percent, sample.timestamp_us, false);
        }
        return true;
    }
    if (sample.phase != device::TouchPhase::kUp) {
        return true;
    }

    constexpr int32_t kTapSlop = 24;
    constexpr int32_t kSwipeDistance = 56;
    constexpr int32_t kSwipeDirectionSlop = 80;
    constexpr uint64_t kGestureTimeoutUs = 800000U;
    const uint64_t elapsed_us = sample.timestamp_us >= state->status_touch_down_us
                                    ? sample.timestamp_us - state->status_touch_down_us
                                    : kGestureTimeoutUs + 1U;
    const bool swipe_up =
        delta_y <= -kSwipeDistance && std::abs(delta_x) <= kSwipeDirectionSlop && elapsed_us <= kGestureTimeoutUs;
    if (state->status_button_pressed || StatusQuickIndex(target) >= 0) {
        SetStatusButtonPressed(*state, target, false);
    }
    state->status_touch_active = false;
    state->status_button_pressed = false;
    state->status_touch_target = StatusTouchTarget::kNone;
    if (swipe_up) {
        EmitStatusAction(*state, host_ui::SystemUiActionType::kCloseStatusLayer);
        return true;
    }

    const int32_t slider_index = StatusSliderIndex(target);
    if (slider_index >= 0) {
        const uint8_t percent = static_cast<uint8_t>(StatusSliderValue(target, sample.x));
        UpdateStatusSlider(*state, target, percent, false);
        EmitStatusSliderValue(*state, target, percent, sample.timestamp_us, true);
    } else if (target == StatusTouchTarget::kScrim && !PointInside(kStatusDialogBounds, sample.x, sample.y) &&
               std::abs(delta_x) <= kTapSlop && std::abs(delta_y) <= kTapSlop && elapsed_us <= kGestureTimeoutUs) {
        EmitStatusTarget(*state, target, sample.x);
    } else if (StatusQuickIndex(target) >= 0 && PointInside(StatusTargetBounds(target), sample.x, sample.y) &&
               std::abs(delta_x) <= kTapSlop && std::abs(delta_y) <= kTapSlop && elapsed_us <= kGestureTimeoutUs) {
        EmitStatusTarget(*state, target, sample.x);
    }
    return true;
}

lv_obj_t* CreateStatusPanel(lv_obj_t* parent, const HallCardBounds& bounds, uint32_t color) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, bounds.x, bounds.y);
    lv_obj_set_size(panel, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x42607fU), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

void DrawStatusQuickCard(MetalioClaw4PlatformState& state, lv_obj_t* root, StatusTouchTarget target, const char* name,
                         const char* detail, bool active, bool available) {
    const uint32_t color = !available ? 0x182331U : (active ? 0x1b765fU : 0x26384dU);
    const int32_t index = StatusQuickIndex(target);
    lv_obj_t* panel = CreateStatusPanel(root, StatusDialogRelative(StatusTargetBounds(target)), color);
    lv_obj_set_style_bg_opa(panel, available && !active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    (void)CreateHallLabel(panel, name, &lv_font_montserrat_24, available ? 0xf4f8ffU : 0x708198U, 18, 20);
    lv_obj_t* detail_label =
        CreateHallLabel(panel, detail, &lv_font_montserrat_18, active && available ? 0x8ff0cfU : 0x91a4bdU, 18, 67);
    if (index >= 0) {
        lv_obj_t* press_overlay = lv_obj_create(panel);
        lv_obj_set_pos(press_overlay, 0, 0);
        lv_obj_set_size(press_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_pad_all(press_overlay, 0, 0);
        lv_obj_set_style_radius(press_overlay, 24, 0);
        lv_obj_set_style_border_width(press_overlay, 0, 0);
        lv_obj_set_style_bg_color(press_overlay, lv_color_hex(0x000000U), 0);
        lv_obj_set_style_bg_opa(press_overlay, 92, 0);
        lv_obj_remove_flag(press_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(press_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(press_overlay, LV_OBJ_FLAG_HIDDEN);
        state.status_quick_panels[index] = panel;
        state.status_quick_detail_labels[index] = detail_label;
        state.status_quick_press_overlays[index] = press_overlay;
    }
}

void DrawStatusSlider(MetalioClaw4PlatformState& state, lv_obj_t* root, StatusTouchTarget target, const char* name,
                      uint8_t percent, uint32_t color) {
    const HallCardBounds bounds = StatusDialogRelative(StatusTargetBounds(target));
    const int32_t index = StatusSliderIndex(target);
    const uint8_t clamped_percent = ClampStatusSliderValue(target, percent);
    (void)CreateHallLabel(root, name, &lv_font_montserrat_18, 0xf4f8ffU, bounds.x + 20, bounds.y + 4);
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_obj_t* value_label =
        CreateHallLabel(root, value, &lv_font_montserrat_18, 0x91a4bdU, bounds.x + bounds.width - 62, bounds.y + 4);

    constexpr int32_t kTrackLeftInset = 20;
    constexpr int32_t kTrackWidth = 584;
    constexpr int32_t kTrackHeight = 14;
    const int32_t track_y = bounds.y + 47;
    lv_obj_t* track = lv_obj_create(root);
    lv_obj_set_pos(track, bounds.x + kTrackLeftInset, track_y);
    lv_obj_set_size(track, kTrackWidth, kTrackHeight);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 7, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x31445bU), 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    const int32_t fill_width =
        static_cast<int32_t>(StatusSliderPositionPercent(target, clamped_percent)) * kTrackWidth / 100;
    lv_obj_t* fill = lv_obj_create(track);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, fill_width > 0 ? fill_width : 1, kTrackHeight);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 7, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* knob = lv_obj_create(root);
    lv_obj_set_pos(knob, bounds.x + kTrackLeftInset + fill_width - 12, track_y - 7);
    lv_obj_set_size(knob, 28, 28);
    lv_obj_set_style_pad_all(knob, 0, 0);
    lv_obj_set_style_border_width(knob, 3, 0);
    lv_obj_set_style_border_color(knob, lv_color_hex(0xf4f8ffU), 0);
    lv_obj_set_style_radius(knob, 14, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(color), 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    if (index >= 0) {
        state.status_slider_value_labels[index] = value_label;
        state.status_slider_fills[index] = fill;
        state.status_slider_knobs[index] = knob;
        state.status_slider_values[index] = clamped_percent;
        state.status_slider_last_emitted_values[index] = clamped_percent;
    }
}

void DrawStatusMetric(lv_obj_t* root, int32_t x, const char* name, const char* value, uint8_t percent, uint32_t color) {
    const HallCardBounds bounds{
        .x = x - kStatusDialogBounds.x, .y = 390 - kStatusDialogBounds.y, .width = 192, .height = 124};
    lv_obj_t* panel = CreateStatusPanel(root, bounds, 0x142235U);
    (void)CreateHallLabel(panel, name, &lv_font_montserrat_18, 0x91a4bdU, 16, 15);
    (void)CreateHallLabel(panel, value, &lv_font_montserrat_18, 0xf4f8ffU, 16, 47);
    lv_obj_t* track = lv_obj_create(panel);
    lv_obj_set_pos(track, 16, 90);
    lv_obj_set_size(track, 160, 10);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 5, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x31445bU), 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* fill = lv_obj_create(track);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, percent > 0U ? static_cast<int32_t>(percent) * 160 / 100 : 1, 10);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 5, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
}

void ResetStatusObjectPointers(MetalioClaw4PlatformState& state) {
    state.status_dialog = nullptr;
    for (uint32_t index = 0U; index < 3U; ++index) {
        state.status_quick_panels[index] = nullptr;
        state.status_quick_detail_labels[index] = nullptr;
        state.status_quick_press_overlays[index] = nullptr;
    }
    for (uint32_t index = 0U; index < 2U; ++index) {
        state.status_slider_value_labels[index] = nullptr;
        state.status_slider_fills[index] = nullptr;
        state.status_slider_knobs[index] = nullptr;
    }
}

void UpdateStatusQuickCardLocked(MetalioClaw4PlatformState& state, StatusTouchTarget target, const char* detail,
                                 bool active, bool available) {
    const int32_t index = StatusQuickIndex(target);
    if (index < 0 || state.status_quick_panels[index] == nullptr ||
        state.status_quick_detail_labels[index] == nullptr) {
        return;
    }
    const uint32_t color = !available ? 0x182331U : (active ? 0x1b765fU : 0x26384dU);
    lv_obj_set_style_bg_color(state.status_quick_panels[index], lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(state.status_quick_panels[index], available && !active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_label_set_text(state.status_quick_detail_labels[index], detail);
    lv_obj_set_style_text_color(state.status_quick_detail_labels[index],
                                lv_color_hex(active && available ? 0x8ff0cfU : 0x91a4bdU), 0);
}

void UpdateStatusControlsLocked(MetalioClaw4PlatformState& state, const host_ui::StatusLayerModel& model) {
    const char* wifi_detail = !model.wifi_available
                                  ? "UNAVAILABLE"
                                  : (model.wifi_connected ? "CONNECTED" : (model.wifi_enabled ? "ON" : "OFF"));
    const char* cellular_detail =
        !model.cellular_available ? "UNAVAILABLE"
                                  : (model.cellular_connected ? "CONNECTED" : (model.cellular_enabled ? "ON" : "OFF"));
    UpdateStatusQuickCardLocked(state, StatusTouchTarget::kWifi, wifi_detail, model.wifi_enabled, model.wifi_available);
    UpdateStatusQuickCardLocked(state, StatusTouchTarget::kCellular, cellular_detail, model.cellular_enabled,
                                model.cellular_available);
    UpdateStatusQuickCardLocked(state, StatusTouchTarget::kPerformance,
                                model.performance_overlay_enabled ? "FPS + CPU ON" : "FPS + CPU OFF",
                                model.performance_overlay_enabled, true);
    UpdateStatusSliderLocked(state, StatusTouchTarget::kBrightness, model.brightness_percent, false);
    UpdateStatusSliderLocked(state, StatusTouchTarget::kVolume, model.volume_percent, false);
}

void DrawStatusLayerLocked(MetalioClaw4PlatformState& state, const host_ui::StatusLayerModel& model) {
    if (state.status_layer == nullptr) {
        state.status_layer = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.status_layer, 0x06101cU);
    }
    ResetStatusObjectPointers(state);
    lv_obj_clean(state.status_layer);
    lv_obj_remove_flag(state.status_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(state.status_layer, lv_color_hex(0x02060cU), 0);
    lv_obj_set_style_bg_opa(state.status_layer, 190, 0);

    state.status_dialog = CreateStatusPanel(state.status_layer, kStatusDialogBounds, 0x0d1b2cU);
    lv_obj_set_style_radius(state.status_dialog, 30, 0);
    lv_obj_set_style_border_width(state.status_dialog, 2, 0);
    lv_obj_set_style_border_color(state.status_dialog, lv_color_hex(0x496786U), 0);

    const char* wifi_detail = !model.wifi_available
                                  ? "UNAVAILABLE"
                                  : (model.wifi_connected ? "CONNECTED" : (model.wifi_enabled ? "ON" : "OFF"));
    const char* cellular_detail =
        !model.cellular_available ? "UNAVAILABLE"
                                  : (model.cellular_connected ? "CONNECTED" : (model.cellular_enabled ? "ON" : "OFF"));
    DrawStatusQuickCard(state, state.status_dialog, StatusTouchTarget::kWifi, "WIFI", wifi_detail, model.wifi_enabled,
                        model.wifi_available);
    DrawStatusQuickCard(state, state.status_dialog, StatusTouchTarget::kCellular, "4G", cellular_detail,
                        model.cellular_enabled, model.cellular_available);
    DrawStatusQuickCard(state, state.status_dialog, StatusTouchTarget::kPerformance, "FPS",
                        model.performance_overlay_enabled ? "FPS + CPU ON" : "FPS + CPU OFF",
                        model.performance_overlay_enabled, true);

    DrawStatusSlider(state, state.status_dialog, StatusTouchTarget::kBrightness, "BRIGHTNESS", model.brightness_percent,
                     kStatusControlColor);
    DrawStatusSlider(state, state.status_dialog, StatusTouchTarget::kVolume, "VOLUME", model.volume_percent,
                     kStatusControlColor);

    char memory[24]{};
    char storage[24]{};
    char battery[16]{};
    (void)std::snprintf(memory, sizeof(memory), "%" PRIu32 " / %" PRIu32 " MB", model.memory_used_kib / 1024U,
                        model.memory_total_kib / 1024U);
    const uint32_t storage_used_tenths = (model.storage_used_kib * 10U + 512U) / 1024U;
    (void)std::snprintf(storage, sizeof(storage), "%" PRIu32 ".%" PRIu32 " / %" PRIu32 " MB", storage_used_tenths / 10U,
                        storage_used_tenths % 10U, model.storage_total_kib / 1024U);
    if (model.battery_available) {
        (void)std::snprintf(battery, sizeof(battery), "%u%%", static_cast<unsigned>(model.battery_percent));
    } else {
        (void)std::snprintf(battery, sizeof(battery), "UNAVAILABLE");
    }
    const uint8_t memory_percent =
        model.memory_total_kib > 0U ? static_cast<uint8_t>(model.memory_used_kib * 100U / model.memory_total_kib) : 0U;
    const uint8_t storage_percent = model.storage_total_kib > 0U
                                        ? static_cast<uint8_t>(model.storage_used_kib * 100U / model.storage_total_kib)
                                        : 0U;
    DrawStatusMetric(state.status_dialog, 48, "MEMORY", memory, memory_percent, kStatusControlColor);
    DrawStatusMetric(state.status_dialog, 264, "STORAGE", storage, storage_percent, kStatusControlColor);
    DrawStatusMetric(state.status_dialog, 480, "BATTERY", battery, model.battery_available ? model.battery_percent : 0U,
                     kStatusControlColor);

    lv_obj_move_foreground(state.status_layer);
    if (state.performance_overlay != nullptr && !lv_obj_has_flag(state.performance_overlay, LV_OBJ_FLAG_HIDDEN)) {
        // The Host performance HUD is a final-display overlay. Keep it above
        // the status scrim as well; the dialog begins below its bottom edge.
        lv_obj_move_foreground(state.performance_overlay);
    }
    lv_timer_ready(lv_display_get_refr_timer(state.display));
}

std::expected<void, host_ui::SystemUiError> ShowStatusLayerImpl(MetalioClaw4PlatformState& state,
                                                                const host_ui::StatusLayerModel& model,
                                                                host_ui::SystemUiActionSink action_sink,
                                                                void* action_context) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    DrawStatusLayerLocked(state, model);
    esp_lv_adapter_unlock();
    state.status_action_sink = action_sink;
    state.status_action_context = action_context;
    state.status_touch_active = false;
    state.status_button_pressed = false;
    state.status_touch_target = StatusTouchTarget::kNone;
    state.input_router.BindTouchSink(StatusTouchSink, &state);
    state.input_router.BindSystemActionSink(action_sink, action_context);
    ESP_LOGI(kTag, "status layer visible");
    return {};
}

void UpdateStatusLayerImpl(MetalioClaw4PlatformState& state, const host_ui::StatusLayerModel& model) {
    if (state.display == nullptr || state.status_layer == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    UpdateStatusControlsLocked(state, model);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();
}

void LeaveStatusLayerImpl(MetalioClaw4PlatformState& state) {
    state.input_router.UnbindTouchSink(&state);
    state.input_router.ClearSystemActionSink(state.status_action_context);
    state.status_action_sink = nullptr;
    state.status_action_context = nullptr;
    state.status_touch_active = false;
    state.status_button_pressed = false;
    state.status_touch_target = StatusTouchTarget::kNone;
    if (state.display == nullptr || state.status_layer == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    lv_obj_clean(state.status_layer);
    ResetStatusObjectPointers(state);
    lv_obj_add_flag(state.status_layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();
    ESP_LOGI(kTag, "status layer hidden");
}

void RaisePerformanceOverlayLocked(MetalioClaw4PlatformState& state) {
    if (state.performance_overlay != nullptr && !lv_obj_has_flag(state.performance_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(state.performance_overlay);
    }
}

bool PerformanceOverlayVisibleLocked(const MetalioClaw4PlatformState& state) {
    return state.performance_overlay != nullptr && !lv_obj_has_flag(state.performance_overlay, LV_OBJ_FLAG_HIDDEN);
}

void UpdatePerformanceOverlayImpl(MetalioClaw4PlatformState& state, bool enabled, uint8_t cpu_percent) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (!enabled) {
        if (state.performance_overlay != nullptr) {
            lv_obj_add_flag(state.performance_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_timer_ready(lv_display_get_refr_timer(state.display));
        }
        state.performance_last_frame_sequence = state.display_refresh_sequence;
        state.performance_last_sample_us = 0;
        state.performance_fps = 0U;
        esp_lv_adapter_unlock();
        return;
    }

    const bool create_overlay = state.performance_overlay == nullptr;
    if (create_overlay) {
        state.performance_overlay = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(state.performance_overlay, 566, 4);
        lv_obj_set_size(state.performance_overlay, 150, 30);
        lv_obj_set_style_pad_all(state.performance_overlay, 0, 0);
        lv_obj_set_style_radius(state.performance_overlay, 6, 0);
        lv_obj_set_style_border_width(state.performance_overlay, 0, 0);
        lv_obj_set_style_bg_color(state.performance_overlay, lv_color_hex(0x07111eU), 0);
        // This HUD is only 150x30 pixels, so its requested translucent backing
        // remains a small, PPA-eligible blend instead of a full-screen cost.
        lv_obj_set_style_bg_opa(state.performance_overlay, 176, 0);
        lv_obj_remove_flag(state.performance_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(state.performance_overlay, LV_OBJ_FLAG_CLICKABLE);
        state.performance_cpu_label =
            CreateHallLabel(state.performance_overlay, "", &lv_font_montserrat_14, 0xf4f8ffU, 7, 7);
        state.performance_fps_label =
            CreateHallLabel(state.performance_overlay, "", &lv_font_montserrat_14, 0xf4f8ffU, 76, 7);
        lv_obj_set_width(state.performance_fps_label, 67);
        lv_obj_set_style_text_align(state.performance_fps_label, LV_TEXT_ALIGN_RIGHT, 0);
    }
    lv_obj_remove_flag(state.performance_overlay, LV_OBJ_FLAG_HIDDEN);

    const int64_t now_us = esp_timer_get_time();
    if (state.performance_last_sample_us != 0 && now_us > state.performance_last_sample_us) {
        const uint32_t frames = state.display_refresh_sequence - state.performance_last_frame_sequence;
        const uint64_t elapsed_us = static_cast<uint64_t>(now_us - state.performance_last_sample_us);
        state.performance_fps =
            static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000000U + elapsed_us / 2U) / elapsed_us);
    }
    state.performance_last_frame_sequence = state.display_refresh_sequence;
    state.performance_last_sample_us = now_us;
    char cpu_text[16]{};
    char fps_text[16]{};
    (void)std::snprintf(cpu_text, sizeof(cpu_text), "CPU %u%%", static_cast<unsigned>(cpu_percent));
    (void)std::snprintf(fps_text, sizeof(fps_text), "FPS %" PRIu32, state.performance_fps);
    lv_label_set_text(state.performance_cpu_label, cpu_text);
    lv_label_set_text(state.performance_fps_label, fps_text);
    // Hall, Guest and the status layer are sibling roots. Any one of them may
    // have moved itself to the foreground since the previous sample, so the
    // enabled Host HUD must reclaim the final-display layer on every update.
    RaisePerformanceOverlayLocked(state);
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    esp_lv_adapter_unlock();
}

// Caller owns the LVGL adapter mutex, so all retained-object mutations remain
// serialized with LVGL refreshes.
int32_t ApplyGraphicsFrameLocked(MetalioClaw4PlatformState& state, const uint8_t* bytes, uint32_t length,
                                 const device::TextureAccess& textures,
                                 const micropixel_texture_handle_t* retained_textures, uint32_t retained_count) {
    if (!state.retained_scene.Initialize()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    const bool created_guest_frame = state.guest_frame == nullptr;
    if (state.guest_frame == nullptr) {
        state.guest_frame = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.guest_frame, 0x000000U);
    }
    const metalio_claw4::RetainedFrameResult result =
        state.retained_scene.Execute(bytes, length, state.guest_frame, textures.resolve, textures.context);
    if (result.status != MICROPIXEL_STATUS_OK) {
        ESP_LOGE(kTag, "retained-object command execution failed");
        return result.status;
    }
    bool needs_present = created_guest_frame || result.visual_changed;
    /* A successful direct-compositor flush is already the physical
     * presentation for this Guest frame. Do not wake LVGL for a redundant
     * private-buffer refresh while dummy draw owns the panel framebuffers.
     * Object mutations continue accumulating and are fully redrawn when the
     * translation ends. */
    if (result.surface_active) {
        needs_present = false;
    }
    // Do not bounce the Guest and Host HUD past each other in z-order on every
    // frame. Each move invalidates their overlapping areas and used to make
    // the main task wait behind continuous LVGL redraws, delaying system
    // gestures for tens of seconds. The HUD is raised once when enabled (or
    // when a new Guest tree is first created) and then both siblings stay put.
    const bool overlay_visible = PerformanceOverlayVisibleLocked(state);
    if (overlay_visible) {
        if (created_guest_frame) {
            RaisePerformanceOverlayLocked(state);
        }
    } else if (created_guest_frame || state.host_smoke != nullptr) {
        lv_obj_move_foreground(state.guest_frame);
    }
    if (DismissHostSmokeLocked(state)) {
        needs_present = true;
    }
    if (!needs_present) {
        ReleaseTextures(state.scene_texture_access, state.scene_textures, state.scene_texture_count);
        std::memcpy(state.scene_textures, retained_textures, retained_count * sizeof(retained_textures[0]));
        state.scene_texture_count = retained_count;
        state.scene_texture_access = retained_count == 0U ? device::TextureAccess{} : textures;
        return MICROPIXEL_STATUS_OK;
    }
    lv_timer_ready(lv_display_get_refr_timer(state.display));
    ReleaseTextures(state.scene_texture_access, state.scene_textures, state.scene_texture_count);
    std::memcpy(state.scene_textures, retained_textures, retained_count * sizeof(retained_textures[0]));
    state.scene_texture_count = retained_count;
    state.scene_texture_access = retained_count == 0U ? device::TextureAccess{} : textures;
    return MICROPIXEL_STATUS_OK;
}

void ReleaseGuestGraphicsImpl(MetalioClaw4PlatformState& state) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (state.guest_frame != nullptr) {
        state.retained_scene.ForgetObjects();
        lv_obj_delete(state.guest_frame);
        state.guest_frame = nullptr;
        lv_timer_ready(lv_display_get_refr_timer(state.display));
        ESP_LOGI(kTag, "Guest graphics tree released before Bitmap teardown");
    }
    ReleaseTextures(state.scene_texture_access, state.scene_textures, state.scene_texture_count);
    state.scene_texture_count = 0U;
    state.scene_texture_access = {};
    ClearPendingFrameTextures(state, true);
    state.bitmap_update_frame_active = false;
    state.bitmap_damage_count = 0U;
    state.bitmap_frame_updates = 0U;
    state.bitmap_frame_bytes = 0U;
    state.graphics_frame_active = false;
    state.graphics_frame_length = 0U;
    state.graphics_frame_commands = 0U;
    heap_caps_free(state.graphics_frame_bytes);
    state.graphics_frame_bytes = nullptr;
    state.retained_scene.Release();
    esp_lv_adapter_unlock();
}

int32_t GraphicsBeginFrameImpl(MetalioClaw4PlatformState& state) {
    if (state.display == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (state.graphics_frame_active || state.bitmap_update_frame_active) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (state.graphics_frame_bytes == nullptr) {
        state.graphics_frame_bytes = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            4U, MICROPIXEL_GRAPHICS_MAX_FRAME_COMMAND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (state.graphics_frame_bytes == nullptr) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
    }
    std::memset(state.graphics_frame_bytes, 0, sizeof(micropixel_graphics_command_header_t));
    state.graphics_frame_length = sizeof(micropixel_graphics_command_header_t);
    state.graphics_frame_commands = 0U;
    ClearPendingFrameTextures(state, true);
    state.graphics_frame_active = true;
    return MICROPIXEL_STATUS_OK;
}

int32_t GraphicsSubmitImpl(MetalioClaw4PlatformState& state, const uint8_t* bytes, uint32_t length,
                           const device::TextureAccess& textures) {
    if (state.display == nullptr || bytes == nullptr || textures.resolve == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }

    const bool frame_active = state.graphics_frame_active;
    const bool lvgl_locked = !frame_active;
    if (lvgl_locked) {
        if (esp_lv_adapter_lock(-1) != ESP_OK) {
            return MICROPIXEL_STATUS_INTERNAL;
        }
    }
    const int32_t validation_status =
        graphics::ValidateCommandStream(bytes, length, kWidth, kHeight, textures.resolve, textures.context);
    if (validation_status != MICROPIXEL_STATUS_OK) {
        if (lvgl_locked) {
            esp_lv_adapter_unlock();
        }
        ESP_LOGW(kTag, "rejected graphics batch: status=%" PRId32 " bytes=%" PRIu32, validation_status, length);
        return validation_status;
    }
    micropixel_texture_handle_t frame_textures[kMaxSceneTextures]{};
    uint32_t frame_texture_count = 0U;
    if (!graphics::CollectTextureHandles(bytes, length, frame_textures, kMaxSceneTextures, frame_texture_count)) {
        if (lvgl_locked) {
            esp_lv_adapter_unlock();
        }
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (state.graphics_frame_active) {
        micropixel_graphics_command_header_t batch_header{};
        std::memcpy(&batch_header, bytes, sizeof(batch_header));
        const uint32_t record_bytes = length - sizeof(batch_header);
        if (state.graphics_frame_bytes == nullptr ||
            batch_header.command_count > MICROPIXEL_GRAPHICS_MAX_FRAME_COMMANDS - state.graphics_frame_commands ||
            record_bytes > MICROPIXEL_GRAPHICS_MAX_FRAME_COMMAND_BYTES - state.graphics_frame_length) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        if (!AddPendingFrameTextures(state, frame_textures, frame_texture_count, textures)) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        std::memcpy(state.graphics_frame_bytes + state.graphics_frame_length, bytes + sizeof(batch_header),
                    record_bytes);
        state.graphics_frame_length += record_bytes;
        state.graphics_frame_commands += batch_header.command_count;
        return MICROPIXEL_STATUS_OK;
    }
    if (!RetainTextures(textures, frame_textures, frame_texture_count)) {
        esp_lv_adapter_unlock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    const int32_t execution_status =
        ApplyGraphicsFrameLocked(state, bytes, length, textures, frame_textures, frame_texture_count);
    if (execution_status != MICROPIXEL_STATUS_OK) {
        ReleaseTextures(textures, frame_textures, frame_texture_count);
    }
    if (lvgl_locked) {
        esp_lv_adapter_unlock();
    }
    if (execution_status != MICROPIXEL_STATUS_OK) {
        return execution_status;
    }

    return MICROPIXEL_STATUS_OK;
}

int32_t GraphicsCommitFrameImpl(MetalioClaw4PlatformState& state, const device::TextureAccess& textures) {
    if (state.display == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (!state.graphics_frame_active || state.graphics_frame_bytes == nullptr || state.graphics_frame_commands == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    micropixel_graphics_command_header_t header{};
    header.magic = MICROPIXEL_GRAPHICS_COMMAND_MAGIC;
    header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    header.total_size = state.graphics_frame_length;
    header.command_count = state.graphics_frame_commands;
    std::memcpy(state.graphics_frame_bytes, &header, sizeof(header));

    const uint32_t frame_commands = state.graphics_frame_commands;
    const uint32_t frame_bytes = state.graphics_frame_length;
    state.graphics_frame_active = false;
    state.graphics_frame_length = 0U;
    state.graphics_frame_commands = 0U;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ClearPendingFrameTextures(state, true);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    const device::TextureAccess retained_access =
        state.graphics_frame_texture_count == 0U ? textures : state.graphics_frame_texture_access;
    const int32_t status = ApplyGraphicsFrameLocked(state, state.graphics_frame_bytes, frame_bytes, retained_access,
                                                    state.graphics_frame_textures, state.graphics_frame_texture_count);
    const uint32_t sequence = status == MICROPIXEL_STATUS_OK ? ++state.graphics_frame_sequence : 0U;
    if (status == MICROPIXEL_STATUS_OK) {
        ++state.presented_guest_frame_sequence;
        ClearPendingFrameTextures(state, false);
    } else {
        ClearPendingFrameTextures(state, true);
    }
    esp_lv_adapter_unlock();
    if (status == MICROPIXEL_STATUS_OK && (sequence <= 8U || (sequence % 120U) == 0U)) {
        ESP_LOGI(kTag, "graphics frame #%" PRIu32 ": commands=%" PRIu32 " bytes=%" PRIu32, sequence, frame_commands,
                 frame_bytes);
    }
    return status;
}

int32_t GraphicsCancelFrameImpl(MetalioClaw4PlatformState& state) {
    if (!state.graphics_frame_active) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    state.graphics_frame_active = false;
    state.graphics_frame_length = 0U;
    state.graphics_frame_commands = 0U;
    ClearPendingFrameTextures(state, true);
    return MICROPIXEL_STATUS_OK;
}

int32_t GraphicsBeginBitmapUpdateFrameImpl(MetalioClaw4PlatformState& state) {
    if (state.display == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (state.bitmap_update_frame_active || state.graphics_frame_active) {
        esp_lv_adapter_unlock();
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    state.bitmap_update_frame_active = true;
    state.bitmap_damage_count = 0U;
    state.bitmap_frame_updates = 0U;
    state.bitmap_frame_bytes = 0U;
    state.bitmap_frame_started_us = static_cast<uint64_t>(esp_timer_get_time());
    esp_lv_adapter_unlock();
    return MICROPIXEL_STATUS_OK;
}

int32_t GraphicsUpdateBitmapImpl(MetalioClaw4PlatformState& state, const device::BitmapView& bitmap, uint32_t x,
                                 uint32_t y, uint32_t width, uint32_t height, const uint8_t* pixels, uint32_t stride) {
    const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
                                         ? 3U
                                         : (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 0U);
    if (state.display == nullptr || bitmap.data == nullptr || pixels == nullptr || bytes_per_pixel == 0U ||
        (bitmap.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) == 0U || width == 0U || height == 0U ||
        static_cast<uint64_t>(x) + width > bitmap.width || static_cast<uint64_t>(y) + height > bitmap.height ||
        stride != width * bytes_per_pixel || bitmap.stride != bitmap.width * bytes_per_pixel ||
        bitmap.size != bitmap.stride * bitmap.height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    auto* destination = const_cast<uint8_t*>(bitmap.data) + y * bitmap.stride + x * bytes_per_pixel;
    for (uint32_t row = 0U; row < height; ++row) {
        std::memcpy(destination + row * bitmap.stride, pixels + row * stride, stride);
    }
    if (state.bitmap_update_frame_active) {
        if (!AccumulateDamage(state.bitmap_damage, state.bitmap_damage_count, bitmap.data, x, y, width, height)) {
            esp_lv_adapter_unlock();
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        ++state.bitmap_frame_updates;
        state.bitmap_frame_bytes += static_cast<uint64_t>(stride) * height;
    } else {
        const bool invalidated = state.retained_scene.InvalidateBitmap(bitmap.data, x, y, width, height);
        if (invalidated) {
            lv_timer_ready(lv_display_get_refr_timer(state.display));
        }
    }
    esp_lv_adapter_unlock();
    return MICROPIXEL_STATUS_OK;
}

int32_t GraphicsCommitBitmapUpdateFrameImpl(MetalioClaw4PlatformState& state) {
    if (state.display == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (!state.bitmap_update_frame_active) {
        esp_lv_adapter_unlock();
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    bool invalidated = false;
    uint32_t ppa_eligible_damage = 0U;
    for (uint32_t index = 0U; index < state.bitmap_damage_count; ++index) {
        const BitmapDamage& damage = state.bitmap_damage[index];
        invalidated =
            state.retained_scene.InvalidateBitmap(damage.data, damage.x, damage.y, damage.width, damage.height) ||
            invalidated;
        if (static_cast<uint64_t>(damage.width) * damage.height > CONFIG_MICROPIXEL_LVGL_PPA_MIN_AREA_PIXELS) {
            ++ppa_eligible_damage;
        }
    }
    if (invalidated) {
        lv_timer_ready(lv_display_get_refr_timer(state.display));
    }

    const uint32_t updates = state.bitmap_frame_updates;
    const uint64_t bytes = state.bitmap_frame_bytes;
    const uint32_t damage_count = state.bitmap_damage_count;
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t elapsed_us = now_us >= state.bitmap_frame_started_us ? now_us - state.bitmap_frame_started_us : 0U;
    const uint32_t sequence = updates == 0U ? state.bitmap_frame_sequence : ++state.bitmap_frame_sequence;
    if (updates != 0U && invalidated) {
        ++state.presented_guest_frame_sequence;
    }
    state.bitmap_update_frame_active = false;
    state.bitmap_damage_count = 0U;
    state.bitmap_frame_updates = 0U;
    state.bitmap_frame_bytes = 0U;
    esp_lv_adapter_unlock();

    if (updates != 0U && (sequence <= 8U || (sequence % 120U) == 0U)) {
        ESP_LOGI(kTag,
                 "offscreen frame #%" PRIu32 ": updates=%" PRIu32 " bytes=%" PRIu64 " unions=%" PRIu32
                 " ppa-eligible=%" PRIu32 " stage=%" PRIu64 " us present=%s",
                 sequence, updates, bytes, damage_count, ppa_eligible_damage, elapsed_us, invalidated ? "yes" : "no");
    }
    return MICROPIXEL_STATUS_OK;
}

metalio_claw4::GraphicsOperations MakeGraphicsOperations(MetalioClaw4PlatformState& state) {
    return {
        .context = &state,
        .available = [](void*) { return GraphicsBackendAvailableImpl(); },
        .get_info =
            [](void* context, micropixel_graphics_info_t& info) {
                return GraphicsGetInfoImpl(*static_cast<MetalioClaw4PlatformState*>(context), info);
            },
        .begin_frame =
            [](void* context) { return GraphicsBeginFrameImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .submit =
            [](void* context, const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures) {
                return GraphicsSubmitImpl(*static_cast<MetalioClaw4PlatformState*>(context), bytes, length, textures);
            },
        .commit_frame =
            [](void* context, const device::TextureAccess& textures) {
                return GraphicsCommitFrameImpl(*static_cast<MetalioClaw4PlatformState*>(context), textures);
            },
        .cancel_frame =
            [](void* context) { return GraphicsCancelFrameImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .begin_bitmap_update_frame =
            [](void* context) {
                return GraphicsBeginBitmapUpdateFrameImpl(*static_cast<MetalioClaw4PlatformState*>(context));
            },
        .update_bitmap =
            [](void* context, const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               const uint8_t* pixels, uint32_t stride) {
                return GraphicsUpdateBitmapImpl(*static_cast<MetalioClaw4PlatformState*>(context), bitmap, x, y, width,
                                                height, pixels, stride);
            },
        .commit_bitmap_update_frame =
            [](void* context) {
                return GraphicsCommitBitmapUpdateFrameImpl(*static_cast<MetalioClaw4PlatformState*>(context));
            },
        .show_launch_bitmap =
            [](void* context, const device::BitmapView& bitmap) {
                return ShowLaunchBitmapImpl(*static_cast<MetalioClaw4PlatformState*>(context), bitmap);
            },
        .dismiss_launch_bitmap =
            [](void* context) { DismissLaunchBitmapImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
        .release_guest_resources =
            [](void* context) { ReleaseGuestGraphicsImpl(*static_cast<MetalioClaw4PlatformState*>(context)); },
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
                const uint8_t safe_percent = percent < host_ui::kMinimumBrightnessPercent
                                                 ? host_ui::kMinimumBrightnessPercent
                                                 : (percent <= 100U ? percent : 100U);
                const esp_err_t status =
                    static_cast<MetalioClaw4PlatformState*>(context)->hardware.SetBacklightBrightness(safe_percent);
                if (status != ESP_OK) {
                    ESP_LOGW(kTag, "could not set backlight brightness: %s", esp_err_to_name(status));
                }
            },
        .apply_volume = [](void*, uint8_t percent) { ConfiguredAudioBackend().SetMasterVolumePercent(percent); },
    };
}

class MetalioClaw4Platform final : public Platform {
   public:
    MetalioClaw4Platform() : graphics_(MakeGraphicsOperations(state_)), system_ui_(MakeSystemUiOperations(state_)) {}

    [[nodiscard]] esp_err_t Initialize() override { return InitializePlatformImpl(state_); }
    [[nodiscard]] device::GraphicsBackend& graphics() override { return graphics_; }
    [[nodiscard]] device::InputBackend& input() override { return state_.input_router; }
    [[nodiscard]] device::AudioBackend& audio() override { return ConfiguredAudioBackend(); }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }
    [[nodiscard]] host_ui::SystemUiBackend& system_ui() override { return system_ui_; }

   private:
    MetalioClaw4PlatformState state_{};
    metalio_claw4::GraphicsAdapter graphics_;
    metalio_claw4::SystemUiAdapter system_ui_;
};

}  // namespace

Platform& ConfiguredPlatform() {
    static MetalioClaw4Platform platform;
    return platform;
}

}  // namespace micropixel::platform
