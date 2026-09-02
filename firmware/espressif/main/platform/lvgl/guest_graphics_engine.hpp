#pragma once

#include <cstdint>
#include <optional>

#include "device/contracts/graphics.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "platform/graphics/app_surface_compositor.hpp"
#include "platform/graphics/damage_region_set.hpp"
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
#include "platform/graphics/esp_pixel_compositor.hpp"
#endif
#include "platform/graphics/guest_scene.hpp"
#include "platform/lvgl/display/dirty_region_coalescer.hpp"
#include "platform/lvgl/display/display_pipeline.hpp"
#include "platform/lvgl/fonts/bitmap_font_rasterizer.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_software_pixel_compositor.hpp"

namespace micropixel::platform::lvgl {

// The Guest renderer occasionally needs to coordinate z-order with the Host
// shell, but it does not own the Host UI. These hooks keep that dependency
// explicit and prevent the renderer from reaching into the platform state.
struct GuestPresentationHooks final {
    void* context{};
    void (*prepare_frame_locked)(void*, lv_obj_t* guest_frame, bool created_guest_frame, bool& needs_present){};
};

class GuestGraphicsEngine final {
   public:
    GuestGraphicsEngine(int32_t width, int32_t height, FontRegistry& fonts,
                        graphics::SurfacePixelFormat app_surface_format = graphics::SurfacePixelFormat::kBgr888);

    void SetPresentationHooks(GuestPresentationHooks hooks) { presentation_hooks_ = hooks; }
    [[nodiscard]] esp_err_t Initialize(lv_display_t* display, DirectFramebufferAccess* framebuffers);
    void RebindFramebuffers(DirectFramebufferAccess* framebuffers);

