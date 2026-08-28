#pragma once

#include <cstdint>

#include "device/graphics.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "platform/lvgl/display/dirty_region_coalescer.hpp"
#include "platform/lvgl/display/display_pipeline.hpp"
#include "platform/lvgl/display/retained_scene.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"

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
    GuestGraphicsEngine(int32_t width, int32_t height, FontRegistry& fonts);

    void SetPresentationHooks(GuestPresentationHooks hooks) { presentation_hooks_ = hooks; }
    [[nodiscard]] esp_err_t Initialize(lv_display_t* display, DirectFramebufferAccess* framebuffers);
    void RebindFramebuffers(DirectFramebufferAccess* framebuffers);

    [[nodiscard]] bool Available() const { return true; }
    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t& info) const;
    [[nodiscard]] int32_t BeginFrame();
    [[nodiscard]] int32_t Submit(const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures);
    [[nodiscard]] int32_t CommitFrame(const device::TextureAccess& textures);
    [[nodiscard]] int32_t CancelFrame();
    [[nodiscard]] int32_t LoadFont(const device::FontResourceView& resource, micropixel_font_info_t& info_out);
    [[nodiscard]] int32_t ReleaseFont(micropixel_font_handle_t font);
    [[nodiscard]] int32_t MeasureText(micropixel_font_handle_t font, const char* text, uint32_t text_length,
                                      micropixel_text_metrics_t& metrics_out);
    [[nodiscard]] int32_t BeginBitmapUpdateFrame();
    [[nodiscard]] int32_t UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height, const uint8_t* pixels, uint32_t stride);
    [[nodiscard]] int32_t CommitBitmapUpdateFrame();
    void Release();

    [[nodiscard]] lv_obj_t* FrameLocked() const { return guest_frame_; }
    [[nodiscard]] uint32_t DisplayRefreshSequence() const { return display_refresh_sequence_; }
    [[nodiscard]] bool RefreshSynchronizationAvailable() const { return display_refresh_ready_ != nullptr; }
    void DrainRefreshReady();
    void WaitForRefreshReady();

   private:
    static void DisplayRefreshStartEvent(lv_event_t* event);
    static void DisplayRefreshReadyEvent(lv_event_t* event);
    static bool ValidateFontHandle(void* context, micropixel_font_handle_t font);

    [[nodiscard]] int32_t ApplyFrameLocked(const uint8_t* bytes, uint32_t length, const device::TextureAccess& textures,
                                           const micropixel_texture_handle_t* retained_textures,
                                           uint32_t retained_count);
    [[nodiscard]] bool EnsureTextureStorage();
    void ClearPendingFrameTextures(bool release);
    [[nodiscard]] bool AddPendingFrameTextures(const micropixel_texture_handle_t* textures, uint32_t count,
                                               const device::TextureAccess& access);

    static constexpr uint32_t kBitmapDamageCapacity = 16U;
    static constexpr uint32_t kMaxSceneTextures = MICROPIXEL_GRAPHICS_MAX_DRAW_OPERATIONS;

    struct BitmapDamage final {
        const uint8_t* data{};
        uint32_t x{};
        uint32_t y{};
        uint32_t width{};
        uint32_t height{};
    };

    [[nodiscard]] static bool AccumulateDamage(BitmapDamage* damages, uint32_t capacity, uint32_t& damage_count,
                                               const uint8_t* data, uint32_t x, uint32_t y, uint32_t width,
                                               uint32_t height);

    int32_t width_{};
    int32_t height_{};
    lv_display_t* display_{};
    lv_obj_t* guest_frame_{};
    DirtyRegionCoalescer dirty_region_coalescer_{};
    FontRegistry& fonts_;
    RetainedScene retained_scene_;
    GuestPresentationHooks presentation_hooks_{};
    BitmapDamage bitmap_damage_[kBitmapDamageCapacity]{};
    uint64_t bitmap_frame_started_us_{};
    uint64_t bitmap_frame_bytes_{};
    uint32_t bitmap_frame_updates_{};
    uint32_t bitmap_frame_sequence_{};
    uint32_t bitmap_damage_count_{};
    uint8_t* graphics_frame_bytes_{};
    uint32_t graphics_frame_length_{};
    uint32_t graphics_frame_commands_{};
    micropixel_texture_handle_t* texture_storage_{};
    micropixel_texture_handle_t* graphics_frame_textures_{};
    micropixel_texture_handle_t* scene_textures_{};
    micropixel_texture_handle_t* scratch_textures_{};
    uint32_t graphics_frame_texture_count_{};
    device::TextureAccess graphics_frame_texture_access_{};
    uint32_t scene_texture_count_{};
    device::TextureAccess scene_texture_access_{};
    uint32_t graphics_frame_sequence_{};
    uint32_t display_refresh_sequence_{};
    int64_t display_refresh_started_us_{};
    StaticSemaphore_t display_refresh_ready_storage_{};
    SemaphoreHandle_t display_refresh_ready_{};
    bool bitmap_update_frame_active_{};
    bool graphics_frame_active_{};
};

}  // namespace micropixel::platform::lvgl
