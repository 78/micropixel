#include <cinttypes>
#include <cstdint>
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
#include "lvgl.h"
#include "platform/audio_backend.hpp"
#include "platform/configured_backends.hpp"
#include "platform/metalio-claw4/board_hardware.hpp"
#include "platform/metalio-claw4/display/dirty_region_coalescer.hpp"
#include "platform/metalio-claw4/display/retained_scene.hpp"
#if CONFIG_MICROPIXEL_ENABLE_SCREEN_CAPTURE
#include "platform/metalio-claw4/display/screen_capture.hpp"
#endif
#include "platform/graphics/command_stream.hpp"
#include "platform/metalio-claw4/input/gt911_input.hpp"
#include "platform/metalio-claw4/input/tca9555_power_key.hpp"
#include "platform/platform.hpp"

namespace micropixel::platform {
namespace {

constexpr char kTag[] = "micropixel_platform";
constexpr int kWidth = 720;
constexpr int kHeight = 720;
constexpr BaseType_t kLvglTaskCore = 1;
constexpr uint32_t kRefreshPeriodMs = 1000;
constexpr uint32_t kBitmapDamageCapacity = 16U;
#if CONFIG_MICROPIXEL_LVGL_PPA_ACCEL
constexpr bool kEnablePpaAccel = true;
#else
constexpr bool kEnablePpaAccel = false;
#endif
constexpr esp_lv_adapter_tear_avoid_mode_t kTearAvoidMode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL;
constexpr const char* kTearAvoidModeName = "double-partial";
constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_33;

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

struct BitmapDamage final {
    const uint8_t* data{};
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

// All board-owned display/input state is stored inside the selected Platform
// object. Callbacks receive this state explicitly instead of reaching through
// file-level aliases.
struct MetalioClaw4PlatformState final {
    metalio_claw4::BoardHardware hardware{kWidth, kHeight};
    lv_display_t* display{};
    lv_obj_t* host_smoke{};
    lv_obj_t* guest_frame{};
    lv_image_dsc_t launch_image_descriptor{};
    metalio_claw4::DirtyRegionCoalescer dirty_region_coalescer{};
    metalio_claw4::RetainedScene retained_scene{kWidth, kHeight};
    metalio_claw4::Tca9555PowerKey power_key{};
    metalio_claw4::Gt911Input touch_input{kWidth, kHeight, kTouchInterrupt};
    BitmapDamage bitmap_damage[kBitmapDamageCapacity]{};
    uint64_t bitmap_frame_started_us{};
    uint64_t bitmap_frame_bytes{};
    uint32_t bitmap_frame_updates{};
    uint32_t bitmap_frame_sequence{};
    uint32_t bitmap_damage_count{};
    uint8_t* graphics_frame_bytes{};
    uint32_t graphics_frame_length{};
    uint32_t graphics_frame_commands{};
    uint32_t graphics_frame_sequence{};
    uint32_t display_refresh_sequence{};
    int64_t display_refresh_started_us{};
    bool bitmap_update_frame_active{};
    bool graphics_frame_active{};
};

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
    if (state != nullptr && state->display_refresh_started_us != 0) {
        const uint32_t sequence = ++state->display_refresh_sequence;
        const uint32_t duration_us =
            static_cast<uint32_t>(esp_timer_get_time() - state->display_refresh_started_us);
        state->display_refresh_started_us = 0;
        if (sequence <= 4U || (sequence % 60U) == 0U) {
            ESP_LOGI(kTag, "display refresh #%" PRIu32 ": total=%" PRIu32 " us", sequence, duration_us);
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
    esp_lv_adapter_config_t adapter_config{};
    adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_config.stack_in_psram = false;
    adapter_config.task_priority = 3;
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
#if CONFIG_MICROPIXEL_ENABLE_SCREEN_CAPTURE
    if (status == ESP_OK) {
        status = metalio_claw4::InitializeScreenCapture(state.display, state.touch_input, kWidth, kHeight);
    }
#endif
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
             "GT911 IRQ GPIO%d, power-key=TCA9555-P0.5/IRQ-GPIO2, graphics=%s, ppa=%s, lvgl-core=1 priority=3",
             kWidth, kHeight, kTearAvoidModeName, static_cast<unsigned long>(kRefreshPeriodMs),
             static_cast<int>(kTouchInterrupt), GraphicsBackendName(), kEnablePpaAccel ? "on" : "off");
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
    info.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB888;
    info.capabilities = 0U;
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    info.capabilities |= MICROPIXEL_GRAPHICS_CAP_SURFACE_TRANSLATION;
#endif
    info.capabilities |= MICROPIXEL_GRAPHICS_CAP_MULTI_SUBMIT_FRAME;
    info.max_command_bytes = MICROPIXEL_GRAPHICS_MAX_COMMAND_BYTES;
    info.max_commands = MICROPIXEL_GRAPHICS_MAX_COMMANDS;
    info.max_text_bytes = MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES;
    info.max_frame_commands = MICROPIXEL_GRAPHICS_MAX_FRAME_COMMANDS;
    return MICROPIXEL_STATUS_OK;
}

bool DismissHostSmokeLocked(MetalioClaw4PlatformState& state) {
    if (state.host_smoke == nullptr) {
        return false;
    }
    state.touch_input.ClearSmokeUi();
    lv_obj_delete(state.host_smoke);
    state.host_smoke = nullptr;
    state.launch_image_descriptor = {};
    return true;
}

int32_t ShowLaunchBitmapImpl(MetalioClaw4PlatformState& state, const device::BitmapView& bitmap) {
    constexpr uint32_t kBytesPerPixel = 3U;
    if (state.display == nullptr || state.host_smoke == nullptr || bitmap.data == nullptr ||
        !esp_ptr_in_drom(bitmap.data) || bitmap.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB888 || bitmap.width == 0U ||
        bitmap.height == 0U || bitmap.width > static_cast<uint32_t>(kWidth) ||
        bitmap.height > static_cast<uint32_t>(kHeight) || bitmap.stride != bitmap.width * kBytesPerPixel ||
        bitmap.size != bitmap.stride * bitmap.height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    state.touch_input.ClearSmokeUi();
    lv_obj_clean(state.host_smoke);
    const uint32_t launch_background = LaunchBackgroundRgb888(bitmap);
    lv_obj_set_style_bg_color(state.host_smoke, lv_color_hex(launch_background), 0);
    state.launch_image_descriptor = {};
    state.launch_image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    state.launch_image_descriptor.header.cf = LV_COLOR_FORMAT_RGB888;
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
    ESP_LOGI(kTag,
             "Bundle launch bitmap visible: %" PRIu32 "x%" PRIu32 " RGB888 source=flash-mmap:%p bytes=%" PRIu32
             " source-psram=0",
             bitmap.width, bitmap.height, bitmap.data, bitmap.size);
    return MICROPIXEL_STATUS_OK;
}

void DismissLaunchBitmapImpl(MetalioClaw4PlatformState& state) {
    if (state.display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (DismissHostSmokeLocked(state)) {
        lv_timer_ready(lv_display_get_refr_timer(state.display));
    }
    esp_lv_adapter_unlock();
}

// Caller owns the LVGL adapter mutex, so all retained-object mutations remain
// serialized with LVGL refreshes.
int32_t ApplyGraphicsFrameLocked(MetalioClaw4PlatformState& state, const uint8_t* bytes, uint32_t length,
                                 device::BitmapResolver resolver, void* resolver_context) {
    if (!state.retained_scene.Initialize()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    const bool created_guest_frame = state.guest_frame == nullptr;
    if (state.guest_frame == nullptr) {
        state.guest_frame = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(state.guest_frame, 0x000000U);
    }
    const metalio_claw4::RetainedFrameResult result =
        state.retained_scene.Execute(bytes, length, state.guest_frame, resolver, resolver_context);
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
    lv_obj_move_foreground(state.guest_frame);
    if (DismissHostSmokeLocked(state)) {
        needs_present = true;
    }
    if (!needs_present) {
        return MICROPIXEL_STATUS_OK;
    }
    lv_timer_ready(lv_display_get_refr_timer(state.display));
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
    state.graphics_frame_active = true;
    return MICROPIXEL_STATUS_OK;
}

int32_t GraphicsSubmitImpl(MetalioClaw4PlatformState& state, const uint8_t* bytes, uint32_t length,
                           device::BitmapResolver resolver, void* resolver_context) {
    if (state.display == nullptr || bytes == nullptr) {
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
        graphics::ValidateCommandStream(bytes, length, kWidth, kHeight, resolver, resolver_context);
    if (validation_status != MICROPIXEL_STATUS_OK) {
        if (lvgl_locked) {
            esp_lv_adapter_unlock();
        }
        ESP_LOGW(kTag, "rejected graphics batch: status=%" PRId32 " bytes=%" PRIu32, validation_status, length);
        return validation_status;
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
        std::memcpy(state.graphics_frame_bytes + state.graphics_frame_length, bytes + sizeof(batch_header),
                    record_bytes);
        state.graphics_frame_length += record_bytes;
        state.graphics_frame_commands += batch_header.command_count;
        return MICROPIXEL_STATUS_OK;
    }
    const int32_t execution_status = ApplyGraphicsFrameLocked(state, bytes, length, resolver, resolver_context);
    if (lvgl_locked) {
        esp_lv_adapter_unlock();
    }
    if (execution_status != MICROPIXEL_STATUS_OK) {
        return execution_status;
    }

    return MICROPIXEL_STATUS_OK;
}

int32_t GraphicsCommitFrameImpl(MetalioClaw4PlatformState& state, device::BitmapResolver resolver,
                                void* resolver_context) {
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
        return MICROPIXEL_STATUS_INTERNAL;
    }
    const int32_t status =
        ApplyGraphicsFrameLocked(state, state.graphics_frame_bytes, frame_bytes, resolver, resolver_context);
    const uint32_t sequence = status == MICROPIXEL_STATUS_OK ? ++state.graphics_frame_sequence : 0U;
    esp_lv_adapter_unlock();
    if (status == MICROPIXEL_STATUS_OK && (sequence <= 8U || (sequence % 120U) == 0U)) {
        ESP_LOGI(kTag, "graphics frame #%" PRIu32 ": commands=%" PRIu32 " bytes=%" PRIu32,
                 sequence, frame_commands, frame_bytes);
    }
    return status;
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
    const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB888
                                         ? 3U
                                         : (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_ARGB8888 ? 4U : 0U);
    if (state.display == nullptr || bitmap.data == nullptr || pixels == nullptr || bytes_per_pixel == 0U ||
        (bitmap.flags & MICROPIXEL_BITMAP_FLAG_MUTABLE) == 0U || width == 0U || height == 0U ||
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

class MetalioClaw4GraphicsBackend final : public device::GraphicsBackend {
   public:
    explicit MetalioClaw4GraphicsBackend(MetalioClaw4PlatformState& state) : state_(state) {}

    [[nodiscard]] bool Available() const override { return GraphicsBackendAvailableImpl(); }
    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t& info) override {
        return GraphicsGetInfoImpl(state_, info);
    }
    [[nodiscard]] int32_t BeginFrame() override {
        return GraphicsBeginFrameImpl(state_);
    }
    [[nodiscard]] int32_t Submit(const uint8_t* bytes, uint32_t length, device::BitmapResolver resolver,
                                 void* resolver_context) override {
        return GraphicsSubmitImpl(state_, bytes, length, resolver, resolver_context);
    }
    [[nodiscard]] int32_t CommitFrame(device::BitmapResolver resolver, void* resolver_context) override {
        return GraphicsCommitFrameImpl(state_, resolver, resolver_context);
    }
    [[nodiscard]] int32_t BeginBitmapUpdateFrame() override { return GraphicsBeginBitmapUpdateFrameImpl(state_); }
    [[nodiscard]] int32_t UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height, const uint8_t* pixels, uint32_t stride) override {
        return GraphicsUpdateBitmapImpl(state_, bitmap, x, y, width, height, pixels, stride);
    }
    [[nodiscard]] int32_t CommitBitmapUpdateFrame() override { return GraphicsCommitBitmapUpdateFrameImpl(state_); }
    [[nodiscard]] int32_t ShowLaunchBitmap(const device::BitmapView& bitmap) override {
        return ShowLaunchBitmapImpl(state_, bitmap);
    }
    void DismissLaunchBitmap() override { DismissLaunchBitmapImpl(state_); }
    void ReleaseGuestResources() override { ReleaseGuestGraphicsImpl(state_); }

   private:
    MetalioClaw4PlatformState& state_;
};

class MetalioClaw4Platform final : public Platform {
   public:
    MetalioClaw4Platform() : graphics_(state_) {}

    [[nodiscard]] esp_err_t Initialize() override { return InitializePlatformImpl(state_); }
    [[nodiscard]] device::GraphicsBackend& graphics() override { return graphics_; }
    [[nodiscard]] device::InputBackend& input() override { return state_.touch_input; }
    [[nodiscard]] device::AudioBackend& audio() override { return ConfiguredAudioBackend(); }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }

   private:
    MetalioClaw4PlatformState state_{};
    MetalioClaw4GraphicsBackend graphics_;
};

}  // namespace

Platform& ConfiguredPlatform() {
    static MetalioClaw4Platform platform;
    return platform;
}

}  // namespace micropixel::platform
