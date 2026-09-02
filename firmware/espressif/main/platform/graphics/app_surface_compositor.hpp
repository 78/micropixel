#pragma once

#include <cstddef>
#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "platform/graphics/damage_region_set.hpp"
#include "platform/graphics/guest_scene.hpp"
#include "platform/graphics/pixel_compositor.hpp"

namespace micropixel::platform::graphics {

enum class AppDrawOperationKind : uint8_t {
    kFill,
    kRoundedRect,
    kTexture,
    kText,
};

struct RasterTextMetrics final {
    uint32_t width{};
    uint32_t height{};
};

// Hardware-neutral text boundary. Implementations rasterize directly into the
// App Surface; they do not create LVGL Guest objects or participate in Host UI
// composition.
class TextRasterizer {
   public:
    virtual ~TextRasterizer() = default;
    TextRasterizer(const TextRasterizer&) = delete;
    TextRasterizer& operator=(const TextRasterizer&) = delete;

    [[nodiscard]] virtual bool Measure(micropixel_font_handle_t font, const char* text, uint16_t text_length,
                                       RasterTextMetrics& metrics) const = 0;
    [[nodiscard]] virtual bool Draw(PixelSurface destination, int32_t x, int32_t y, uint32_t rgb888,
                                    micropixel_font_handle_t font, const char* text, uint16_t text_length) const = 0;

   protected:
    TextRasterizer() = default;
};

// One normalized retained draw slot. Storage is supplied by the owner so the
// compositor never allocates while parsing or presenting a Guest frame.
struct AppDrawOperation final {
    AppDrawOperationKind kind{};
    bool visible{};
    bool in_layer{};
    int16_t z_order{};
    uint16_t stable_order{};
    SurfaceRect destination{};
    SurfaceRect bounds{};
    uint32_t rgb888{};
    uint32_t stroke_rgb888{};
    uint32_t radius{};
    uint32_t stroke_width{};
    uint8_t opacity{};
    micropixel_texture_handle_t texture{};
    SurfaceRect source{};
    device::BitmapView bitmap{};
    micropixel_font_handle_t font{};
    uint16_t text_length{};
    char text[MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES + 1U]{};
};

struct AppLayerState final {
    bool valid{};
    bool translation_active{};
    uint8_t layer_id{};
    SurfaceRect clip{};
    int32_t translate_x{};
    int32_t translate_y{};
};

enum class AppSurfaceStatus : uint8_t {
    kOk,
    kUnsupported,
    kInvalidArgument,
    kResourceExhausted,
    kRenderFailed,
};

struct AppSurfaceFrameResult final {
    AppSurfaceStatus status{AppSurfaceStatus::kInvalidArgument};
    bool visual_changed{};
    bool layer_snapshot_used{};
    uint32_t damage_region_count{};
    uint32_t capacity_merge_count{};
    uint64_t damage_pixels{};
    uint32_t draw_operations_replayed{};
    uint32_t operations_normalized{};
    bool incremental_normalization{};
};

// Retains the authoritative Guest scene in fixed slots and updates a persistent
// opaque App Surface. A Present only restores and replays changed regions.
// A retained-translation scope is captured once into an optional Layer cache;
// subsequent shake frames restore the old location and move that snapshot.
class AppSurfaceCompositor final {
   public:
    static constexpr size_t kMaxDamageRegions = 16U;

    AppSurfaceCompositor(AppDrawOperation* first_scene, AppDrawOperation* second_scene, uint32_t operation_capacity,
                         PixelCompositor& pixels, DamageMergePolicy damage_policy, TextRasterizer* text = nullptr)
        : current_(first_scene),
          scratch_(second_scene),
          operation_capacity_(operation_capacity),
          pixels_(pixels),
          text_(text),
          damage_policy_(damage_policy) {}

    AppSurfaceCompositor(const AppSurfaceCompositor&) = delete;
    AppSurfaceCompositor& operator=(const AppSurfaceCompositor&) = delete;

