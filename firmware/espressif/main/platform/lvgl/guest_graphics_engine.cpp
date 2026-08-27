#include "platform/lvgl/guest_graphics_engine.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/graphics/command_stream.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::lvgl {
namespace {

constexpr char kTag[] = "guest_graphics";

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

void StyleFullscreenContainer(lv_obj_t* container, int32_t width, int32_t height, uint32_t background) {
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, width, height);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

GuestGraphicsEngine::GuestGraphicsEngine(int32_t width, int32_t height, FontRegistry& fonts)
    : width_(width), height_(height), fonts_(fonts), retained_scene_(width, height, fonts) {}

bool GuestGraphicsEngine::ValidateFontHandle(void* context, micropixel_font_handle_t font) {
    return context != nullptr && static_cast<GuestGraphicsEngine*>(context)->fonts_.ResolveGuestHandle(font) != nullptr;
}

bool GuestGraphicsEngine::AccumulateDamage(BitmapDamage* damages, uint32_t capacity, uint32_t& damage_count,
                                           const uint8_t* data, uint32_t x, uint32_t y, uint32_t width,
                                           uint32_t height) {
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
    if (damage_count >= capacity) {
        return false;
    }
    damages[damage_count++] = BitmapDamage{data, x, y, width, height};
    return true;
}

esp_err_t GuestGraphicsEngine::Initialize(lv_display_t* display, esp_lcd_panel_handle_t panel) {
    if (display == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    display_refresh_ready_ = xSemaphoreCreateBinaryStatic(&display_refresh_ready_storage_);
    if (display_refresh_ready_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    display_ = display;
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    retained_scene_.BindSurface(display_, panel);
#else
    (void)panel;
#endif
    if (!retained_scene_.Initialize()) {
        display_ = nullptr;
        display_refresh_ready_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    lv_display_add_event_cb(display_, DisplayRefreshStartEvent, LV_EVENT_REFR_START, this);
    lv_display_add_event_cb(display_, DisplayRefreshReadyEvent, LV_EVENT_REFR_READY, this);
    return ESP_OK;
}

void GuestGraphicsEngine::RebindPanel(esp_lcd_panel_handle_t panel) {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
    retained_scene_.BindSurface(display_, panel);
#else
    (void)panel;
#endif
}

void GuestGraphicsEngine::DisplayRefreshStartEvent(lv_event_t* event) {
    auto* engine = static_cast<GuestGraphicsEngine*>(lv_event_get_user_data(event));
    if (engine == nullptr) {
        return;
    }
    engine->dirty_region_coalescer_.Coalesce(engine->display_);
    engine->display_refresh_started_us_ = esp_timer_get_time();
}

void GuestGraphicsEngine::DisplayRefreshReadyEvent(lv_event_t* event) {
    auto* engine = static_cast<GuestGraphicsEngine*>(lv_event_get_user_data(event));
    if (engine == nullptr) {
        return;
    }
    if (engine->display_refresh_started_us_ != 0) {
        const uint32_t sequence = ++engine->display_refresh_sequence_;
        const uint32_t duration_us = static_cast<uint32_t>(esp_timer_get_time() - engine->display_refresh_started_us_);
        engine->display_refresh_started_us_ = 0;
        if (sequence <= 4U || (sequence % 60U) == 0U) {
            ESP_LOGI(kTag, "display refresh #%" PRIu32 ": total=%" PRIu32 " us", sequence, duration_us);
        }
    }
    if (engine->display_refresh_ready_ != nullptr) {
        (void)xSemaphoreGive(engine->display_refresh_ready_);
    }
}

void GuestGraphicsEngine::DrainRefreshReady() {
    if (display_refresh_ready_ == nullptr) {
        return;
    }
    while (xSemaphoreTake(display_refresh_ready_, 0U) == pdTRUE) {
    }
}

void GuestGraphicsEngine::WaitForRefreshReady() {
    if (display_refresh_ready_ != nullptr) {
        (void)xSemaphoreTake(display_refresh_ready_, portMAX_DELAY);
    }
}

int32_t GuestGraphicsEngine::GetInfo(micropixel_graphics_info_t& info) const {
    if (display_ == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    info = {};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    info.width = width_;
    info.height = height_;
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

bool GuestGraphicsEngine::EnsureTextureStorage() {
    if (texture_storage_ != nullptr) {
        return true;
    }
    constexpr uint32_t kTextureArrayCount = 3U;
    texture_storage_ = static_cast<micropixel_texture_handle_t*>(
        heap_caps_aligned_calloc(alignof(micropixel_texture_handle_t), kTextureArrayCount * kMaxSceneTextures,
                                 sizeof(micropixel_texture_handle_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (texture_storage_ == nullptr) {
        return false;
    }
    graphics_frame_textures_ = texture_storage_;
    scene_textures_ = texture_storage_ + kMaxSceneTextures;
    scratch_textures_ = texture_storage_ + 2U * kMaxSceneTextures;
    return true;
}

void GuestGraphicsEngine::ClearPendingFrameTextures(bool release) {
    if (release) {
        ReleaseTextures(graphics_frame_texture_access_, graphics_frame_textures_, graphics_frame_texture_count_);
    }
    graphics_frame_texture_count_ = 0U;
    graphics_frame_texture_access_ = {};
}

bool GuestGraphicsEngine::AddPendingFrameTextures(const micropixel_texture_handle_t* textures, uint32_t count,
                                                  const device::TextureAccess& access) {
    if (graphics_frame_texture_count_ != 0U && !SameTextureAccess(graphics_frame_texture_access_, access)) {
        return false;
    }
    const uint32_t original_count = graphics_frame_texture_count_;
    for (uint32_t index = 0U; index < count; ++index) {
        bool exists = false;
        for (uint32_t current = 0U; current < graphics_frame_texture_count_; ++current) {
            if (graphics_frame_textures_[current] == textures[index]) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        if (graphics_frame_texture_count_ >= kMaxSceneTextures || access.retain == nullptr ||
            !access.retain(access.context, textures[index])) {
            ReleaseTextures(access, graphics_frame_textures_ + original_count,
                            graphics_frame_texture_count_ - original_count);
            graphics_frame_texture_count_ = original_count;
            return false;
        }
        graphics_frame_textures_[graphics_frame_texture_count_++] = textures[index];
    }
    if (original_count == 0U) {
        graphics_frame_texture_access_ = access;
    }
    return true;
}

int32_t GuestGraphicsEngine::ApplyFrameLocked(const uint8_t* bytes, uint32_t length,
                                              const device::TextureAccess& textures,
                                              const micropixel_texture_handle_t* retained_textures,
                                              uint32_t retained_count) {
    if (!retained_scene_.Initialize()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    const bool created_guest_frame = guest_frame_ == nullptr;
    if (guest_frame_ == nullptr) {
        guest_frame_ = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(guest_frame_, width_, height_, 0x000000U);
    }
    const RetainedFrameResult result =
        retained_scene_.Execute(bytes, length, guest_frame_, textures.resolve, textures.context);
    if (result.status != MICROPIXEL_STATUS_OK) {
        ESP_LOGE(kTag, "retained-object command execution failed");
        return result.status;
    }
    bool needs_present = created_guest_frame || result.visual_changed;
    // Direct-compositor output is already physically visible. LVGL only needs
    // another refresh after the translated surface becomes inactive.
    if (result.surface_active) {
        needs_present = false;
    }

    if (presentation_hooks_.prepare_frame_locked != nullptr) {
        presentation_hooks_.prepare_frame_locked(presentation_hooks_.context, guest_frame_, created_guest_frame,
                                                 needs_present);
    }

    ReleaseTextures(scene_texture_access_, scene_textures_, scene_texture_count_);
    std::memcpy(scene_textures_, retained_textures, retained_count * sizeof(retained_textures[0]));
    scene_texture_count_ = retained_count;
    scene_texture_access_ = retained_count == 0U ? device::TextureAccess{} : textures;
    if (needs_present) {
        RequestDisplayRefresh(display_);
    }
    return MICROPIXEL_STATUS_OK;
}

void GuestGraphicsEngine::Release() {
    if (display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (guest_frame_ != nullptr) {
        retained_scene_.ForgetObjects();
        lv_obj_delete(guest_frame_);
        guest_frame_ = nullptr;
        RequestDisplayRefresh(display_);
        ESP_LOGI(kTag, "Guest graphics tree released before Bitmap teardown");
    }
    ReleaseTextures(scene_texture_access_, scene_textures_, scene_texture_count_);
    scene_texture_count_ = 0U;
    scene_texture_access_ = {};
    ClearPendingFrameTextures(true);
    bitmap_update_frame_active_ = false;
    bitmap_damage_count_ = 0U;
    bitmap_frame_updates_ = 0U;
    bitmap_frame_bytes_ = 0U;
    graphics_frame_active_ = false;
    graphics_frame_length_ = 0U;
    graphics_frame_commands_ = 0U;
    heap_caps_free(graphics_frame_bytes_);
    graphics_frame_bytes_ = nullptr;
    heap_caps_free(texture_storage_);
    texture_storage_ = nullptr;
    graphics_frame_textures_ = nullptr;
    scene_textures_ = nullptr;
    scratch_textures_ = nullptr;
    retained_scene_.Release();
    fonts_.ReleaseGuestFonts();
    esp_lv_adapter_unlock();
}

int32_t GuestGraphicsEngine::BeginFrame() {
    if (display_ == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (graphics_frame_active_ || bitmap_update_frame_active_) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (!EnsureTextureStorage()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    if (graphics_frame_bytes_ == nullptr) {
        graphics_frame_bytes_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            4U, MICROPIXEL_GRAPHICS_MAX_FRAME_COMMAND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (graphics_frame_bytes_ == nullptr) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
    }
    std::memset(graphics_frame_bytes_, 0, sizeof(micropixel_graphics_command_header_t));
    graphics_frame_length_ = sizeof(micropixel_graphics_command_header_t);
    graphics_frame_commands_ = 0U;
    ClearPendingFrameTextures(true);
    graphics_frame_active_ = true;
    return MICROPIXEL_STATUS_OK;
}

int32_t GuestGraphicsEngine::Submit(const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures) {
    if (display_ == nullptr || bytes == nullptr || textures.resolve == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (!EnsureTextureStorage()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }

    const bool frame_active = graphics_frame_active_;
    const bool lvgl_locked = !frame_active;
    if (lvgl_locked && esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    const int32_t validation_status = graphics::ValidateCommandStream(bytes, length, width_, height_, textures.resolve,
                                                                      textures.context, ValidateFontHandle, this);
    if (validation_status != MICROPIXEL_STATUS_OK) {
        if (lvgl_locked) {
            esp_lv_adapter_unlock();
        }
        ESP_LOGW(kTag, "rejected graphics batch: status=%" PRId32 " bytes=%" PRIu32, validation_status, length);
        return validation_status;
    }
    uint32_t frame_texture_count = 0U;
    if (!graphics::CollectTextureHandles(bytes, length, scratch_textures_, kMaxSceneTextures, frame_texture_count)) {
        if (lvgl_locked) {
            esp_lv_adapter_unlock();
        }
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (graphics_frame_active_) {
        micropixel_graphics_command_header_t batch_header{};
        std::memcpy(&batch_header, bytes, sizeof(batch_header));
        const uint32_t record_bytes = length - sizeof(batch_header);
        if (graphics_frame_bytes_ == nullptr ||
            batch_header.command_count > MICROPIXEL_GRAPHICS_MAX_FRAME_COMMANDS - graphics_frame_commands_ ||
            record_bytes > MICROPIXEL_GRAPHICS_MAX_FRAME_COMMAND_BYTES - graphics_frame_length_) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        if (!AddPendingFrameTextures(scratch_textures_, frame_texture_count, textures)) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        std::memcpy(graphics_frame_bytes_ + graphics_frame_length_, bytes + sizeof(batch_header), record_bytes);
        graphics_frame_length_ += record_bytes;
        graphics_frame_commands_ += batch_header.command_count;
        return MICROPIXEL_STATUS_OK;
    }
    if (!RetainTextures(textures, scratch_textures_, frame_texture_count)) {
        esp_lv_adapter_unlock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    const int32_t execution_status = ApplyFrameLocked(bytes, length, textures, scratch_textures_, frame_texture_count);
    if (execution_status != MICROPIXEL_STATUS_OK) {
        ReleaseTextures(textures, scratch_textures_, frame_texture_count);
    }
    if (lvgl_locked) {
        esp_lv_adapter_unlock();
    }
    return execution_status;
}

int32_t GuestGraphicsEngine::CommitFrame(const device::TextureAccess& textures) {
    if (display_ == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (!graphics_frame_active_ || graphics_frame_bytes_ == nullptr || graphics_frame_commands_ == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    micropixel_graphics_command_header_t header{};
    header.magic = MICROPIXEL_GRAPHICS_COMMAND_MAGIC;
    header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    header.total_size = graphics_frame_length_;
    header.command_count = graphics_frame_commands_;
    std::memcpy(graphics_frame_bytes_, &header, sizeof(header));

    const uint32_t frame_commands = graphics_frame_commands_;
    const uint32_t frame_bytes = graphics_frame_length_;
    graphics_frame_active_ = false;
    graphics_frame_length_ = 0U;
    graphics_frame_commands_ = 0U;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ClearPendingFrameTextures(true);
        return MICROPIXEL_STATUS_INTERNAL;
    }
    const device::TextureAccess retained_access =
        graphics_frame_texture_count_ == 0U ? textures : graphics_frame_texture_access_;
    const int32_t status = ApplyFrameLocked(graphics_frame_bytes_, frame_bytes, retained_access,
                                            graphics_frame_textures_, graphics_frame_texture_count_);
    const uint32_t sequence = status == MICROPIXEL_STATUS_OK ? ++graphics_frame_sequence_ : 0U;
    ClearPendingFrameTextures(status != MICROPIXEL_STATUS_OK);
    esp_lv_adapter_unlock();
    if (status == MICROPIXEL_STATUS_OK && (sequence <= 8U || (sequence % 120U) == 0U)) {
        ESP_LOGI(kTag, "graphics frame #%" PRIu32 ": commands=%" PRIu32 " bytes=%" PRIu32, sequence, frame_commands,
                 frame_bytes);
    }
    return status;
}

int32_t GuestGraphicsEngine::CancelFrame() {
    if (!graphics_frame_active_) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    graphics_frame_active_ = false;
    graphics_frame_length_ = 0U;
    graphics_frame_commands_ = 0U;
    ClearPendingFrameTextures(true);
    return MICROPIXEL_STATUS_OK;
}

int32_t GuestGraphicsEngine::LoadFont(const device::FontResourceView& resource, micropixel_font_info_t& info_out) {
    if (resource.data == nullptr || resource.size == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return fonts_.LoadFont(std::span<const uint8_t>(resource.data, resource.size), info_out);
}

int32_t GuestGraphicsEngine::ReleaseFont(micropixel_font_handle_t font) { return fonts_.ReleaseFont(font); }

int32_t GuestGraphicsEngine::MeasureText(micropixel_font_handle_t font_handle, const char* text, uint32_t text_length,
                                         micropixel_text_metrics_t& metrics_out) {
    if (text == nullptr || text_length == 0U || text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES ||
        fonts_.ResolveGuestHandle(font_handle) == nullptr) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    char terminated[MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES + 1U]{};
    std::memcpy(terminated, text, text_length);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    const lv_font_t* font = fonts_.ResolveGuestHandle(font_handle);
    lv_point_t size{};
    lv_text_get_size(&size, terminated, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    metrics_out = {};
    metrics_out.size = sizeof(metrics_out);
    metrics_out.width = size.x < 0 ? 0U : static_cast<uint32_t>(size.x);
    metrics_out.height = size.y < 0 ? 0U : static_cast<uint32_t>(size.y);
    metrics_out.baseline = font->line_height - font->base_line;
    esp_lv_adapter_unlock();
    return MICROPIXEL_STATUS_OK;
}

int32_t GuestGraphicsEngine::BeginBitmapUpdateFrame() {
    if (display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (bitmap_update_frame_active_ || graphics_frame_active_) {
        esp_lv_adapter_unlock();
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    bitmap_update_frame_active_ = true;
    bitmap_damage_count_ = 0U;
    bitmap_frame_updates_ = 0U;
    bitmap_frame_bytes_ = 0U;
    bitmap_frame_started_us_ = static_cast<uint64_t>(esp_timer_get_time());
    esp_lv_adapter_unlock();
    return MICROPIXEL_STATUS_OK;
}

int32_t GuestGraphicsEngine::UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                          uint32_t height, const uint8_t* pixels, uint32_t stride) {
    const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
                                         ? 3U
                                         : (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 0U);
    if (display_ == nullptr || bitmap.data == nullptr || pixels == nullptr || bytes_per_pixel == 0U ||
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
    if (bitmap_update_frame_active_) {
        if (!AccumulateDamage(bitmap_damage_, kBitmapDamageCapacity, bitmap_damage_count_, bitmap.data, x, y, width,
                              height)) {
            esp_lv_adapter_unlock();
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        ++bitmap_frame_updates_;
        bitmap_frame_bytes_ += static_cast<uint64_t>(stride) * height;
    } else {
        const bool invalidated = retained_scene_.InvalidateBitmap(bitmap.data, x, y, width, height);
        if (invalidated) {
            RequestDisplayRefresh(display_);
        }
    }
    esp_lv_adapter_unlock();
    return MICROPIXEL_STATUS_OK;
}

int32_t GuestGraphicsEngine::CommitBitmapUpdateFrame() {
    if (display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (!bitmap_update_frame_active_) {
        esp_lv_adapter_unlock();
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    bool invalidated = false;
    uint32_t ppa_eligible_damage = 0U;
    for (uint32_t index = 0U; index < bitmap_damage_count_; ++index) {
        const BitmapDamage& damage = bitmap_damage_[index];
        invalidated = retained_scene_.InvalidateBitmap(damage.data, damage.x, damage.y, damage.width, damage.height) ||
                      invalidated;
        if (static_cast<uint64_t>(damage.width) * damage.height > CONFIG_MICROPIXEL_LVGL_PPA_MIN_AREA_PIXELS) {
            ++ppa_eligible_damage;
        }
    }
    if (invalidated) {
        RequestDisplayRefresh(display_);
    }

    const uint32_t updates = bitmap_frame_updates_;
    const uint64_t bytes = bitmap_frame_bytes_;
    const uint32_t damage_count = bitmap_damage_count_;
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t elapsed_us = now_us >= bitmap_frame_started_us_ ? now_us - bitmap_frame_started_us_ : 0U;
    const uint32_t sequence = updates == 0U ? bitmap_frame_sequence_ : ++bitmap_frame_sequence_;
    bitmap_update_frame_active_ = false;
    bitmap_damage_count_ = 0U;
    bitmap_frame_updates_ = 0U;
    bitmap_frame_bytes_ = 0U;
    esp_lv_adapter_unlock();

    if (updates != 0U && (sequence <= 8U || (sequence % 120U) == 0U)) {
        ESP_LOGI(kTag,
                 "offscreen frame #%" PRIu32 ": updates=%" PRIu32 " bytes=%" PRIu64 " unions=%" PRIu32
                 " ppa-eligible=%" PRIu32 " stage=%" PRIu64 " us present=%s",
                 sequence, updates, bytes, damage_count, ppa_eligible_damage, elapsed_us, invalidated ? "yes" : "no");
    }
    return MICROPIXEL_STATUS_OK;
}

}  // namespace micropixel::platform::lvgl
