#ifndef MICROPIXEL_APPS_MAZE_BREAK_GAME_RENDERER_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GAME_RENDERER_HPP

#include <stdint.h>

#include "apps/maze-break/game/world.hpp"
#include "apps/maze-break/gfx/sprites.hpp"
#include "apps/maze-break/gfx/textures.hpp"
#include "apps/maze-break/gfx/view_config.hpp"
#include "sdk/graphics.hpp"

namespace maze_break::game {

// Per-frame timing shown in the corner when the performance overlay is on.
// Values are tenths of a millisecond so the HUD needs no float formatting.
struct HudStats {
    uint32_t fps{};
    uint32_t render_ms_x10{};   // geometry + record encoding + Host rasterization
    uint32_t present_ms_x10{};  // SURFACE_PRESENT service call
    uint32_t wait_ms_x10{};     // blocked waiting for a free buffer
    bool show_perf{};
    bool visible{true};  // Hide gameplay labels beneath the start-screen diagrams.
};

// Raycaster front end for the Graphics 1.6 Host raster kernels. The geometry
// (ray DDA, floor row setup, billboard sort and z-test) runs here; every pixel
// is written by the Host from the records this class appends to a
// RasterDrawList: SPAN_PAIR rows for floor and ceiling, COLUMN runs for walls
// and things, SPRITE records for the weapon and HUD glyphs and RECT fills for
// bars, the crosshair and the damage tint. The App never maps the frame.
class Renderer {
   public:
    // `view` must describe a buffer no larger than kMaxViewWidth x kMaxViewHeight.
    void Initialize(const gfx::ViewConfig& view);
    // Uploads textures, sprites, the glyph atlas and the lit palette to the
    // Host raster kernels. Palette, textures and sprites must already be
    // built. Returns false when any upload is refused; the renderer is then
    // unusable.
    [[nodiscard]] bool UploadResources(const micropixel::SurfaceRaster& raster);
    // Appends the frame's records to `list` (open on the target buffer). The
    // list is left open for the caller's overlays and Finish(). Returns false
    // when a record could not be encoded.
    [[nodiscard]] bool Render(micropixel::RasterDrawList& list, const World& world, const HudStats& hud);

    // Text in the HUD glyph set, one SOLID_COLOR sprite per glyph.
    [[nodiscard]] bool DrawText(micropixel::RasterDrawList& list, int x, int y, const char* text, uint16_t color,
                                int scale) const;
    // Filled disc or 2 px wide ring, as one RECT per row.
    [[nodiscard]] bool DrawCircle(micropixel::RasterDrawList& list, int cx, int cy, int radius, uint16_t color,
                                  bool filled) const;

    [[nodiscard]] const gfx::ViewConfig& view() const { return view_; }

   private:
    // Host raster texture slots: textures occupy their TextureId, sprites
    // follow (world sprites and the weapon), then the glyph atlas.
    static constexpr uint8_t kSpriteSlotBase = gfx::kTexCount;
    static constexpr uint8_t kGlyphSlot = kSpriteSlotBase + gfx::kSprCount;
    static constexpr int kSlotCount = kGlyphSlot + 1;

    // One textured wall (or door slab) column produced by the ray cast and
    // painted after the floor, so the floor pass can skip what it covers.
    struct WallSlice {
        int16_t y0{};
        int16_t y1{-1};  // y1 < y0: nothing to paint
        uint16_t tex_x{};
        int32_t v_start{};
        int32_t v_step{};
        gfx::TextureId texture{};
        uint8_t light{};
    };

    // Ray cast: fills walls_/doors_/zbuffer_ and the per-column wall coverage
    // used by DrawFloorAndCeiling. Paints nothing.
    void CastWalls(const World& world);
    [[nodiscard]] bool DrawFloorAndCeiling(micropixel::RasterDrawList& list, const Player& player);
    [[nodiscard]] bool DrawWalls(micropixel::RasterDrawList& list);
    [[nodiscard]] bool DrawThings(micropixel::RasterDrawList& list, const World& world);
    [[nodiscard]] bool DrawWeapon(micropixel::RasterDrawList& list, const World& world);
    [[nodiscard]] bool DrawDamageTint(micropixel::RasterDrawList& list, const World& world);
    [[nodiscard]] bool DrawHud(micropixel::RasterDrawList& list, const World& world, const HudStats& hud);
    // Weapon-class sprite scaled by an integer factor at full brightness.
    [[nodiscard]] bool BlitSprite(micropixel::RasterDrawList& list, gfx::SpriteId id, int x, int y, int scale) const;
    [[nodiscard]] int LightFor(float distance) const;

    gfx::ViewConfig view_{};
    int half_height_{};
    int hud_height_{};
    float zbuffer_[gfx::kMaxViewWidth]{};
    WallSlice walls_[gfx::kMaxViewWidth]{};
    WallSlice doors_[gfx::kMaxViewWidth]{};
    // Rows [cover_top_, cover_bottom_] of each column are hidden by the far
    // wall; floor and ceiling pixels inside that range are never painted.
    int16_t cover_top_[gfx::kMaxViewWidth]{};
    int16_t cover_bottom_[gfx::kMaxViewWidth]{};
    // Per kCoverBlock columns: min/max of cover_bottom_ and max of cover_top_.
    static constexpr int kCoverBlock = 16;
    static constexpr int kCoverBlocks = (gfx::kMaxViewWidth + kCoverBlock - 1) / kCoverBlock;
    int16_t block_bottom_min_[kCoverBlocks]{};
    int16_t block_bottom_max_[kCoverBlocks]{};
    int16_t block_top_max_[kCoverBlocks]{};
    uint8_t light_lut_[512]{};
    Thing things_[World::kMaxThings]{};
    float thing_depth_[World::kMaxThings]{};
    uint8_t order_[World::kMaxThings]{};
};

}  // namespace maze_break::game

#endif