    [[nodiscard]] AppSurfaceFrameResult PresentScene(const GuestScene& scene, PixelSurface destination,
                                                     device::BitmapResolver resolver, void* resolver_context);

    // Re-composites every current texture operation whose source window
    // intersects pixels changed in a StreamingTexture.
    [[nodiscard]] AppSurfaceFrameResult RefreshBitmap(const uint8_t* bitmap_data, DamageRect source_damage,
                                                      PixelSurface destination);

    void SetLayerCache(PixelSurface cache);
    void Reset();

    [[nodiscard]] size_t LastDamageCount() const { return damage_.Size(); }
    [[nodiscard]] DamageRect LastDamage(size_t index) const { return damage_[index].rect; }
    [[nodiscard]] uint32_t CurrentOperationCount() const { return current_count_; }

   private:
    [[nodiscard]] AppSurfaceStatus NormalizeScene(const GuestScene& scene, device::BitmapResolver resolver,
                                                  void* resolver_context, uint32_t& operation_count,
                                                  AppLayerState& layer);
    [[nodiscard]] AppSurfaceStatus NormalizeScenePatch(const GuestScene& scene, device::BitmapResolver resolver,
                                                       void* resolver_context, uint32_t& operation_count,
                                                       AppLayerState& layer);
    [[nodiscard]] AppSurfaceStatus NormalizeOperation(const GuestScene& scene, const GuestSceneNode& node,
                                                      const GuestSceneSpriteInstance* instance, uint16_t stable_order,
                                                      const AppLayerState& layer, device::BitmapResolver resolver,
                                                      void* resolver_context, AppDrawOperation& operation) const;
    static void SortOperations(AppDrawOperation* operations, uint32_t operation_count);
    [[nodiscard]] AppSurfaceFrameResult PresentNormalized(uint32_t scratch_count, uint32_t scratch_background,
                                                          bool scratch_background_valid,
                                                          const AppLayerState& scratch_layer, PixelSurface destination);
    [[nodiscard]] bool AddDamage(PixelSurface destination, SurfaceRect rect);
    [[nodiscard]] bool RenderDamage(PixelSurface destination, const AppDrawOperation* operations,
                                    uint32_t operation_count, uint32_t background, const AppLayerState& layer,
                                    bool use_layer_snapshot, uint32_t& draw_operations_replayed);
    [[nodiscard]] bool CaptureLayer(PixelSurface destination, const AppLayerState& layer);
    [[nodiscard]] bool SameDestination(PixelSurface destination) const;
    void RememberDestination(PixelSurface destination);
    [[nodiscard]] AppSurfaceFrameResult Result(AppSurfaceStatus status, bool visual_changed,
                                               uint32_t draw_operations_replayed = 0U) const;

    AppDrawOperation* current_{};
    AppDrawOperation* scratch_{};
    uint32_t operation_capacity_{};
    PixelCompositor& pixels_;
    TextRasterizer* text_{};
    DamageMergePolicy damage_policy_{};
    DamageRegionSet<kMaxDamageRegions> damage_{};
    uint16_t stale_operation_indices_[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    uint16_t stable_to_sorted_index_[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    uint32_t current_count_{};
    uint32_t normalized_operations_{};
    uint16_t stale_operation_count_{};
    uint32_t background_rgb888_{};
    AppLayerState current_layer_{};
    PixelSurface layer_cache_{};
    bool layer_snapshot_active_{};
    bool background_valid_{};
    bool synchronized_{};
    bool scratch_synchronized_{};
    bool incremental_normalization_{};
    uint8_t* destination_pixels_{};
    uint32_t destination_size_{};
    uint32_t destination_width_{};
    uint32_t destination_height_{};
    uint32_t destination_stride_{};
    SurfacePixelFormat destination_format_{};
    uint32_t destination_origin_x_{};
    uint32_t destination_origin_y_{};
};

}  // namespace micropixel::platform::graphics
