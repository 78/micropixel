#ifndef MICROPIXEL_APPS_MAZE_BREAK_GAME_RENDERER_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GAME_RENDERER_HPP

#include <stdint.h>

#include "apps/maze-evil/game/world.hpp"
#include "apps/maze-evil/gfx/sprites.hpp"
#include "apps/maze-evil/gfx/textures.hpp"
#include "apps/maze-evil/gfx/view_config.hpp"
#include "sdk/graphics.hpp"
#include "sdk/raycast.hpp"

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

// Frame front end over the SDK Raycaster and the Graphics 1.6 Host raster
// kernels. The Raycaster casts the level (SPAN_PAIR rows for floor and
// ceiling, COLUMN runs for walls, doors and things); this class owns the
// resource upload, the weapon, the damage tint and the HUD (SPRITE records for
// glyphs, RECT fills for bars and the crosshair). The App never maps the frame.
class Renderer {
   public:
    // `view` must describe a buffer no larger than kMaxViewWidth x kMaxViewHeight.
    void Initialize(const gfx::ViewConfig& view);
    // Uploads textures, sprites, the glyph atlas and the lit palette to the
    // Host raster kernels. The lighting palette must already be built; art
    // pixels are immutable. Returns false when any upload is refused.
    [[nodiscard]] bool UploadResources(const micropixel::RasterResources& raster);
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

    // Things become camera-facing billboards clipped by the Raycaster's
    // per-column depth.
    [[nodiscard]] bool DrawThings(micropixel::RasterDrawList& list, const World& world);
    [[nodiscard]] bool DrawWeapon(micropixel::RasterDrawList& list, const World& world);
    [[nodiscard]] bool DrawDamageTint(micropixel::RasterDrawList& list, const World& world);
    [[nodiscard]] bool DrawHud(micropixel::RasterDrawList& list, const World& world, const HudStats& hud);
    // Weapon-class sprite scaled by an integer factor at full brightness.
    [[nodiscard]] bool BlitSprite(micropixel::RasterDrawList& list, gfx::SpriteId id, int x, int y, int scale) const;

    gfx::ViewConfig view_{};
    int half_height_{};
    int hud_height_{};
    micropixel::Raycaster caster_{};
    Thing things_[World::kMaxThings]{};
    micropixel::Billboard billboards_[World::kMaxThings]{};
};
static_assert(World::kMaxThings <= micropixel::Raycaster::kMaxBillboards,
              "every World thing must fit one Raycaster billboard pass");
static_assert(gfx::kMaxViewWidth <= micropixel::Raycaster::kMaxColumns,
              "the App's view bound must not exceed the Raycaster column capacity");

}  // namespace maze_break::game

#endif
