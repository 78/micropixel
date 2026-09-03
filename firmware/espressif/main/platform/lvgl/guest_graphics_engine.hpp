#pragma once

#include <array>
#include <atomic>
#include <cstddef>
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

namespace micropixel::work {
class BackgroundExecutor;
}  // namespace micropixel::work

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
    // Periodic telemetry text is written to the console from this executor
    // instead of the Guest task. Without one the text is logged inline.
    void BindBackgroundExecutor(work::BackgroundExecutor& executor) { background_executor_ = &executor; }
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

    static void PublishTimerCallback(lv_timer_t* timer);

    // ---- Surface rotation and lock-free publish -----------------------------
    //
    // The Guest task composes into a surface LVGL is not reading and hands it
    // over through a single-slot mailbox (pending_) guarded by a spinlock. The
    // LVGL task adopts the mailbox at the start of its refresh (or from the
    // publish timer): it retargets the image descriptor, invalidates the
    // accumulated damage and runs the Host presentation hook. The Guest never
    // takes the LVGL lock on this path, so it is not serialized behind the
    // panel flush / vsync wait that the LVGL task performs while holding it.
    //
    // Ownership:
    //   Guest task only ....... retained scene, compositor, non-displayed
    //                           surfaces, texture/font retention, telemetry;
    //   LVGL task only ........ guest_frame_, app_surface_image_, descriptor,
    //                           app_surface_active_, displayed_surface_ writes;
    //   spinlock .............. pending_, displayed_surface_ reads by Guest.
    //
    // With a single surface LVGL reads the very buffer being composed, so the
    // whole Submit runs under the LVGL lock and adopts inline.
    static constexpr uint8_t kNoSurface = UINT8_MAX;
    static constexpr uint32_t kPendingDamageCapacity = graphics::AppSurfaceCompositor::kMaxDamageRegions;

    struct PendingFrame final {
        uint8_t surface{kNoSurface};
        // Union of the content damage of every frame folded into this mailbox
        // entry, in App Surface coordinates. overflow means "invalidate all".
        graphics::DamageRegionSet<kPendingDamageCapacity> damage{};
        bool damage_overflow{};
        bool visual_changed{};
    };

    // Picks the surface the next compose may write to. Never returns the
    // displayed surface; with two surfaces it may take back the pending one
    // (that frame is then superseded rather than shown).
    [[nodiscard]] uint8_t AcquireComposeSurface();
    // Hands a composed surface to the LVGL task and wakes it.
    void PublishSurface(uint8_t surface, bool visual_changed);
    // LVGL task (or any caller holding the LVGL lock): consumes the mailbox if
    // it holds a frame. `inside_refresh` is set from LV_EVENT_REFR_START, where
    // the dirty areas are picked up by the running refresh.
    void AdoptPendingFrameLocked(bool inside_refresh);
    void ClearPendingFrame();

    // Composes the retained scene into `target`. Runs without the LVGL lock
    // when the target is not the displayed surface.
    [[nodiscard]] int32_t ComposeScene(const device::TextureAccess& textures, graphics::PixelSurface target,
                                       graphics::AppSurfaceFrameResult& result);
    // Guest task: swaps in the retained resources of the newly composed frame
    // and publishes the surface.
    void PublishScene(const device::TextureAccess& textures, uint8_t surface,
                      const graphics::AppSurfaceFrameResult& result,
                      const micropixel_texture_handle_t* retained_textures, uint32_t retained_texture_count,
                      const micropixel_font_handle_t* retained_fonts, uint32_t retained_font_count);
    void LogSceneTelemetry(const graphics::AppSurfaceFrameResult& result, uint32_t sequence);

    // ---- Telemetry hand-off ---------------------------------------------------
    //
    // The console is a blocking 115200-baud UART: the periodic scene summary is
    // ~1.5 KiB, i.e. >100 ms if written from the Guest task. The producer
    // formats every line into `telemetry_report_` and submits one job to the
    // background executor, which writes the lines out and releases the buffer.
    // `in_flight` is the ownership token: the Guest task only writes while it
    // is false; the executor sets it back to false after the last line. A
    // window whose predecessor is still in flight is dropped and counted.
    struct TelemetryReport final {
        static constexpr std::size_t kCapacity = 2048U;
        static constexpr std::size_t kMaxLines = 6U;
        std::array<char, kCapacity> text{};
        std::array<uint16_t, kMaxLines> line_offsets{};
        std::size_t length{};
        std::size_t line_count{};
        std::atomic<bool> in_flight{};
        uint32_t dropped{};
    };
    // Returns false when the previous report has not been written out yet.
    [[nodiscard]] bool BeginTelemetryReport();
    void AppendTelemetryLine(const char* format, ...) __attribute__((format(printf, 2, 3)));
    // Hands the report to the executor when `defer` is set and one is bound;
    // otherwise logs it inline.
    void FlushTelemetryReport(bool defer);
    static void EmitTelemetryReportJob(void* context);
    void EmitTelemetryReport();
    [[nodiscard]] bool EnsureTextureStorage();
    [[nodiscard]] bool EnsureSceneStorage();
    void ReleaseFonts(const micropixel_font_handle_t* fonts, uint32_t count);
    [[nodiscard]] bool RetainFonts(const micropixel_font_handle_t* fonts, uint32_t count);
    [[nodiscard]] bool EnsureAppSurfaceStorage();
    [[nodiscard]] graphics::PixelSurface SurfaceAt(uint8_t index) const;
    [[nodiscard]] bool ComposeUnderLock() const { return app_surface_count_ < 2U; }
    void ShowAppSurfaceLocked();
    void HideAppSurfaceLocked();
    // Guest task: composes a streaming bitmap change into a free surface and
    // publishes it. Returns whether anything visible changed.
    [[nodiscard]] bool RefreshAppSurfaceBitmap(const uint8_t* bitmap_data, graphics::DamageRect damage);
    // Refreshes every region and, with a single surface, adopts inline under
    // the LVGL lock. Returns whether anything visible changed.
    [[nodiscard]] bool PresentBitmapDamage(const graphics::DamageRegion* regions, size_t count);
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
    // 1..kMaxSurfaces slots inside app_surface_pixels_ (each slot is
    // app_surface_allocation_bytes_), followed by the Layer cache slot.
    uint8_t app_surface_count_{1U};
    // Surface the LVGL image currently reads. Written by the LVGL task while
    // adopting, read by the Guest under publish_lock_ to pick a compose target.
    uint8_t displayed_surface_{};
    PendingFrame pending_{};
    portMUX_TYPE publish_lock_ = portMUX_INITIALIZER_UNLOCKED;
    // Long-period LVGL timer the Guest marks ready after publishing so the
    // LVGL task wakes and adopts even while its refresh timer is paused.
    lv_timer_t* publish_timer_{};
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
    // Per-stage Submit timings accumulated over the periodic scene log window.
    struct SubmitStageTelemetry final {
        uint32_t frames{};
        uint64_t lock_wait_us{};
        uint64_t apply_us{};
        uint64_t compose_us{};
        uint64_t normalize_us{};
        uint64_t damage_us{};
        uint64_t render_us{};
        uint64_t total_us{};
        uint64_t max_total_us{};
        // Published frames superseded in the mailbox before LVGL adopted them.
        uint32_t frames_replaced{};

        void Reset() { *this = {}; }
    };
    SubmitStageTelemetry submit_stage_telemetry_{};
    TelemetryReport telemetry_report_{};
    work::BackgroundExecutor* background_executor_{};
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