    [[nodiscard]] bool Available() const { return true; }
    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t& info) const;
    [[nodiscard]] int32_t Submit(const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures);
    [[nodiscard]] int32_t LoadFont(const device::FontResourceView& resource, micropixel_font_info_t& info_out);
    [[nodiscard]] int32_t ReleaseFont(micropixel_font_handle_t font);
    [[nodiscard]] int32_t MeasureText(micropixel_font_handle_t font, const char* text, uint32_t text_length,
                                      micropixel_text_metrics_t& metrics_out);
    [[nodiscard]] int32_t BeginBitmapUpdateFrame();
    [[nodiscard]] int32_t UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height, const uint8_t* pixels, uint32_t stride);
    [[nodiscard]] int32_t CommitBitmapUpdateFrame();
    [[nodiscard]] bool ScaleBitmapSoftware(const device::BitmapView& source, const device::BitmapView& destination);
    void Release();

    [[nodiscard]] lv_obj_t* FrameLocked() const { return guest_frame_; }
    [[nodiscard]] uint32_t GuestPresentedFrameSequence() const { return guest_presented_frame_sequence_; }
    [[nodiscard]] bool RefreshSynchronizationAvailable() const { return display_refresh_ready_ != nullptr; }
    void DrainRefreshReady();
    void WaitForRefreshReady();

   private:
    static void DisplayRefreshStartEvent(lv_event_t* event);
    static void DisplayRefreshReadyEvent(lv_event_t* event);
    static bool ValidateFontHandle(void* context, micropixel_font_handle_t font);

    [[nodiscard]] int32_t ApplySceneLocked(const device::TextureAccess& textures,
                                           const micropixel_texture_handle_t* retained_textures,
                                           uint32_t retained_texture_count,
                                           const micropixel_font_handle_t* retained_fonts,
                                           uint32_t retained_font_count);
    [[nodiscard]] bool EnsureTextureStorage();
    [[nodiscard]] bool EnsureSceneStorage();
    void ReleaseFonts(const micropixel_font_handle_t* fonts, uint32_t count);
    [[nodiscard]] bool RetainFonts(const micropixel_font_handle_t* fonts, uint32_t count);
    [[nodiscard]] bool EnsureAppSurfaceStorageLocked();
    [[nodiscard]] graphics::PixelSurface AppSurfacePixels() const;
    void ShowAppSurfaceLocked();
    void HideAppSurfaceLocked();
    void InvalidateAppSurfaceDamageLocked();
    [[nodiscard]] bool RefreshAppSurfaceBitmapLocked(const uint8_t* bitmap_data, graphics::DamageRect damage);
    void ReleaseAppSurfaceLocked();

    static constexpr uint32_t kBitmapDamageCapacity = 16U;
    static constexpr uint32_t kMaxSceneTextures = MICROPIXEL_GRAPHICS_MAX_SCENE_NODES;

    int32_t width_{};
    int32_t height_{};
    graphics::SurfacePixelFormat app_surface_format_{graphics::SurfacePixelFormat::kBgr888};
    lv_display_t* display_{};
    lv_obj_t* guest_frame_{};
    lv_obj_t* app_surface_image_{};
    lv_image_dsc_t app_surface_image_descriptor_{};
    uint8_t* app_surface_pixels_{};
    uint8_t* app_surface_layer_pixels_{};
    uint8_t* software_transform_scratch_{};
    uint32_t app_surface_pixel_bytes_{};
    uint32_t app_surface_allocation_bytes_{};
    uint32_t app_surface_stride_{};
    uint32_t software_transform_scratch_bytes_{};
    graphics::AppDrawOperation* app_surface_operation_storage_{};
    graphics::GuestSceneNode* guest_scene_node_storage_{};
    graphics::GuestSceneSpriteInstance* guest_scene_instance_storage_{};
    graphics::GuestSceneContainer* guest_scene_container_storage_{};
    uint16_t* guest_scene_draw_order_storage_{};
    LvglSoftwarePixelCompositor software_pixel_compositor_{};
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    graphics::EspPixelCompositor hardware_pixel_compositor_;
#endif
    std::optional<graphics::AppSurfaceCompositor> app_surface_compositor_{};
    std::optional<graphics::GuestScene> guest_scene_{};
    DirtyRegionCoalescer dirty_region_coalescer_{};
    DirtyRegionStats refresh_damage_{};
    FontRegistry& fonts_;
    BitmapFontRasterizer bitmap_font_rasterizer_;
    GuestPresentationHooks presentation_hooks_{};
    graphics::DamageRegionSet<kBitmapDamageCapacity> bitmap_damage_{};
    uint64_t bitmap_frame_started_us_{};
    uint64_t bitmap_frame_bytes_{};
    uint32_t bitmap_frame_updates_{};
    uint32_t bitmap_frame_sequence_{};
    micropixel_texture_handle_t* texture_storage_{};
    micropixel_texture_handle_t* scene_textures_{};
    micropixel_texture_handle_t* scratch_textures_{};
    micropixel_font_handle_t* font_storage_{};
    micropixel_font_handle_t* scene_fonts_{};
    micropixel_font_handle_t* scratch_fonts_{};
    uint32_t scene_texture_count_{};
    device::TextureAccess scene_texture_access_{};
    uint32_t scene_font_count_{};
    uint32_t app_surface_frame_sequence_{};
    uint32_t scene_wire_bytes_{};
    uint16_t scene_wire_records_{};
    uint16_t scene_wire_instances_{};
    bool layer_snapshot_telemetry_active_{};
    uint32_t display_refresh_sequence_{};
    uint32_t guest_presented_frame_sequence_{};
    int64_t display_refresh_started_us_{};
    StaticSemaphore_t display_refresh_ready_storage_{};
    SemaphoreHandle_t display_refresh_ready_{};
    bool bitmap_update_frame_active_{};
    bool app_surface_active_{};
    bool app_surface_allocation_failed_{};
    // A boolean deliberately coalesces any number of Guest updates into the
    // single LVGL refresh that actually presents them.
    bool guest_refresh_pending_{};
    bool guest_refresh_active_{};
};

}  // namespace micropixel::platform::lvgl
