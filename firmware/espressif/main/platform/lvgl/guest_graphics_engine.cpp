#include "platform/lvgl/guest_graphics_engine.hpp"

#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "work/background_executor.hpp"

namespace micropixel::platform::lvgl {
namespace {

constexpr char kTag[] = "guest_graphics";
constexpr size_t kAppSurfaceAlignment = 64U;
constexpr uint32_t kAppSurfaceStrideAlignmentPixels = 16U;
constexpr uint32_t kAppSurfaceTransformScratchRows = 16U;
// The publish timer is only ever fired explicitly with lv_timer_ready(); the
// period just bounds how often it runs idle.
constexpr uint32_t kPublishTimerPeriodMs = 60U * 1000U;
// Periodic telemetry goes out over the blocking 115200-baud UART console; the
// scene burst is ~1.5 KiB (>100 ms of UART time). It is written by the
// background executor, but it still competes for the console, so it stays
// spread thin (every ~10 s at 60 FPS).
constexpr uint32_t kSceneTelemetryPeriodFrames = 600U;
constexpr uint32_t kDisplayRefreshTelemetryPeriod = 300U;
// Histogram shapes of the hardware telemetry; kept local so the log layout is
// identical on CPU-only targets, where the hardware compositor is not built.
constexpr std::size_t kDma2dWidthBins = 4U;
constexpr std::size_t kPpaBlendBins = 8U;
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
static_assert(kDma2dWidthBins == graphics::Dma2dCopyEngine::kWidthBins);
static_assert(kPpaBlendBins == graphics::kPpaBlendHistogramBinCount);
#endif

uint16_t SceneWireInstanceCount(const uint8_t* bytes, uint32_t length) {
    micropixel_graphics_scene_header_t header{};
    if (bytes == nullptr || length < sizeof(header)) {
        return 0U;
    }
    std::memcpy(&header, bytes, sizeof(header));
    uint32_t offset = sizeof(header);
    uint32_t instances = 0U;
    for (uint16_t index = 0U; index < header.record_count; ++index) {
        micropixel_graphics_scene_record_header_t record{};
        std::memcpy(&record, bytes + offset, sizeof(record));
        if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES) {
            micropixel_graphics_scene_batch_instances_record_t batch{};
            std::memcpy(&batch, bytes + offset, sizeof(batch));
            instances += batch.instance_count;
        }
        offset += record.size;
    }
    return static_cast<uint16_t>(instances > UINT16_MAX ? UINT16_MAX : instances);
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

GuestGraphicsEngine::GuestGraphicsEngine(int32_t width, int32_t height, FontRegistry& fonts,
                                         graphics::SurfacePixelFormat app_surface_format)
    : width_(width),
      height_(height),
      app_surface_format_(app_surface_format),
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
      hardware_pixel_compositor_(software_pixel_compositor_),
#endif
      fonts_(fonts),
      bitmap_font_rasterizer_(fonts) {
}

bool GuestGraphicsEngine::ValidateFontHandle(void* context, micropixel_font_handle_t font) {
    return context != nullptr && static_cast<GuestGraphicsEngine*>(context)->fonts_.ResolveGuestHandle(font) != nullptr;
}

esp_err_t GuestGraphicsEngine::Initialize(lv_display_t* display, DirectFramebufferAccess* framebuffers) {
    if (display == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    display_refresh_ready_ = xSemaphoreCreateBinaryStatic(&display_refresh_ready_storage_);
    if (display_refresh_ready_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    display_ = display;
    if (width_ > 0 && static_cast<uint32_t>(width_) <= UINT32_MAX / (4U * kAppSurfaceTransformScratchRows)) {
        const uint64_t scratch_bytes =
            (static_cast<uint64_t>(width_) * 4U * kAppSurfaceTransformScratchRows + kAppSurfaceAlignment - 1U) &
            ~(kAppSurfaceAlignment - 1U);
        if (scratch_bytes <= UINT32_MAX && scratch_bytes <= SIZE_MAX) {
            software_transform_scratch_ = static_cast<uint8_t*>(heap_caps_aligned_alloc(
                kAppSurfaceAlignment, static_cast<size_t>(scratch_bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (software_transform_scratch_ != nullptr) {
                software_transform_scratch_bytes_ = static_cast<uint32_t>(scratch_bytes);
                software_pixel_compositor_.SetTransformScratch(software_transform_scratch_,
                                                               software_transform_scratch_bytes_);
            }
        }
    }
    if (software_transform_scratch_ == nullptr) {
        ESP_LOGW(kTag, "LVGL software scale scratch unavailable; reference CPU scale remains active");
    }
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY
    esp_lv_adapter_display_telemetry_t discarded{};
    (void)esp_lv_adapter_display_telemetry_take(&discarded);
#endif
#if !defined(CONFIG_SOC_PPA_SUPPORTED) || !CONFIG_SOC_PPA_SUPPORTED
    ESP_LOGI(kTag, "App Surface compositor: CPU-only target");
#elif CONFIG_MICROPIXEL_MOSAICO_SOFTWARE_RENDERING
    ESP_LOGI(kTag, "App Surface compositor: CPU-only S31 experiment");
#else
    const esp_err_t compositor_status =
        hardware_pixel_compositor_.Initialize(CONFIG_MICROPIXEL_APP_SURFACE_HW_MIN_AREA_PIXELS);
    if (compositor_status != ESP_OK) {
        ESP_LOGW(kTag, "hardware pixel compositor partially unavailable: %s; CPU fallback remains active",
                 esp_err_to_name(compositor_status));
    }
#endif
    (void)framebuffers;
    // Initialize() runs before the LVGL task starts, so creating the timer
    // here needs no lock.
    publish_timer_ = lv_timer_create(PublishTimerCallback, kPublishTimerPeriodMs, this);
    if (publish_timer_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_add_event_cb(display_, DisplayRefreshStartEvent, LV_EVENT_REFR_START, this);
    lv_display_add_event_cb(display_, DisplayRefreshReadyEvent, LV_EVENT_REFR_READY, this);
    return ESP_OK;
}

void GuestGraphicsEngine::RebindFramebuffers(DirectFramebufferAccess* framebuffers) { (void)framebuffers; }

void GuestGraphicsEngine::PublishTimerCallback(lv_timer_t* timer) {
    auto* engine = static_cast<GuestGraphicsEngine*>(lv_timer_get_user_data(timer));
    if (engine != nullptr) {
        engine->AdoptPendingFrameLocked(false);
    }
}

void GuestGraphicsEngine::DisplayRefreshStartEvent(lv_event_t* event) {
    auto* engine = static_cast<GuestGraphicsEngine*>(lv_event_get_user_data(event));
    if (engine == nullptr) {
        return;
    }
    // Adopt before the dirty areas are coalesced so the newest Guest frame is
    // what this refresh copies to the panel.
    engine->AdoptPendingFrameLocked(true);
    engine->refresh_damage_ = engine->dirty_region_coalescer_.Coalesce(engine->display_);
    engine->guest_refresh_active_ = engine->guest_refresh_pending_;
    engine->guest_refresh_pending_ = false;
    engine->display_refresh_started_us_ = esp_timer_get_time();
}

void GuestGraphicsEngine::DisplayRefreshReadyEvent(lv_event_t* event) {
    auto* engine = static_cast<GuestGraphicsEngine*>(lv_event_get_user_data(event));
    if (engine == nullptr) {
        return;
    }
    if (engine->display_refresh_started_us_ != 0) {
        const uint32_t sequence = ++engine->display_refresh_sequence_;
        const bool guest_refresh = engine->guest_refresh_active_;
        if (guest_refresh) {
            ++engine->guest_presented_frame_sequence_;
        }
        engine->guest_refresh_active_ = false;
        const uint32_t duration_us = static_cast<uint32_t>(esp_timer_get_time() - engine->display_refresh_started_us_);
        engine->display_refresh_started_us_ = 0;
#if CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG
        if (sequence <= 4U || (sequence % kDisplayRefreshTelemetryPeriod) == 0U) {
            ESP_LOGI(kTag,
                     "display refresh #%" PRIu32 ": total=%" PRIu32 " us damage=%" PRIu32 "->%" PRIu32
                     " regions/%" PRIu64 "->%" PRIu64 " pixels guest=%s",
                     sequence, duration_us, engine->refresh_damage_.input_regions,
                     engine->refresh_damage_.output_regions, engine->refresh_damage_.input_pixels,
                     engine->refresh_damage_.output_pixels, guest_refresh ? "yes" : "no");
        }
#else
        (void)duration_us;
        (void)guest_refresh;
#endif
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
    info.pixel_format = app_surface_format_ == graphics::SurfacePixelFormat::kRgb565 ? MICROPIXEL_PIXEL_FORMAT_RGB565
                                                                                     : MICROPIXEL_PIXEL_FORMAT_BGR888;
    info.max_containers = MICROPIXEL_GRAPHICS_MAX_CONTAINERS;
    info.max_scene_bytes = MICROPIXEL_GRAPHICS_MAX_SCENE_BYTES;
    info.max_scene_nodes = MICROPIXEL_GRAPHICS_MAX_SCENE_NODES;
    info.max_batch_instances = MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES;
    info.max_sprite_batches = MICROPIXEL_GRAPHICS_MAX_SPRITE_BATCHES;
    info.reserved0 = 0U;
    return MICROPIXEL_STATUS_OK;
}

bool GuestGraphicsEngine::EnsureTextureStorage() {
    if (texture_storage_ != nullptr && font_storage_ != nullptr) {
        return true;
    }
    constexpr uint32_t kTextureArrayCount = 2U;
    texture_storage_ = static_cast<micropixel_texture_handle_t*>(
        heap_caps_aligned_calloc(alignof(micropixel_texture_handle_t), kTextureArrayCount * kMaxSceneTextures,
                                 sizeof(micropixel_texture_handle_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (texture_storage_ == nullptr) {
        return false;
    }
    font_storage_ = static_cast<micropixel_font_handle_t*>(
        heap_caps_aligned_calloc(alignof(micropixel_font_handle_t), kTextureArrayCount * kMaxSceneTextures,
                                 sizeof(micropixel_font_handle_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (font_storage_ == nullptr) {
        heap_caps_free(texture_storage_);
        texture_storage_ = nullptr;
        return false;
    }
    scene_textures_ = texture_storage_;
    scratch_textures_ = texture_storage_ + kMaxSceneTextures;
    scene_fonts_ = font_storage_;
    scratch_fonts_ = font_storage_ + kMaxSceneTextures;
    return true;
}

bool GuestGraphicsEngine::EnsureSceneStorage() {
    if (guest_scene_.has_value()) {
        return true;
    }
    guest_scene_node_storage_ = static_cast<graphics::GuestSceneNode*>(
        heap_caps_aligned_calloc(alignof(graphics::GuestSceneNode), 2U * MICROPIXEL_GRAPHICS_MAX_SCENE_NODES,
                                 sizeof(graphics::GuestSceneNode), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    guest_scene_instance_storage_ = static_cast<graphics::GuestSceneSpriteInstance*>(heap_caps_aligned_calloc(
        alignof(graphics::GuestSceneSpriteInstance), 2U * MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES,
        sizeof(graphics::GuestSceneSpriteInstance), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    guest_scene_container_storage_ = static_cast<graphics::GuestSceneContainer*>(
        heap_caps_aligned_calloc(alignof(graphics::GuestSceneContainer), 2U * (MICROPIXEL_GRAPHICS_MAX_CONTAINERS + 1U),
                                 sizeof(graphics::GuestSceneContainer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    guest_scene_draw_order_storage_ =
        static_cast<uint16_t*>(heap_caps_aligned_calloc(alignof(uint16_t), 2U * MICROPIXEL_GRAPHICS_MAX_SCENE_NODES,
                                                        sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (guest_scene_node_storage_ == nullptr || guest_scene_instance_storage_ == nullptr ||
        guest_scene_container_storage_ == nullptr || guest_scene_draw_order_storage_ == nullptr) {
        heap_caps_free(guest_scene_node_storage_);
        heap_caps_free(guest_scene_instance_storage_);
        heap_caps_free(guest_scene_container_storage_);
        heap_caps_free(guest_scene_draw_order_storage_);
        guest_scene_node_storage_ = nullptr;
        guest_scene_instance_storage_ = nullptr;
        guest_scene_container_storage_ = nullptr;
        guest_scene_draw_order_storage_ = nullptr;
        return false;
    }
    guest_scene_.emplace(guest_scene_node_storage_, guest_scene_node_storage_ + MICROPIXEL_GRAPHICS_MAX_SCENE_NODES,
                         MICROPIXEL_GRAPHICS_MAX_SCENE_NODES, guest_scene_instance_storage_,
                         guest_scene_instance_storage_ + MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES,
                         MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES, guest_scene_container_storage_,
                         guest_scene_container_storage_ + MICROPIXEL_GRAPHICS_MAX_CONTAINERS + 1U,
                         guest_scene_draw_order_storage_,
                         guest_scene_draw_order_storage_ + MICROPIXEL_GRAPHICS_MAX_SCENE_NODES);
    return true;
}

bool GuestGraphicsEngine::EnsureAppSurfaceStorage() {
    if (app_surface_compositor_.has_value()) {
        return true;
    }
    if (app_surface_allocation_failed_ || width_ <= 0 || height_ <= 0 ||
        static_cast<uint32_t>(width_) > UINT32_MAX - (kAppSurfaceStrideAlignmentPixels - 1U)) {
        return false;
    }
    const uint32_t storage_width = (static_cast<uint32_t>(width_) + kAppSurfaceStrideAlignmentPixels - 1U) &
                                   ~(kAppSurfaceStrideAlignmentPixels - 1U);
    const uint32_t bytes_per_pixel = app_surface_format_ == graphics::SurfacePixelFormat::kRgb565 ? 2U : 3U;
    const uint64_t stride = static_cast<uint64_t>(storage_width) * bytes_per_pixel;
    const uint64_t pixel_bytes = stride * static_cast<uint32_t>(height_);
    const uint64_t allocation_bytes = (pixel_bytes + kAppSurfaceAlignment - 1U) & ~(kAppSurfaceAlignment - 1U);
    if (stride > UINT32_MAX || pixel_bytes == 0U || allocation_bytes > UINT32_MAX || allocation_bytes > SIZE_MAX / 4U) {
        app_surface_allocation_failed_ = true;
        return false;
    }
    static_assert(CONFIG_MICROPIXEL_APP_SURFACE_COUNT >= 1 &&
                  CONFIG_MICROPIXEL_APP_SURFACE_COUNT <=
                      static_cast<int>(graphics::AppSurfaceCompositor::kMaxSurfaces));
    app_surface_count_ = static_cast<uint8_t>(CONFIG_MICROPIXEL_APP_SURFACE_COUNT);
    displayed_surface_ = 0U;
    pending_ = {};
    // Surfaces first, then the Layer cache slot.
    app_surface_pixels_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kAppSurfaceAlignment, static_cast<size_t>(allocation_bytes * (app_surface_count_ + 1U)),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    app_surface_operation_storage_ = static_cast<graphics::AppDrawOperation*>(
        heap_caps_aligned_calloc(kAppSurfaceAlignment, 2U * MICROPIXEL_GRAPHICS_MAX_SCENE_NODES,
                                 sizeof(graphics::AppDrawOperation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (app_surface_pixels_ == nullptr || app_surface_operation_storage_ == nullptr) {
        heap_caps_free(app_surface_pixels_);
        heap_caps_free(app_surface_operation_storage_);
        app_surface_pixels_ = nullptr;
        app_surface_layer_pixels_ = nullptr;
        app_surface_operation_storage_ = nullptr;
        app_surface_allocation_failed_ = true;
        ESP_LOGW(kTag, "App Surface allocation failed; retaining LVGL Guest fallback");
        return false;
    }

    constexpr graphics::DamageMergePolicy kDamageMergePolicy{
        .max_extra_pixels = CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_EXTRA_PIXELS,
        .max_region_pixels = CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_MAX_PIXELS,
    };
    app_surface_pixel_bytes_ = static_cast<uint32_t>(pixel_bytes);
    app_surface_allocation_bytes_ = static_cast<uint32_t>(allocation_bytes);
    app_surface_stride_ = static_cast<uint32_t>(stride);
    app_surface_layer_pixels_ = app_surface_pixels_ + app_surface_allocation_bytes_ * app_surface_count_;
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    graphics::PixelCompositor& pixel_compositor = hardware_pixel_compositor_;
#else
    graphics::PixelCompositor& pixel_compositor = software_pixel_compositor_;
#endif
    app_surface_compositor_.emplace(
        app_surface_operation_storage_, app_surface_operation_storage_ + MICROPIXEL_GRAPHICS_MAX_SCENE_NODES,
        MICROPIXEL_GRAPHICS_MAX_SCENE_NODES, pixel_compositor, kDamageMergePolicy, &bitmap_font_rasterizer_);
    app_surface_compositor_->SetLayerCache({
        .pixels = app_surface_layer_pixels_,
        .size = app_surface_allocation_bytes_,
        .width = static_cast<uint32_t>(width_),
        .height = static_cast<uint32_t>(height_),
        .stride = app_surface_stride_,
        .format = app_surface_format_,
    });
    app_surface_compositor_->SetClock([]() { return static_cast<uint64_t>(esp_timer_get_time()); });
    app_surface_image_descriptor_ = {};
    app_surface_image_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    app_surface_image_descriptor_.header.cf =
        app_surface_format_ == graphics::SurfacePixelFormat::kRgb565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB888;
    app_surface_image_descriptor_.header.w = static_cast<uint32_t>(width_);
    app_surface_image_descriptor_.header.h = static_cast<uint32_t>(height_);
    app_surface_image_descriptor_.header.stride = app_surface_stride_;
    app_surface_image_descriptor_.data_size = app_surface_pixel_bytes_;
    app_surface_image_descriptor_.data = SurfaceAt(displayed_surface_).pixels;
    ESP_LOGI(kTag,
             "App Surface ready: %" PRId32 "x%" PRId32 " %s stride=%" PRIu32 " pixels=%" PRIu32
             " surfaces=%u"
             " layer-cache=%" PRIu32 " transform-scratch=%" PRIu32 " scene=%zu bytes",
             width_, height_, app_surface_format_ == graphics::SurfacePixelFormat::kRgb565 ? "RGB565" : "RGB888",
             app_surface_stride_, app_surface_pixel_bytes_, static_cast<unsigned>(app_surface_count_),
             app_surface_allocation_bytes_, software_transform_scratch_bytes_,
             2U * MICROPIXEL_GRAPHICS_MAX_SCENE_NODES * sizeof(graphics::AppDrawOperation));
    return true;
}

graphics::PixelSurface GuestGraphicsEngine::SurfaceAt(uint8_t index) const {
    if (app_surface_pixels_ == nullptr) {
        return {};
    }
    return {
        .pixels = app_surface_pixels_ + static_cast<size_t>(app_surface_allocation_bytes_) * index,
        .size = app_surface_allocation_bytes_,
        .width = static_cast<uint32_t>(width_),
        .height = static_cast<uint32_t>(height_),
        .stride = app_surface_stride_,
        .format = app_surface_format_,
    };
}

void GuestGraphicsEngine::ShowAppSurfaceLocked() {
    if (app_surface_image_ == nullptr) {
        app_surface_image_ = lv_image_create(guest_frame_);
        lv_image_set_src(app_surface_image_, &app_surface_image_descriptor_);
        lv_image_set_inner_align(app_surface_image_, LV_IMAGE_ALIGN_TOP_LEFT);
        lv_image_set_pivot(app_surface_image_, 0, 0);
        lv_image_set_antialias(app_surface_image_, false);
        lv_obj_set_pos(app_surface_image_, 0, 0);
        lv_obj_set_size(app_surface_image_, width_, height_);
        lv_obj_remove_flag(app_surface_image_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(app_surface_image_, LV_OBJ_FLAG_CLICKABLE);
    }
    if (!app_surface_active_) {
        lv_obj_remove_flag(app_surface_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(app_surface_image_);
        app_surface_active_ = true;
    }
}

void GuestGraphicsEngine::HideAppSurfaceLocked() {
    if (app_surface_image_ != nullptr && app_surface_active_) {
        lv_obj_add_flag(app_surface_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (app_surface_active_ && app_surface_compositor_.has_value()) {
        // Frames rendered by LVGL are not mirrored into the hidden pixel cache.
        // Force a complete reconstruction before this path becomes visible again.
        app_surface_compositor_->Reset();
    }
    app_surface_active_ = false;
}

uint8_t GuestGraphicsEngine::AcquireComposeSurface() {
    if (app_surface_count_ < 2U) {
        return 0U;
    }
    taskENTER_CRITICAL(&publish_lock_);
    const uint8_t displayed = displayed_surface_;
    const uint8_t pending = pending_.surface;
    uint8_t target = kNoSurface;
    for (uint8_t index = 0U; index < app_surface_count_; ++index) {
        if (index != displayed && index != pending) {
            target = index;
            break;
        }
    }
    if (target == kNoSurface) {
        // Two surfaces and LVGL has not adopted the last frame yet: take that
        // surface back. Its damage stays in the mailbox so the superseding
        // frame still invalidates everything the panel has not seen.
        target = pending;
        pending_.surface = kNoSurface;
    }
    taskEXIT_CRITICAL(&publish_lock_);
    return target;
}

void GuestGraphicsEngine::PublishSurface(uint8_t surface, bool visual_changed) {
    if (!app_surface_compositor_.has_value()) {
        return;
    }
    constexpr graphics::DamageMergePolicy kDamageMergePolicy{
        .max_extra_pixels = CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_EXTRA_PIXELS,
        .max_region_pixels = CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_MAX_PIXELS,
    };
    const graphics::AppSurfaceCompositor& compositor = *app_surface_compositor_;
    taskENTER_CRITICAL(&publish_lock_);
    const bool replaced = pending_.surface != kNoSurface && pending_.surface != surface;
    pending_.surface = surface;
    pending_.visual_changed = pending_.visual_changed || visual_changed;
    // Only content changes need to reach the panel; carry-over replayed into
    // this surface reproduces pixels the panel already shows.
    for (size_t index = 0U; index < compositor.ContentDamageCount() && !pending_.damage_overflow; ++index) {
        pending_.damage_overflow = !pending_.damage.Add(this, compositor.ContentDamage(index), kDamageMergePolicy);
    }
    taskEXIT_CRITICAL(&publish_lock_);
    if (replaced) {
        ++submit_stage_telemetry_.frames_replaced;
    }
    if (publish_timer_ != nullptr) {
        // Plain stores on a timer LVGL never deletes; the adapter wake makes
        // the LVGL task re-run its timers even while the refresh timer is
        // paused waiting for an invalidation.
        lv_timer_ready(publish_timer_);
    }
    (void)esp_lv_adapter_request_wake();
}

void GuestGraphicsEngine::ClearPendingFrame() {
    taskENTER_CRITICAL(&publish_lock_);
    pending_ = {};
    taskEXIT_CRITICAL(&publish_lock_);
}

void GuestGraphicsEngine::AdoptPendingFrameLocked(bool inside_refresh) {
    PendingFrame frame{};
    taskENTER_CRITICAL(&publish_lock_);
    if (pending_.surface != kNoSurface) {
        frame = pending_;
        pending_ = {};
        displayed_surface_ = frame.surface;
    }
    taskEXIT_CRITICAL(&publish_lock_);
    if (frame.surface == kNoSurface || !app_surface_compositor_.has_value()) {
        return;
    }
    const bool created_guest_frame = guest_frame_ == nullptr;
    if (created_guest_frame) {
        guest_frame_ = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(guest_frame_, width_, height_, 0x000000U);
    }
    app_surface_image_descriptor_.data = SurfaceAt(frame.surface).pixels;
    const bool became_visible = !app_surface_active_;
    ShowAppSurfaceLocked();
    if (frame.damage_overflow || became_visible) {
        lv_obj_invalidate(app_surface_image_);
    } else {
        lv_area_t image_area{};
        lv_obj_get_coords(app_surface_image_, &image_area);
        for (size_t index = 0U; index < frame.damage.Size(); ++index) {
            const graphics::DamageRect damage = frame.damage[index].rect;
            const lv_area_t area{
                .x1 = image_area.x1 + static_cast<int32_t>(damage.x),
                .y1 = image_area.y1 + static_cast<int32_t>(damage.y),
                .x2 = image_area.x1 + static_cast<int32_t>(damage.x + damage.width) - 1,
                .y2 = image_area.y1 + static_cast<int32_t>(damage.y + damage.height) - 1,
            };
            (void)lv_obj_invalidate_area(app_surface_image_, &area);
        }
    }
    bool needs_present = created_guest_frame || became_visible || frame.visual_changed;
    if (presentation_hooks_.prepare_frame_locked != nullptr) {
        presentation_hooks_.prepare_frame_locked(presentation_hooks_.context, guest_frame_, created_guest_frame,
                                                 needs_present);
    }
    guest_refresh_pending_ = guest_refresh_pending_ || needs_present;
    // The invalidations above resumed the refresh timer, but its period is
    // deliberately long while the screen is static, so outside a refresh it
    // also has to be marked ready. Inside REFR_START the current refresh
    // already consumes the new dirty areas; marking ready there would queue a
    // second, empty refresh right behind it.
    if (needs_present && !inside_refresh) {
        RequestDisplayRefresh(display_);
    }
}

bool GuestGraphicsEngine::RefreshAppSurfaceBitmap(const uint8_t* bitmap_data, graphics::DamageRect damage) {
    if (!app_surface_compositor_.has_value() || !app_surface_compositor_->Synchronized()) {
        // No scene presented yet (or the surface was reset): the next Submit
        // rebuilds everything from the retained scene anyway.
        return false;
    }
    const uint8_t target = AcquireComposeSurface();
    const graphics::AppSurfaceFrameResult result =
        app_surface_compositor_->RefreshBitmap(bitmap_data, damage, SurfaceAt(target));
    if (result.status != graphics::AppSurfaceStatus::kOk) {
        ESP_LOGE(kTag, "App Surface bitmap refresh failed: status=%u", static_cast<unsigned>(result.status));
        return false;
    }
    // Always publish: AcquireComposeSurface() may have taken back a frame that
    // was still waiting in the mailbox, and that content must reach LVGL.
    PublishSurface(target, result.visual_changed);
    return result.visual_changed;
}

void GuestGraphicsEngine::ReleaseAppSurfaceLocked() {
    ClearPendingFrame();
    app_surface_active_ = false;
    app_surface_image_ = nullptr;
    app_surface_compositor_.reset();
    heap_caps_free(app_surface_operation_storage_);
    heap_caps_free(app_surface_pixels_);
    app_surface_operation_storage_ = nullptr;
    app_surface_pixels_ = nullptr;
    app_surface_layer_pixels_ = nullptr;
    app_surface_pixel_bytes_ = 0U;
    app_surface_allocation_bytes_ = 0U;
    app_surface_stride_ = 0U;
    app_surface_image_descriptor_ = {};
    app_surface_allocation_failed_ = false;
    app_surface_frame_sequence_ = 0U;
    scene_wire_bytes_ = 0U;
    scene_wire_records_ = 0U;
    scene_wire_instances_ = 0U;
    layer_snapshot_telemetry_active_ = false;
}

void GuestGraphicsEngine::ReleaseFonts(const micropixel_font_handle_t* fonts, uint32_t count) {
    for (uint32_t index = 0U; index < count; ++index) {
        fonts_.ReleaseSceneFont(fonts[index]);
    }
}

bool GuestGraphicsEngine::RetainFonts(const micropixel_font_handle_t* fonts, uint32_t count) {
    uint32_t retained = 0U;
    for (; retained < count; ++retained) {
        if (!fonts_.RetainSceneFont(fonts[retained])) {
            break;
        }
    }
    if (retained == count) {
        return true;
    }
    ReleaseFonts(fonts, retained);
    return false;
}

int32_t GuestGraphicsEngine::ComposeScene(const device::TextureAccess& textures, graphics::PixelSurface target,
                                          graphics::AppSurfaceFrameResult& result) {
    if (!guest_scene_.has_value() || !app_surface_compositor_.has_value()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    const int64_t compose_started_us = esp_timer_get_time();
    result = app_surface_compositor_->PresentScene(*guest_scene_, target, textures.resolve, textures.context);
    submit_stage_telemetry_.compose_us += static_cast<uint64_t>(esp_timer_get_time() - compose_started_us);
    submit_stage_telemetry_.normalize_us += result.normalize_us;
    submit_stage_telemetry_.damage_us += result.damage_us;
    submit_stage_telemetry_.render_us += result.render_us;
    if (result.status != graphics::AppSurfaceStatus::kOk) {
        ESP_LOGE(kTag, "App Surface scene render failed: status=%u", static_cast<unsigned>(result.status));
        const graphics::AppSurfaceRenderFailure& failure = app_surface_compositor_->LastRenderFailure();
        if (result.status == graphics::AppSurfaceStatus::kRenderFailed && failure.valid) {
            ESP_LOGE(kTag,
                     "  rejected op kind=%u%s dst=%" PRId32 ",%" PRId32 " %" PRId32 "x%" PRId32 " window=%" PRId32
                     ",%" PRId32 " %" PRId32 "x%" PRId32 " src=%" PRId32 ",%" PRId32 " %" PRId32 "x%" PRId32
                     " bitmap=%" PRIu32 "x%" PRIu32 " fmt=%" PRIu32 " target=%" PRIu32 "x%" PRIu32 " fmt=%u",
                     static_cast<unsigned>(failure.kind), failure.layer_snapshot ? "(layer-snapshot)" : "",
                     failure.destination.x, failure.destination.y, failure.destination.width,
                     failure.destination.height, failure.draw_bounds.x, failure.draw_bounds.y,
                     failure.draw_bounds.width, failure.draw_bounds.height, failure.source.x, failure.source.y,
                     failure.source.width, failure.source.height, failure.bitmap_width, failure.bitmap_height,
                     failure.bitmap_format, target.width, target.height, static_cast<unsigned>(target.format));
        }
        return result.status == graphics::AppSurfaceStatus::kResourceExhausted ? MICROPIXEL_STATUS_RESOURCE_EXHAUSTED
                                                                               : MICROPIXEL_STATUS_INTERNAL;
    }
    return MICROPIXEL_STATUS_OK;
}

void GuestGraphicsEngine::PublishScene(const device::TextureAccess& textures, uint8_t surface,
                                       const graphics::AppSurfaceFrameResult& result,
                                       const micropixel_texture_handle_t* retained_textures,
                                       uint32_t retained_texture_count, const micropixel_font_handle_t* retained_fonts,
                                       uint32_t retained_font_count) {
    const uint32_t sequence = ++app_surface_frame_sequence_;
    LogSceneTelemetry(result, sequence);
    // The previous frame's resources stay retained until this frame is fully
    // composed: the compositor may have read them while replaying damage.
    ReleaseTextures(scene_texture_access_, scene_textures_, scene_texture_count_);
    std::memcpy(scene_textures_, retained_textures, retained_texture_count * sizeof(retained_textures[0]));
    scene_texture_count_ = retained_texture_count;
    scene_texture_access_ = retained_texture_count == 0U ? device::TextureAccess{} : textures;
    ReleaseFonts(scene_fonts_, scene_font_count_);
    std::memcpy(scene_fonts_, retained_fonts, retained_font_count * sizeof(retained_fonts[0]));
    scene_font_count_ = retained_font_count;
    PublishSurface(surface, result.visual_changed);
}

#if CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG
void GuestGraphicsEngine::LogSceneTelemetry(const graphics::AppSurfaceFrameResult& result, uint32_t sequence) {
    const bool layer_snapshot_transition = result.layer_snapshot_used != layer_snapshot_telemetry_active_;
    layer_snapshot_telemetry_active_ = result.layer_snapshot_used;
    const bool periodic = (sequence % kSceneTelemetryPeriodFrames) == 0U;
    const bool transition_line = layer_snapshot_transition && sequence > 8U && !periodic;
    const bool stage_line = periodic && submit_stage_telemetry_.frames != 0U;
    if (!transition_line && !periodic && sequence > 8U) {
        return;
    }
    if (!BeginTelemetryReport()) {
        if (stage_line) {
            submit_stage_telemetry_.Reset();
        }
        return;
    }
    if (transition_line) {
        // Layer cache entry/exit is an acceptance signal (see the Snake shake
        // criteria); one short line records it.
        AppendTelemetryLine("App Surface scene #%" PRIu32 ": layer-cache=%s damage=%" PRIu32 "/%" PRIu64
                            " pixels replays=%" PRIu32 " wire=%" PRIu16 "rec/%" PRIu16 "inst",
                            sequence, result.layer_snapshot_used ? "yes" : "no", result.damage_region_count,
                            result.damage_pixels, result.draw_operations_replayed, scene_wire_records_,
                            scene_wire_instances_);
    }
    if (sequence <= 8U || (sequence % kSceneTelemetryPeriodFrames) == 0U) {
        const SoftwarePixelCompositorStats software = software_pixel_compositor_.Stats();
        struct HardwareStats final {
            uint32_t ppa_fills{};
            uint32_t ppa_blends{};
            uint32_t small_blend_candidates{};
            uint32_t ppa_scales{};
            uint32_t dma2d_copies{};
            uint32_t dma2d_batches{};
            uint32_t software_fallbacks{};
            uint64_t ppa_blend_pixels{};
            uint64_t ppa_blend_elapsed_us{};
            uint64_t ppa_fill_elapsed_us{};
            uint64_t dma2d_elapsed_us{};
            uint64_t software_elapsed_us{};
            uint64_t dma2d_pixels{};
            uint64_t dma2d_sync_us{};
            uint64_t dma2d_wait_us{};
            uint64_t dma2d_max_wait_us{};
            uint64_t dma2d_max_wait_pixels{};
            uint64_t dma2d_channel_wait_us{};
            uint64_t dma2d_hardware_us{};
            uint64_t dma2d_wakeup_us{};
            std::array<uint32_t, kDma2dWidthBins> dma2d_blocks_by_width{};
            std::array<uint64_t, kDma2dWidthBins> dma2d_pixels_by_width{};
            std::array<uint32_t, kPpaBlendBins> ppa_blend_histogram_calls{};
            std::array<uint64_t, kPpaBlendBins> ppa_blend_histogram_elapsed_us{};
        } hardware;
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
        const graphics::HardwarePixelCompositorStats measured = hardware_pixel_compositor_.Stats();
        hardware = {
            .ppa_fills = measured.ppa_fills,
            .ppa_blends = measured.ppa_blends,
            .small_blend_candidates = measured.small_blend_candidates,
            .ppa_scales = measured.ppa_scales,
            .dma2d_copies = measured.dma2d_copies,
            .dma2d_batches = measured.dma2d_batches,
            .software_fallbacks = measured.software_fallbacks,
            .ppa_blend_pixels = measured.ppa_blend_pixels,
            .ppa_blend_elapsed_us = measured.ppa_blend_elapsed_us,
            .ppa_fill_elapsed_us = measured.ppa_fill_elapsed_us,
            .dma2d_elapsed_us = measured.dma2d_elapsed_us,
            .software_elapsed_us = measured.software_elapsed_us,
            .ppa_blend_histogram_calls = measured.ppa_blend_histogram_calls,
            .ppa_blend_histogram_elapsed_us = measured.ppa_blend_histogram_elapsed_us,
        };
        if (const graphics::Dma2dCopyEngine* copy_engine = hardware_pixel_compositor_.DirectCopyEngine();
            copy_engine != nullptr) {
            hardware.dma2d_pixels = copy_engine->PixelsCopied();
            hardware.dma2d_sync_us = copy_engine->CacheSyncMicros();
            hardware.dma2d_wait_us = copy_engine->TransferWaitMicros();
            hardware.dma2d_max_wait_us = copy_engine->MaxTransferWaitMicros();
            hardware.dma2d_max_wait_pixels = copy_engine->MaxTransferWaitPixels();
            hardware.dma2d_channel_wait_us = copy_engine->ChannelWaitMicros();
            hardware.dma2d_hardware_us = copy_engine->HardwareMicros();
            hardware.dma2d_wakeup_us = copy_engine->WakeupMicros();
            for (std::size_t bin = 0U; bin < kDma2dWidthBins; ++bin) {
                hardware.dma2d_blocks_by_width[bin] = copy_engine->BlocksByWidth(bin);
                hardware.dma2d_pixels_by_width[bin] = copy_engine->PixelsByWidth(bin);
            }
        }
#endif
        AppendTelemetryLine("App Surface scene #%" PRIu32 ": revision=%" PRIu32 " damage=%" PRIu32 "/%" PRIu64
                            " pixels replays=%" PRIu32 " normalize=%" PRIu32 "/%s wire=%" PRIu16 "rec/%" PRIu16
                            "inst/%" PRIu32 "B layer-cache=%s capacity-merges=%" PRIu32 " hw=fill:%" PRIu32
                            "/blend:%" PRIu32 "/scale:%" PRIu32 "/dma2d:%" PRIu32 "/%" PRIu32 "batches cpu:%" PRIu32
                            " blend-work=%" PRIu64 "px/%" PRIu64 "us/small-candidates:%" PRIu32 " sw=blit:%" PRIu64
                            "px/rgb888-to-rgb565:%" PRIu64 "px/alpha:%" PRIu64 "px",
                            sequence, guest_scene_->Revision(), result.damage_region_count, result.damage_pixels,
                            result.draw_operations_replayed, result.operations_normalized,
                            result.incremental_normalization ? "patch" : "full", scene_wire_records_,
                            scene_wire_instances_, scene_wire_bytes_, result.layer_snapshot_used ? "yes" : "no",
                            result.capacity_merge_count, hardware.ppa_fills, hardware.ppa_blends, hardware.ppa_scales,
                            hardware.dma2d_copies, hardware.dma2d_batches, hardware.software_fallbacks,
                            hardware.ppa_blend_pixels, hardware.ppa_blend_elapsed_us, hardware.small_blend_candidates,
                            software.blit_pixels, software.rgb888_to_rgb565_pixels, software.alpha_blend_pixels);
        // Kept as a separate line so each retained log entry stays short.
        AppendTelemetryLine(
            "App Surface engines #%" PRIu32 " cumulative us: fill=%" PRIu64 " dma2d=%" PRIu64 " (%" PRIu64
            "px sync=%" PRIu64 " wait=%" PRIu64 "=chan:%" PRIu64 "+hw:%" PRIu64 "+wake:%" PRIu64 " max-wait=%" PRIu64
            "us@%" PRIu64 "px) cpu=%" PRIu64 " dma2d-width<16/<64/<256/wide=%" PRIu32 "/%" PRIu32 "/%" PRIu32
            "/%" PRIu32 " blocks %" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "px",
            sequence, hardware.ppa_fill_elapsed_us, hardware.dma2d_elapsed_us, hardware.dma2d_pixels,
            hardware.dma2d_sync_us, hardware.dma2d_wait_us, hardware.dma2d_channel_wait_us, hardware.dma2d_hardware_us,
            hardware.dma2d_wakeup_us, hardware.dma2d_max_wait_us, hardware.dma2d_max_wait_pixels,
            hardware.software_elapsed_us, hardware.dma2d_blocks_by_width[0], hardware.dma2d_blocks_by_width[1],
            hardware.dma2d_blocks_by_width[2], hardware.dma2d_blocks_by_width[3], hardware.dma2d_pixels_by_width[0],
            hardware.dma2d_pixels_by_width[1], hardware.dma2d_pixels_by_width[2], hardware.dma2d_pixels_by_width[3]);
        AppendTelemetryLine("PPA blend histogram #%" PRIu32 " cumulative calls/us: 100-255=%" PRIu32 "/%" PRIu64
                            " 256-511=%" PRIu32 "/%" PRIu64 " 512-839=%" PRIu32 "/%" PRIu64 " 840-1023=%" PRIu32
                            "/%" PRIu64 " 1024-2047=%" PRIu32 "/%" PRIu64 " 2048-4095=%" PRIu32 "/%" PRIu64
                            " 4096-16383=%" PRIu32 "/%" PRIu64 " 16384+=%" PRIu32 "/%" PRIu64,
                            sequence, hardware.ppa_blend_histogram_calls[0], hardware.ppa_blend_histogram_elapsed_us[0],
                            hardware.ppa_blend_histogram_calls[1], hardware.ppa_blend_histogram_elapsed_us[1],
                            hardware.ppa_blend_histogram_calls[2], hardware.ppa_blend_histogram_elapsed_us[2],
                            hardware.ppa_blend_histogram_calls[3], hardware.ppa_blend_histogram_elapsed_us[3],
                            hardware.ppa_blend_histogram_calls[4], hardware.ppa_blend_histogram_elapsed_us[4],
                            hardware.ppa_blend_histogram_calls[5], hardware.ppa_blend_histogram_elapsed_us[5],
                            hardware.ppa_blend_histogram_calls[6], hardware.ppa_blend_histogram_elapsed_us[6],
                            hardware.ppa_blend_histogram_calls[7], hardware.ppa_blend_histogram_elapsed_us[7]);
    }
    if (stage_line) {
        // The current frame's compose time is already accumulated; its lock/apply
        // share lands in the next window, which is negligible over the period.
        const SubmitStageTelemetry& stages = submit_stage_telemetry_;
        AppendTelemetryLine("submit stages (%" PRIu32 " frames avg us): lock-wait=%" PRIu64 " apply=%" PRIu64
                            " compose=%" PRIu64 " (normalize=%" PRIu64 " damage=%" PRIu64 " render=%" PRIu64
                            ") total=%" PRIu64 " max-total=%" PRIu64 " replaced=%" PRIu32,
                            stages.frames, stages.lock_wait_us / stages.frames, stages.apply_us / stages.frames,
                            stages.compose_us / stages.frames, stages.normalize_us / stages.frames,
                            stages.damage_us / stages.frames, stages.render_us / stages.frames,
                            stages.total_us / stages.frames, stages.max_total_us, stages.frames_replaced);
        submit_stage_telemetry_.Reset();
    }
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_PERFORMANCE_TELEMETRY
    if (periodic) {
        esp_lv_adapter_display_telemetry_t telemetry{};
        if (esp_lv_adapter_display_telemetry_take(&telemetry)) {
            AppendTelemetryLine(
                "display telemetry (%" PRIu32 " scene frames): sw-image=%" PRIu64 "/%" PRIu64 "px/%" PRIu64
                "us fb-dma2d=%" PRIu64 "/%" PRIu64 "px/%" PRIu64 "us panel-submit=%" PRIu64 "/%" PRIu64 "B/%" PRIu64
                "us rgb565-byte-swap=%" PRIu64 "/%" PRIu64 "px/%" PRIu64 "us vsync-wait=%" PRIu64 "/%" PRIu64 "us",
                kSceneTelemetryPeriodFrames, telemetry.software_image_calls, telemetry.software_image_pixels,
                telemetry.software_image_elapsed_us, telemetry.framebuffer_dma2d_calls,
                telemetry.framebuffer_dma2d_pixels, telemetry.framebuffer_dma2d_elapsed_us,
                telemetry.panel_submit_calls, telemetry.panel_submit_bytes, telemetry.panel_submit_elapsed_us,
                telemetry.rgb565_wire_cpu_count, telemetry.rgb565_wire_cpu_pixels, telemetry.rgb565_wire_cpu_us,
                telemetry.panel_vsync_wait_calls, telemetry.panel_vsync_wait_elapsed_us);
        }
    }
#endif
    // The first frames are start-up diagnostics: written inline so none of
    // them is dropped behind an in-flight report.
    FlushTelemetryReport(sequence > 8U);
}

bool GuestGraphicsEngine::BeginTelemetryReport() {
    if (telemetry_report_.in_flight.load(std::memory_order_acquire)) {
        ++telemetry_report_.dropped;
        return false;
    }
    telemetry_report_.length = 0U;
    telemetry_report_.line_count = 0U;
    return true;
}

void GuestGraphicsEngine::AppendTelemetryLine(const char* format, ...) {
    TelemetryReport& report = telemetry_report_;
    if (report.line_count >= TelemetryReport::kMaxLines || report.length + 1U >= TelemetryReport::kCapacity) {
        return;
    }
    const std::size_t available = TelemetryReport::kCapacity - report.length;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(report.text.data() + report.length, available, format, arguments);
    va_end(arguments);
    if (written < 0) {
        return;
    }
    // A truncated line keeps whatever fit; vsnprintf always terminates it.
    const std::size_t line_length =
        static_cast<std::size_t>(written) < available - 1U ? static_cast<std::size_t>(written) : available - 1U;
    report.line_offsets[report.line_count++] = static_cast<uint16_t>(report.length);
    report.length += line_length + 1U;
}

void GuestGraphicsEngine::FlushTelemetryReport(bool defer) {
    TelemetryReport& report = telemetry_report_;
    if (report.dropped != 0U) {
        const uint32_t dropped = report.dropped;
        report.dropped = 0U;
        AppendTelemetryLine("telemetry: %" PRIu32 " report(s) dropped while the previous one was being written",
                            dropped);
    }
    if (report.line_count == 0U) {
        return;
    }
    if (defer && background_executor_ != nullptr) {
        report.in_flight.store(true, std::memory_order_release);
        if (background_executor_->Submit(&EmitTelemetryReportJob, this)) {
            return;
        }
        report.in_flight.store(false, std::memory_order_relaxed);
    }
    EmitTelemetryReport();
}

void GuestGraphicsEngine::EmitTelemetryReportJob(void* context) {
    static_cast<GuestGraphicsEngine*>(context)->EmitTelemetryReport();
}

void GuestGraphicsEngine::EmitTelemetryReport() {
    TelemetryReport& report = telemetry_report_;
    for (std::size_t index = 0U; index < report.line_count; ++index) {
        ESP_LOGI(kTag, "%s", report.text.data() + report.line_offsets[index]);
    }
    report.line_count = 0U;
    report.length = 0U;
    report.in_flight.store(false, std::memory_order_release);
}
#else
void GuestGraphicsEngine::LogSceneTelemetry(const graphics::AppSurfaceFrameResult& result, uint32_t sequence) {
    (void)result;
    (void)sequence;
    layer_snapshot_telemetry_active_ = result.layer_snapshot_used;
    if ((sequence % kSceneTelemetryPeriodFrames) == 0U) {
        submit_stage_telemetry_.Reset();
    }
}

bool GuestGraphicsEngine::BeginTelemetryReport() { return false; }
void GuestGraphicsEngine::AppendTelemetryLine(const char*, ...) {}
void GuestGraphicsEngine::FlushTelemetryReport(bool) {}
void GuestGraphicsEngine::EmitTelemetryReportJob(void*) {}
void GuestGraphicsEngine::EmitTelemetryReport() {}
#endif

void GuestGraphicsEngine::Release() {
    if (display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (guest_frame_ != nullptr) {
        lv_obj_delete(guest_frame_);
        guest_frame_ = nullptr;
        guest_refresh_pending_ = false;
        RequestDisplayRefresh(display_);
        ESP_LOGI(kTag, "Guest graphics tree released before Bitmap teardown");
    }
    ReleaseAppSurfaceLocked();
    ReleaseTextures(scene_texture_access_, scene_textures_, scene_texture_count_);
    scene_texture_count_ = 0U;
    scene_texture_access_ = {};
    ReleaseFonts(scene_fonts_, scene_font_count_);
    scene_font_count_ = 0U;
    bitmap_update_frame_active_ = false;
    bitmap_damage_.Clear();
    bitmap_frame_updates_ = 0U;
    bitmap_frame_bytes_ = 0U;
    heap_caps_free(texture_storage_);
    texture_storage_ = nullptr;
    scene_textures_ = nullptr;
    scratch_textures_ = nullptr;
    heap_caps_free(font_storage_);
    font_storage_ = nullptr;
    scene_fonts_ = nullptr;
    scratch_fonts_ = nullptr;
    guest_scene_.reset();
    heap_caps_free(guest_scene_node_storage_);
    guest_scene_node_storage_ = nullptr;
    heap_caps_free(guest_scene_instance_storage_);
    guest_scene_instance_storage_ = nullptr;
    heap_caps_free(guest_scene_container_storage_);
    guest_scene_container_storage_ = nullptr;
    heap_caps_free(guest_scene_draw_order_storage_);
    guest_scene_draw_order_storage_ = nullptr;
    fonts_.ReleaseGuestFonts();
    esp_lv_adapter_unlock();
}

int32_t GuestGraphicsEngine::Submit(const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures) {
    if (display_ == nullptr || bytes == nullptr || textures.resolve == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (!EnsureTextureStorage() || !EnsureSceneStorage()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    // Scene apply, resource retention, composition and publish all run on the
    // Guest task without the LVGL lock: the retained scene and the compose
    // target are private to this task, LVGL only reads the displayed surface,
    // and the hand-over is a spinlock-guarded mailbox. The Guest therefore
    // composes frame N while LVGL flushes frame N-1 and waits for vsync.
    // Release() runs on this same task after the session ends, so no other
    // task can free these buffers while a compose is in flight.
    const int64_t submit_started_us = esp_timer_get_time();
    const int32_t validation_status = guest_scene_->Apply(bytes, length, width_, height_, textures.resolve,
                                                          textures.context, ValidateFontHandle, this);
    const int64_t apply_finished_us = esp_timer_get_time();
    if (validation_status != MICROPIXEL_STATUS_OK) {
        ESP_LOGW(kTag, "rejected graphics scene: status=%" PRId32 " bytes=%" PRIu32, validation_status, length);
        return validation_status;
    }
    micropixel_graphics_scene_header_t wire_header{};
    std::memcpy(&wire_header, bytes, sizeof(wire_header));
    scene_wire_bytes_ = length;
    scene_wire_records_ = wire_header.record_count;
    scene_wire_instances_ = SceneWireInstanceCount(bytes, length);

    uint32_t texture_count = 0U;
    uint32_t font_count = 0U;
    for (uint16_t index = 0U; index < guest_scene_->NodeCount(); ++index) {
        const graphics::GuestSceneNode& node = guest_scene_->Nodes()[index];
        if (node.kind == graphics::GuestSceneNodeKind::kTexture ||
            (node.kind == graphics::GuestSceneNodeKind::kSpriteBatch && node.texture != 0U)) {
            bool exists = false;
            for (uint32_t current = 0U; current < texture_count; ++current) {
                exists = exists || scratch_textures_[current] == node.texture;
            }
            if (!exists) {
                scratch_textures_[texture_count++] = node.texture;
            }
        } else if (node.kind == graphics::GuestSceneNodeKind::kText) {
            bool exists = false;
            for (uint32_t current = 0U; current < font_count; ++current) {
                exists = exists || scratch_fonts_[current] == node.font;
            }
            if (!exists) {
                scratch_fonts_[font_count++] = node.font;
            }
        }
    }
    if (!RetainFonts(scratch_fonts_, font_count)) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    if (!RetainTextures(textures, scratch_textures_, texture_count)) {
        ReleaseFonts(scratch_fonts_, font_count);
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    if (!EnsureAppSurfaceStorage()) {
        ReleaseTextures(textures, scratch_textures_, texture_count);
        ReleaseFonts(scratch_fonts_, font_count);
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    // With a single surface LVGL reads the very buffer being composed, so the
    // whole compose has to stay under the lock and the frame is adopted inline.
    // Otherwise nothing here takes the LVGL lock: the compose target is a
    // surface LVGL is not reading and the publish is a lock-free mailbox.
    const bool compose_under_lock = ComposeUnderLock();
    graphics::AppSurfaceFrameResult frame{};
    int32_t execution_status = MICROPIXEL_STATUS_OK;
    bool locked = false;
    int64_t lock_requested_us = apply_finished_us;
    int64_t lock_acquired_us = apply_finished_us;
    if (compose_under_lock) {
        lock_requested_us = esp_timer_get_time();
        locked = esp_lv_adapter_lock(-1) == ESP_OK;
        lock_acquired_us = esp_timer_get_time();
        if (!locked) {
            execution_status = MICROPIXEL_STATUS_INTERNAL;
        }
    }
    const uint8_t target = AcquireComposeSurface();
    if (execution_status == MICROPIXEL_STATUS_OK) {
        execution_status = ComposeScene(textures, SurfaceAt(target), frame);
    }
    if (execution_status == MICROPIXEL_STATUS_OK) {
        PublishScene(textures, target, frame, scratch_textures_, texture_count, scratch_fonts_, font_count);
        if (locked) {
            AdoptPendingFrameLocked(false);
        }
    }
    if (locked) {
        esp_lv_adapter_unlock();
    }
    if (execution_status != MICROPIXEL_STATUS_OK) {
        ReleaseTextures(textures, scratch_textures_, texture_count);
        ReleaseFonts(scratch_fonts_, font_count);
    }
    const int64_t submit_finished_us = esp_timer_get_time();
    SubmitStageTelemetry& stages = submit_stage_telemetry_;
    ++stages.frames;
    stages.lock_wait_us += static_cast<uint64_t>(lock_acquired_us - lock_requested_us);
    stages.apply_us += static_cast<uint64_t>(apply_finished_us - submit_started_us);
    const uint64_t total_us = static_cast<uint64_t>(submit_finished_us - submit_started_us);
    stages.total_us += total_us;
    stages.max_total_us = total_us > stages.max_total_us ? total_us : stages.max_total_us;
    return execution_status;
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

// Streaming bitmap updates run on the Guest task. The bitmap storage and the
// frame bookkeeping are private to that task, so only the single-surface
// configuration needs the LVGL lock (the compose target is the surface LVGL
// reads), in which case the published frame is adopted inline.
bool GuestGraphicsEngine::PresentBitmapDamage(const graphics::DamageRegion* regions, size_t count) {
    const bool lock = ComposeUnderLock();
    if (lock && esp_lv_adapter_lock(-1) != ESP_OK) {
        return false;
    }
    bool invalidated = false;
    for (size_t index = 0U; index < count; ++index) {
        invalidated =
            RefreshAppSurfaceBitmap(static_cast<const uint8_t*>(regions[index].source), regions[index].rect) ||
            invalidated;
    }
    if (lock) {
        AdoptPendingFrameLocked(false);
        esp_lv_adapter_unlock();
    }
    return invalidated;
}

int32_t GuestGraphicsEngine::BeginBitmapUpdateFrame() {
    if (display_ == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (bitmap_update_frame_active_) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    bitmap_update_frame_active_ = true;
    bitmap_damage_.Clear();
    bitmap_frame_updates_ = 0U;
    bitmap_frame_bytes_ = 0U;
    bitmap_frame_started_us_ = static_cast<uint64_t>(esp_timer_get_time());
    return MICROPIXEL_STATUS_OK;
}

int32_t GuestGraphicsEngine::UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                          uint32_t height, const uint8_t* pixels, uint32_t stride) {
    const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888
                                         ? 3U
                                         : (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888
                                                ? 4U
                                                : (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565 ? 2U : 0U));
    if (display_ == nullptr || bitmap.data == nullptr || pixels == nullptr || bytes_per_pixel == 0U ||
        (bitmap.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) == 0U || width == 0U || height == 0U ||
        static_cast<uint64_t>(x) + width > bitmap.width || static_cast<uint64_t>(y) + height > bitmap.height ||
        stride != width * bytes_per_pixel || bitmap.stride != bitmap.width * bytes_per_pixel ||
        bitmap.size != bitmap.stride * bitmap.height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (bitmap_update_frame_active_) {
        constexpr graphics::DamageMergePolicy kDamageMergePolicy{
            .max_extra_pixels = CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_EXTRA_PIXELS,
            .max_region_pixels = CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_MAX_PIXELS,
        };
        if (!bitmap_damage_.Add(bitmap.data, {.x = x, .y = y, .width = width, .height = height}, kDamageMergePolicy)) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
    }
    // The compositor only reads bitmaps while composing on this same task, so
    // the copy needs no synchronization against LVGL.
    auto* destination = const_cast<uint8_t*>(bitmap.data) + y * bitmap.stride + x * bytes_per_pixel;
    for (uint32_t row = 0U; row < height; ++row) {
        std::memcpy(destination + row * bitmap.stride, pixels + row * stride, stride);
    }
    if (bitmap_update_frame_active_) {
        ++bitmap_frame_updates_;
        bitmap_frame_bytes_ += static_cast<uint64_t>(stride) * height;
    } else {
        const graphics::DamageRegion region{.source = bitmap.data,
                                            .rect = {.x = x, .y = y, .width = width, .height = height}};
        (void)PresentBitmapDamage(&region, 1U);
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t GuestGraphicsEngine::CommitBitmapUpdateFrame() {
    if (display_ == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (!bitmap_update_frame_active_) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    const bool invalidated = PresentBitmapDamage(&bitmap_damage_[0], bitmap_damage_.Size());

    const uint32_t updates = bitmap_frame_updates_;
    const uint64_t bytes = bitmap_frame_bytes_;
    const size_t damage_count = bitmap_damage_.Size();
    const uint32_t capacity_merges = bitmap_damage_.CapacityMergeCount();
    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t elapsed_us = now_us >= bitmap_frame_started_us_ ? now_us - bitmap_frame_started_us_ : 0U;
    const uint32_t sequence = updates == 0U ? bitmap_frame_sequence_ : ++bitmap_frame_sequence_;
    bitmap_update_frame_active_ = false;
    bitmap_damage_.Clear();
    bitmap_frame_updates_ = 0U;
    bitmap_frame_bytes_ = 0U;

    if (updates != 0U && (sequence <= 8U || (sequence % kSceneTelemetryPeriodFrames) == 0U)) {
        ESP_LOGI(kTag,
                 "offscreen frame #%" PRIu32 ": updates=%" PRIu32 " bytes=%" PRIu64 " regions=%" PRIu32
                 " capacity-merges=%" PRIu32 " stage=%" PRIu64 " us present=%s",
                 sequence, updates, bytes, static_cast<uint32_t>(damage_count), capacity_merges, elapsed_us,
                 invalidated ? "yes" : "no");
    }
    return MICROPIXEL_STATUS_OK;
}

bool GuestGraphicsEngine::ScaleBitmapSoftware(const device::BitmapView& source, const device::BitmapView& destination) {
    return software_pixel_compositor_.ScaleBitmap(source, destination);
}

}  // namespace micropixel::platform::lvgl
