#include "apps/maze-evil/game/renderer.hpp"

#include <span>

#include "apps/maze-evil/gfx/font.hpp"
#include "apps/maze-evil/gfx/palette.hpp"
#include "apps/maze-evil/gfx/textures.hpp"
#include "apps/maze-evil/rc_math.hpp"

namespace maze_break::game {
namespace {

using math::Clamp;
using micropixel::Color;
using micropixel::Rect;

// Minimal integer formatting; the Guest links no printf.
class TextBuilder final {
   public:
    void Append(const char* text) {
        while (*text != '\0' && size_ + 1 < kCapacity) {
            text_[size_++] = *text++;
        }
        text_[size_] = '\0';
    }

    void AppendInt(int value) {
        if (value < 0) {
            Append("-");
            value = -value;
        }
        char reversed[12];
        int count = 0;
        do {
            reversed[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && count < 11);
        while (count != 0 && size_ + 1 < kCapacity) {
            text_[size_++] = reversed[--count];
        }
        text_[size_] = '\0';
    }

    // Prints tenths as "12.3".
    void AppendTenths(uint32_t tenths) {
        AppendInt(static_cast<int>(tenths / 10U));
        Append(".");
        AppendInt(static_cast<int>(tenths % 10U));
    }

    [[nodiscard]] const char* c_str() const { return text_; }  // NOLINT(readability-identifier-naming)

   private:
    static constexpr int kCapacity = 64;
    char text_[kCapacity]{};
    int size_{};
};

}  // namespace

void Renderer::Initialize(const gfx::ViewConfig& view) {
    view_ = view;
    if (view_.width > micropixel::Raycaster::kMaxColumns) {
        view_.width = micropixel::Raycaster::kMaxColumns;
    }
    if (view_.height > gfx::kMaxViewHeight) {
        view_.height = gfx::kMaxViewHeight;
    }
    if (view_.hud_scale < 1) {
        view_.hud_scale = 1;
    }
    half_height_ = view_.height / 2;
    hud_height_ = 26 * view_.hud_scale;
    // Light falls off with distance. The 2.6 / 1.05 curve is unchanged; it is
    // clamped three steps below the HUD row so the near-field plateau is not
    // full-white (the old "flashlight" disc). Mid and far levels match the
    // original table once the raw curve drops below that cap. Y-facing walls
    // drop four levels.
    micropixel::RaycastConfig config{};
    config.width = view_.width;
    config.height = view_.height;
    config.texture_shift = gfx::kTextureShift;
    config.floor_slot = gfx::kTexFloor;
    config.ceiling_slot = gfx::kTexCeiling;
    config.lighting.levels = gfx::kLightLevels - 3;
    config.lighting.minimum = 2;
    config.lighting.side_shade = 4;
    config.lighting.curve_levels = 31.0F;
    config.lighting.full_distance = 2.6F;
    config.lighting.falloff = 1.05F;
    (void)caster_.Initialize(config);
}

namespace {

// Host raster textures are powers of two on both axes. World sprites use
// 32/64-pixel canvases; the 128x96 shotgun and 56x40 flash need padding.
constexpr int PadToPowerOfTwo(int value) {
    int padded = 8;
    while (padded < value) {
        padded <<= 1;
    }
    return padded;
}

constexpr int kMaxSpriteTexels = 128 * 128;
static_assert(gfx::kGlyphAtlasBytes <= kMaxSpriteTexels, "glyph atlas must fit the upload scratch");

}  // namespace

bool Renderer::UploadResources(const micropixel::RasterResources& raster) {
    // Wall textures are already column-major, floor and ceiling row-major.
    for (int id = 0; id < gfx::kTexCount; ++id) {
        const auto texture = static_cast<gfx::TextureId>(id);
        const micropixel::RasterLayout layout =
            gfx::ColumnMajor(texture) ? micropixel::RasterLayout::kColumnMajor : micropixel::RasterLayout::kRowMajor;
        if (!raster
                 .UploadTexture(static_cast<uint8_t>(id), gfx::kTextureSize, gfx::kTextureSize, layout,
                                gfx::TextureFor(texture))
                 .has_value()) {
            return false;
        }
    }
    // Sprites are stored row-major; the Host COLUMN and SPRITE kernels want
    // column-major, so transpose into a scratch texture padded with the
    // transparent index.
    static uint8_t scratch[kMaxSpriteTexels];
    for (int id = 0; id < gfx::kSprCount; ++id) {
        const gfx::Sprite& sprite = gfx::SpriteFor(static_cast<gfx::SpriteId>(id));
        const int padded_width = PadToPowerOfTwo(sprite.width);
        const int padded_height = PadToPowerOfTwo(sprite.height);
        if (padded_width * padded_height > kMaxSpriteTexels) {
            return false;
        }
        for (int u = 0; u < padded_width; ++u) {
            uint8_t* column = scratch + u * padded_height;
            for (int v = 0; v < padded_height; ++v) {
                column[v] = u < sprite.width && v < sprite.height ? sprite.At(u, v) : gfx::kTransparent;
            }
        }
        if (!raster
                 .UploadTexture(static_cast<uint8_t>(kSpriteSlotBase + id), static_cast<uint32_t>(padded_width),
                                static_cast<uint32_t>(padded_height), micropixel::RasterLayout::kColumnMajor,
                                std::span<const uint8_t>(scratch, static_cast<size_t>(padded_width * padded_height)))
                 .has_value()) {
            return false;
        }
    }
    gfx::BuildGlyphAtlas(scratch);
    if (!raster
             .UploadTexture(kGlyphSlot, gfx::kGlyphAtlasWidth, gfx::kGlyphAtlasHeight,
                            micropixel::RasterLayout::kColumnMajor,
                            std::span<const uint8_t>(scratch, gfx::kGlyphAtlasWidth * gfx::kGlyphAtlasHeight))
             .has_value()) {
        return false;
    }
    // The colormaps are exactly a lit palette: kLightLevels x 256 canonical
    // RGB565, contiguous from light 0.
    return raster
        .UploadLitPalette(0U, gfx::kLightLevels,
                          std::span<const uint16_t>(gfx::ColormapFor(0),
                                                    gfx::kLightLevels * micropixel::RasterResources::kPaletteEntries))
        .has_value();
}

bool Renderer::Render(micropixel::RasterDrawList& list, const World& world, const HudStats& hud) {
    // Cast first so the floor pass knows which rows the walls will cover;
    // records are executed in order: floor -> walls -> things -> overlays.
    const Player& p = world.player();
    const micropixel::RaycastCamera camera{p.x, p.y, p.dir_x, p.dir_y, p.plane_x, p.plane_y};
    caster_.Cast(camera, world.grid());
    return caster_.DrawWorld(list) && DrawThings(list, world) && DrawWeapon(list, world) &&
           DrawDamageTint(list, world) && (!hud.visible || DrawHud(list, world, hud));
}

bool Renderer::DrawThings(micropixel::RasterDrawList& list, const World& world) {
    const int count = world.CollectThings(things_, World::kMaxThings);
    for (int i = 0; i < count; ++i) {
        const Thing& thing = things_[i];
        const gfx::Sprite& sprite = gfx::SpriteFor(thing.sprite);
        billboards_[i] = micropixel::Billboard{
            .x = thing.x,
            .y = thing.y,
            .height = thing.height,
            .lift = thing.lift,
            .texture_slot = static_cast<uint8_t>(kSpriteSlotBase + thing.sprite),
            .texture_width = static_cast<uint16_t>(sprite.width),
            .texture_height = static_cast<uint16_t>(sprite.height),
            .self_lit = thing.sprite == gfx::kSprFireballA || thing.sprite == gfx::kSprFireballB ||
                        gfx::IsTorchSprite(thing.sprite),
        };
    }
    return caster_.DrawBillboards(list,
                                  std::span<const micropixel::Billboard>(billboards_, static_cast<size_t>(count)));
}

bool Renderer::BlitSprite(micropixel::RasterDrawList& list, gfx::SpriteId id, int x, int y, int scale) const {
    const gfx::Sprite& sprite = gfx::SpriteFor(id);
    return list.Sprite(Rect{x, y, sprite.width * scale, sprite.height * scale},
                       static_cast<uint8_t>(kSpriteSlotBase + id), gfx::kLightLevels - 1, 0U, 0U,
                       static_cast<uint16_t>(sprite.width), static_cast<uint16_t>(sprite.height));
}

bool Renderer::DrawWeapon(micropixel::RasterDrawList& list, const World& world) {
    if (world.phase() == Phase::kDead) {
        return true;
    }
    const int width = view_.width;
    const int height = view_.height;
    const int s = view_.hud_scale;
    const Player& p = world.player();
    const gfx::Sprite& gun = gfx::SpriteFor(gfx::kSprShotgun);
    const float bob_amount = p.speed > 0.2F ? 1.0F : 0.0F;
    const int bob_x = static_cast<int>(math::Sin(p.bob_phase) * 6.0F * bob_amount) * s;
    const int bob_y = static_cast<int>(math::Fabs(math::Cos(p.bob_phase)) * 4.0F * bob_amount) * s;
    const int recoil = static_cast<int>(p.recoil * 16.0F) * s;
    const int gun_scale = s;
    const int flash_scale = s;
    // The status bar hides the bottom hud_height_ rows; tuck the stock under it.
    // The sprite's barrel axis runs from (48, 2) to (64, 50). Project it
    // toward the crosshair instead of using a resolution-dependent side offset.
    const int resting_y = height - hud_height_ - gun.height * gun_scale + 8 * s;
    const int muzzle_y = resting_y + 2 * s;
    const int gun_x = width / 2 - 48 * s + (muzzle_y - height / 2) / 3 + bob_x;
    const int gun_y = resting_y + bob_y + recoil;
    bool ok = true;
    if (world.muzzle_flash()) {
        const gfx::Sprite& flash = gfx::SpriteFor(gfx::kSprMuzzleFlash);
        ok = BlitSprite(list, gfx::kSprMuzzleFlash, gun_x + 48 * s - flash.width * flash_scale / 2,
                        gun_y - flash.height * flash_scale + 6 * s, flash_scale);
    }
    return BlitSprite(list, gfx::kSprShotgun, gun_x, gun_y, gun_scale) && ok;
}

bool Renderer::DrawDamageTint(micropixel::RasterDrawList& list, const World& world) {
    if (world.player().damage_flash <= 0.0F && world.phase() != Phase::kDead) {
        return true;
    }
    // A red wash over the whole frame; death holds it, a hit fades it out.
    const float strength = world.phase() == Phase::kDead ? 0.55F : Clamp(world.player().damage_flash, 0.0F, 1.0F);
    const int alpha = Clamp(static_cast<int>(strength * 140.0F), 24, 140);
    return list.FillRect(Rect{0, 0, view_.width, view_.height}, Color::Rgb(220U, 16U, 16U),
                         static_cast<uint8_t>(alpha));
}

bool Renderer::DrawText(micropixel::RasterDrawList& list, int x, int y, const char* text, uint16_t color,
                        int scale) const {
    const Color solid = Color::FromRgb565(color);
    bool ok = true;
    for (const char* p = text; *p != '\0'; ++p, x += (gfx::kGlyphWidth + 1) * scale) {
        int u0 = 0;
        int v0 = 0;
        if (!gfx::GlyphCell(*p, u0, v0)) {
            continue;
        }
        ok = list.SolidSprite(Rect{x, y, gfx::kGlyphWidth * scale, gfx::kGlyphHeight * scale}, kGlyphSlot, solid,
                              static_cast<uint16_t>(u0), static_cast<uint16_t>(v0), gfx::kGlyphWidth,
                              gfx::kGlyphHeight) &&
             ok;
    }
    return ok;
}

bool Renderer::DrawCircle(micropixel::RasterDrawList& list, int cx, int cy, int radius, uint16_t color,
                          bool filled) const {
    const Color solid = Color::FromRgb565(color);
    const int inner = radius - 2;
    bool ok = true;
    for (int y = -radius; y <= radius; ++y) {
        // Half-width of the outer disc on this row, and of the hole for a ring.
        const int outer_half = static_cast<int>(math::Sqrt(static_cast<float>(radius * radius - y * y)));
        if (filled || inner <= 0) {
            ok = list.FillRect(Rect{cx - outer_half, cy + y, 2 * outer_half + 1, 1}, solid) && ok;
            continue;
        }
        const int inner_sq = inner * inner - y * y;
        if (inner_sq < 0) {
            ok = list.FillRect(Rect{cx - outer_half, cy + y, 2 * outer_half + 1, 1}, solid) && ok;
            continue;
        }
        const int inner_half = static_cast<int>(math::Sqrt(static_cast<float>(inner_sq)));
        const int arm = outer_half - inner_half;
        if (arm <= 0) {
            continue;
        }
        ok = list.FillRect(Rect{cx - outer_half, cy + y, arm, 1}, solid) && ok;
        ok = list.FillRect(Rect{cx + inner_half + 1, cy + y, arm, 1}, solid) && ok;
    }
    return ok;
}

bool Renderer::DrawHud(micropixel::RasterDrawList& list, const World& world, const HudStats& hud) {
    const int width = view_.width;
    const int height = view_.height;
    const int half = half_height_;
    const int s = view_.hud_scale;
    const Player& p = world.player();
    TextBuilder text;
    bool ok = true;

    // Status bar.
    const int bar_top = height - hud_height_;
    ok = list.FillRect(Rect{0, bar_top, width, 1}, Color::FromRgb565(gfx::PaletteRgb565(gfx::Index(gfx::kGray, 5)))) &&
         ok;
    ok = list.FillRect(Rect{0, bar_top + 1, width, hud_height_ - 1},
                       Color::FromRgb565(gfx::PaletteRgb565(gfx::Index(gfx::kGray, 1)))) &&
         ok;
    const uint16_t label = gfx::PaletteRgb565(gfx::Index(gfx::kGray, 9));
    const uint16_t health_color =
        gfx::PaletteRgb565(p.health > 50 ? gfx::Index(gfx::kGreen, 12)
                                         : (p.health > 25 ? gfx::Index(gfx::kYellow, 13) : gfx::Index(gfx::kRed, 12)));
    const uint16_t ammo_color = gfx::PaletteRgb565(gfx::Index(gfx::kGold, 12));
    const uint16_t kills_color = gfx::PaletteRgb565(gfx::Index(gfx::kCyan, 12));

    ok = DrawText(list, 8 * s, bar_top + 3 * s, "HEALTH", label, s) && ok;
    text = {};
    text.AppendInt(p.health);
    text.Append("%");
    ok = DrawText(list, 8 * s, bar_top + 11 * s, text.c_str(), health_color, 2 * s) && ok;

    ok = DrawText(list, 92 * s, bar_top + 3 * s, "SHELLS", label, s) && ok;
    text = {};
    text.AppendInt(p.ammo);
    ok = DrawText(list, 92 * s, bar_top + 11 * s, text.c_str(), ammo_color, 2 * s) && ok;

    ok = DrawText(list, 164 * s, bar_top + 3 * s, "IMPS", label, s) && ok;
    text = {};
    text.AppendInt(world.kills());
    text.Append("/");
    text.AppendInt(world.total_imps());
    ok = DrawText(list, 164 * s, bar_top + 11 * s, text.c_str(), kills_color, 2 * s) && ok;

    // Crosshair: four arms leaving the centre open.
    if (world.phase() == Phase::kPlaying) {
        const Color white = Color::FromRgb565(gfx::PaletteRgb565(gfx::Index(gfx::kWhite, 15)));
        const int cx = width / 2;
        const int arm = 3 * s;  // -4s..-2s and 2s..4s
        ok = list.FillRect(Rect{cx - 4 * s, half, arm, s}, white) && ok;
        ok = list.FillRect(Rect{cx + 2 * s, half, arm, s}, white) && ok;
        ok = list.FillRect(Rect{cx, half - 4 * s, s, arm}, white) && ok;
        ok = list.FillRect(Rect{cx, half + 2 * s, s, arm}, white) && ok;
    }

    // Centre message with a drop shadow.
    if (const char* message = world.message()) {
        const int text_width = gfx::TextWidth(message, s);
        const int x = (width - text_width) / 2;
        const int y = world.phase() == Phase::kPlaying ? 88 * s : half - 16 * s;
        ok = DrawText(list, x + s, y + s, message, gfx::PaletteRgb565(gfx::Index(gfx::kGray, 1)), s) && ok;
        ok = DrawText(list, x, y, message, gfx::PaletteRgb565(gfx::Index(gfx::kYellow, 14)), s) && ok;
    }

    if (hud.show_perf) {
        text = {};
        text.Append("FPS ");
        text.AppendInt(static_cast<int>(hud.fps));
        text.Append("  RENDER ");
        text.AppendTenths(hud.render_ms_x10);
        text.Append("  PRESENT ");
        text.AppendTenths(hud.present_ms_x10);
        text.Append("  WAIT ");
        text.AppendTenths(hud.wait_ms_x10);
        ok = DrawText(list, 3 * s, 3 * s, text.c_str(), gfx::PaletteRgb565(gfx::Index(gfx::kGray, 1)), s) && ok;
        ok = DrawText(list, 2 * s, 2 * s, text.c_str(), gfx::PaletteRgb565(gfx::Index(gfx::kCyan, 13)), s) && ok;
    }
    return ok;
}

}  // namespace maze_break::game
