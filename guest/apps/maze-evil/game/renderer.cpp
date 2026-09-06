#include "apps/maze-evil/game/renderer.hpp"

#include "apps/maze-evil/gfx/font.hpp"
#include "apps/maze-evil/gfx/palette.hpp"
#include "apps/maze-evil/gfx/textures.hpp"
#include "apps/maze-evil/rc_math.hpp"

namespace maze_break::game {
namespace {

constexpr int kMaxDdaSteps = 96;
constexpr float kMinDepth = 0.02F;

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
    if (view_.width > gfx::kMaxViewWidth) {
        view_.width = gfx::kMaxViewWidth;
    }
    if (view_.height > gfx::kMaxViewHeight) {
        view_.height = gfx::kMaxViewHeight;
    }
    if (view_.hud_scale < 1) {
        view_.hud_scale = 1;
    }
    half_height_ = view_.height / 2;
    hud_height_ = 26 * view_.hud_scale;
    // Light falls off with distance; entries are indexed by distance * 16.
    // The curve is tuned on a 32-level scale and clamped to the palette's 16
    // levels, so everything closer than ~2.6 tiles reads at full brightness.
    constexpr float kCurveLevels = 31.0F;
    for (int i = 0; i < 512; ++i) {
        const float distance = i / 16.0F;
        const int light = static_cast<int>(kCurveLevels * 2.6F / (2.6F + distance * 1.05F) + 0.5F);
        light_lut_[i] = static_cast<uint8_t>(Clamp(light, 2, gfx::kLightLevels - 1));
    }
}

int Renderer::LightFor(float distance) const {
    const int index = static_cast<int>(distance * 16.0F);
    return light_lut_[Clamp(index, 0, 511)];
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

bool Renderer::UploadResources(const micropixel::SurfaceRaster& raster) {
    if (raster.max_textures() < static_cast<uint32_t>(kSlotCount) ||
        raster.max_light_levels() < static_cast<uint32_t>(gfx::kLightLevels)) {
        return false;
    }
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
                                static_cast<uint32_t>(padded_height), micropixel::RasterLayout::kColumnMajor, scratch)
                 .has_value()) {
            return false;
        }
    }
    gfx::BuildGlyphAtlas(scratch);
    if (!raster
             .UploadTexture(kGlyphSlot, gfx::kGlyphAtlasWidth, gfx::kGlyphAtlasHeight,
                            micropixel::RasterLayout::kColumnMajor, scratch)
             .has_value()) {
        return false;
    }
    // The colormaps are exactly a lit palette: kLightLevels x 256 canonical
    // RGB565, contiguous from light 0.
    return raster.UploadLitPalette(gfx::kLightLevels, gfx::ColormapFor(0)).has_value();
}

bool Renderer::Render(micropixel::RasterDrawList& list, const World& world, const HudStats& hud) {
    // Cast first so the floor pass knows which rows the walls will cover;
    // records are executed in order: floor -> walls -> things -> overlays.
    CastWalls(world);
    return DrawFloorAndCeiling(list, world.player()) && DrawWalls(list) && DrawThings(list, world) &&
           DrawWeapon(list, world) && DrawDamageTint(list, world) && (!hud.visible || DrawHud(list, world, hud));
}

namespace {

bool SpanPair(micropixel::RasterDrawList& list, int y_floor, int y_ceiling, int x0, int x1, int32_t fx, int32_t fy,
              int32_t sx, int32_t sy, int light) {
    return list.SpanPair(static_cast<uint16_t>(y_floor), static_cast<uint16_t>(y_ceiling), static_cast<uint16_t>(x0),
                         static_cast<uint16_t>(x1), gfx::kTexFloor, gfx::kTexCeiling, static_cast<uint8_t>(light), fx,
                         fy, sx, sy);
}

}  // namespace

bool Renderer::DrawFloorAndCeiling(micropixel::RasterDrawList& list, const Player& p) {
    const int width = view_.width;
    const int height = view_.height;
    const int half = half_height_;
    // Leftmost and rightmost rays.
    const float ray0_x = p.dir_x - p.plane_x;
    const float ray0_y = p.dir_y - p.plane_y;
    const float ray1_x = p.dir_x + p.plane_x;
    const float ray1_y = p.dir_y + p.plane_y;
    const float pos_z = 0.5F * static_cast<float>(height);
    const float inv_width = 1.0F / static_cast<float>(width);

    const int blocks = (width + kCoverBlock - 1) / kCoverBlock;

    // The centre row (y == half) belongs to the walls at infinity; the loop
    // fills the floor row y and mirrors the ceiling to height-1-y. Pixels
    // where both rows are hidden by the far wall are skipped: that is the
    // bulk of the frame's overdraw, and the walls are painted afterwards so
    // painting a covered pixel is only wasted work, never a visible error.
    for (int y = half + 1; y < height; ++y) {
        const int p_row = y - half;
        const float row_distance = pos_z / static_cast<float>(p_row);
        const float step_x = row_distance * (ray1_x - ray0_x) * inv_width;
        const float step_y = row_distance * (ray1_y - ray0_y) * inv_width;
        const float floor_x = p.x + row_distance * ray0_x;
        const float floor_y = p.y + row_distance * ray0_y;
        // 16.16 world coordinates: the integer part is the tile, the fraction
        // selects the texel.
        const int32_t fx = static_cast<int32_t>(floor_x * 65536.0F);
        const int32_t fy = static_cast<int32_t>(floor_y * 65536.0F);
        const int32_t sx = static_cast<int32_t>(step_x * 65536.0F);
        const int32_t sy = static_cast<int32_t>(step_y * 65536.0F);
        const int y_ceiling = height - 1 - y;
        const int light = LightFor(row_distance);
        // Walk the row in kCoverBlock-wide blocks; only blocks the wall edge
        // passes through are tested per column.
        int run_start = -1;
        bool ok = true;
        for (int block = 0; block < blocks; ++block) {
            const int x0 = block * kCoverBlock;
            const int x1 = x0 + kCoverBlock > width ? width : x0 + kCoverBlock;
            if (y > block_bottom_max_[block]) {
                // Every column shows floor here.
                if (run_start < 0) {
                    run_start = x0;
                }
                continue;
            }
            if (y <= block_bottom_min_[block] && y_ceiling >= block_top_max_[block]) {
                // Every column is behind the wall.
                if (run_start >= 0) {
                    ok = SpanPair(list, y, y_ceiling, run_start, x0 - 1, fx + sx * run_start, fy + sy * run_start, sx,
                                  sy, light) &&
                         ok;
                    run_start = -1;
                }
                continue;
            }
            for (int x = x0; x < x1; ++x) {
                const bool hidden = y <= cover_bottom_[x] && y_ceiling >= cover_top_[x];
                if (hidden) {
                    if (run_start >= 0) {
                        ok = SpanPair(list, y, y_ceiling, run_start, x - 1, fx + sx * run_start, fy + sy * run_start,
                                      sx, sy, light) &&
                             ok;
                        run_start = -1;
                    }
                } else if (run_start < 0) {
                    run_start = x;
                }
            }
        }
        if (run_start >= 0) {
            ok = SpanPair(list, y, y_ceiling, run_start, width - 1, fx + sx * run_start, fy + sy * run_start, sx, sy,
                          light) &&
                 ok;
        }
        if (!ok) {
            return false;
        }
    }
    if ((height & 1) == 0) {
        // Even heights leave row half-1 unmirrored; treat it as the far ceiling.
        const uint16_t color = gfx::ColormapFor(LightFor(pos_z))[gfx::TextureFor(gfx::kTexCeiling)[0]];
        return list.FillRect(Rect{0, half - 1, width, 1}, Color::FromRgb565(color));
    }
    return true;
}

bool Renderer::DrawWalls(micropixel::RasterDrawList& list) {
    const int width = view_.width;
    bool ok = true;
    for (int x = 0; x < width; ++x) {
        const WallSlice& wall = walls_[x];
        if (wall.y1 >= wall.y0) {
            ok = list.Column(static_cast<uint16_t>(x), wall.y0, wall.y1, wall.texture, wall.light, wall.tex_x,
                             wall.v_start, wall.v_step) &&
                 ok;
        }
        const WallSlice& door = doors_[x];
        if (door.y1 >= door.y0) {
            ok = list.Column(static_cast<uint16_t>(x), door.y0, door.y1, door.texture, door.light, door.tex_x,
                             door.v_start, door.v_step) &&
                 ok;
        }
    }
    return ok;
}

void Renderer::CastWalls(const World& world) {
    const int width = view_.width;
    const int height = view_.height;
    const int half = half_height_;
    const float height_f = static_cast<float>(height);
    const Player& p = world.player();
    for (int x = 0; x < width; ++x) {
        walls_[x] = WallSlice{};
        doors_[x] = WallSlice{};
        // Nothing covered until a wall column says otherwise.
        cover_top_[x] = static_cast<int16_t>(height);
        cover_bottom_[x] = -1;
        const float camera_x = 2.0F * x / static_cast<float>(width) - 1.0F;
        const float ray_x = p.dir_x + p.plane_x * camera_x;
        const float ray_y = p.dir_y + p.plane_y * camera_x;
        int map_x = math::FloorInt(p.x);
        int map_y = math::FloorInt(p.y);
        const float delta_x = ray_x == 0.0F ? 1e30F : math::Fabs(1.0F / ray_x);
        const float delta_y = ray_y == 0.0F ? 1e30F : math::Fabs(1.0F / ray_y);
        const int step_x = ray_x < 0.0F ? -1 : 1;
        const int step_y = ray_y < 0.0F ? -1 : 1;
        float side_x = ray_x < 0.0F ? (p.x - map_x) * delta_x : (map_x + 1.0F - p.x) * delta_x;
        float side_y = ray_y < 0.0F ? (p.y - map_y) * delta_y : (map_y + 1.0F - p.y) * delta_y;

        int side = 0;
        Tile hit_tile = Tile::kBrick;
        float hit_dist = 1e30F;
        bool door_seen = false;
        float door_dist = 0.0F;
        float door_open = 0.0F;
        int door_side = 0;

        for (int step = 0; step < kMaxDdaSteps; ++step) {
            if (side_x < side_y) {
                side_x += delta_x;
                map_x += step_x;
                side = 0;
            } else {
                side_y += delta_y;
                map_y += step_y;
                side = 1;
            }
            const Tile tile = world.TileAt(map_x, map_y);
            if (tile == Tile::kDoor) {
                const float open = world.DoorOpen(map_x, map_y);
                if (open >= 1.0F) {
                    continue;
                }
                if (open <= 0.0F) {
                    hit_tile = tile;
                    hit_dist = side == 0 ? side_x - delta_x : side_y - delta_y;
                    break;
                }
                if (!door_seen) {
                    door_seen = true;
                    door_dist = side == 0 ? side_x - delta_x : side_y - delta_y;
                    door_open = open;
                    door_side = side;
                }
                continue;
            }
            if (IsWall(tile)) {
                hit_tile = tile;
                hit_dist = side == 0 ? side_x - delta_x : side_y - delta_y;
                break;
            }
        }
        if (hit_dist < kMinDepth) {
            hit_dist = kMinDepth;
        }

        // Far wall (or closed door).
        {
            float wall_x = side == 0 ? p.y + hit_dist * ray_y : p.x + hit_dist * ray_x;
            wall_x -= math::Floor(wall_x);
            int tex_x = static_cast<int>(wall_x * gfx::kTextureSize) & gfx::kTextureMask;
            if ((side == 0 && ray_x > 0.0F) || (side == 1 && ray_y < 0.0F)) {
                tex_x = gfx::kTextureMask - tex_x;
            }
            const int line_height = static_cast<int>(height_f / hit_dist);
            if (line_height > 0) {
                const int draw_start = Clamp(-line_height / 2 + half, 0, height - 1);
                const int draw_end = Clamp(line_height / 2 + half, 0, height - 1);
                const int32_t step = (gfx::kTextureSize << 16) / line_height;
                const int32_t tex_pos = (draw_start - half + line_height / 2) * step;
                const int light = Clamp(LightFor(hit_dist) - (side == 1 ? 4 : 0), 0, gfx::kLightLevels - 1);
                walls_[x] = WallSlice{
                    .y0 = static_cast<int16_t>(draw_start),
                    .y1 = static_cast<int16_t>(draw_end),
                    .tex_x = static_cast<uint16_t>(tex_x),
                    .v_start = tex_pos,
                    .v_step = step,
                    .texture = WallTexture(hit_tile),
                    .light = static_cast<uint8_t>(light),
                };
                cover_top_[x] = static_cast<int16_t>(draw_start);
                cover_bottom_[x] = static_cast<int16_t>(draw_end);
            }
            zbuffer_[x] = hit_dist;
        }

        // Partially raised door slab in front of the far wall. The slab slides
        // up into the ceiling, so the visible part is the bottom (1-open) of
        // the texture pinned to the top of the doorway.
        if (door_seen) {
            if (door_dist < kMinDepth) {
                door_dist = kMinDepth;
            }
            float wall_x = door_side == 0 ? p.y + door_dist * ray_y : p.x + door_dist * ray_x;
            wall_x -= math::Floor(wall_x);
            int tex_x = static_cast<int>(wall_x * gfx::kTextureSize) & gfx::kTextureMask;
            if ((door_side == 0 && ray_x > 0.0F) || (door_side == 1 && ray_y < 0.0F)) {
                tex_x = gfx::kTextureMask - tex_x;
            }
            const int line_height = static_cast<int>(height_f / door_dist);
            if (line_height > 0) {
                const int top = -line_height / 2 + half;
                const int visible = static_cast<int>((1.0F - door_open) * line_height);
                const int draw_start = Clamp(top, 0, height - 1);
                int draw_end = Clamp(top + visible - 1, -1, height - 1);
                const int32_t step = (gfx::kTextureSize << 16) / line_height;
                const int32_t tex_pos =
                    static_cast<int32_t>(door_open * gfx::kTextureSize * 65536.0F) + (draw_start - top) * step;
                // The texture must not wrap past its last row at the slab's
                // bottom edge; the column kernel wraps, so clip the run instead.
                const int32_t last_row = (gfx::kTextureSize << 16) - 1;
                if (step > 0 && tex_pos <= last_row) {
                    const int rows_in_texture = (last_row - tex_pos) / step + 1;
                    if (draw_end - draw_start + 1 > rows_in_texture) {
                        draw_end = draw_start + rows_in_texture - 1;
                    }
                } else {
                    draw_end = draw_start - 1;
                }
                if (draw_end >= draw_start) {
                    const int light = Clamp(LightFor(door_dist) - (door_side == 1 ? 4 : 0), 0, gfx::kLightLevels - 1);
                    doors_[x] = WallSlice{
                        .y0 = static_cast<int16_t>(draw_start),
                        .y1 = static_cast<int16_t>(draw_end),
                        .tex_x = static_cast<uint16_t>(tex_x),
                        .v_start = tex_pos,
                        .v_step = step,
                        .texture = gfx::kTexDoor,
                        .light = static_cast<uint8_t>(light),
                    };
                    if (door_open < 0.5F) {
                        zbuffer_[x] = door_dist;
                    }
                }
            }
        }
    }
    // Block summaries let the floor pass classify most blocks without
    // touching individual columns.
    const int blocks = (width + kCoverBlock - 1) / kCoverBlock;
    for (int block = 0; block < blocks; ++block) {
        const int x0 = block * kCoverBlock;
        const int x1 = x0 + kCoverBlock > width ? width : x0 + kCoverBlock;
        int bottom_min = cover_bottom_[x0];
        int bottom_max = cover_bottom_[x0];
        int top_max = cover_top_[x0];
        for (int x = x0 + 1; x < x1; ++x) {
            bottom_min = cover_bottom_[x] < bottom_min ? cover_bottom_[x] : bottom_min;
            bottom_max = cover_bottom_[x] > bottom_max ? cover_bottom_[x] : bottom_max;
            top_max = cover_top_[x] > top_max ? cover_top_[x] : top_max;
        }
        block_bottom_min_[block] = static_cast<int16_t>(bottom_min);
        block_bottom_max_[block] = static_cast<int16_t>(bottom_max);
        block_top_max_[block] = static_cast<int16_t>(top_max);
    }
}

bool Renderer::DrawThings(micropixel::RasterDrawList& list, const World& world) {
    const int width = view_.width;
    const int height = view_.height;
    const int half = half_height_;
    const Player& p = world.player();
    const int count = world.CollectThings(things_, World::kMaxThings);
    const float inv_det = 1.0F / (p.plane_x * p.dir_y - p.dir_x * p.plane_y);

    // Sort far to near so nearer billboards paint over farther ones.
    int visible = 0;
    for (int i = 0; i < count; ++i) {
        const float sx = things_[i].x - p.x;
        const float sy = things_[i].y - p.y;
        const float depth = inv_det * (-p.plane_y * sx + p.plane_x * sy);
        if (depth <= 0.08F) {
            continue;
        }
        thing_depth_[i] = depth;
        order_[visible++] = static_cast<uint8_t>(i);
    }
    for (int i = 1; i < visible; ++i) {
        const uint8_t key = order_[i];
        int j = i - 1;
        while (j >= 0 && thing_depth_[order_[j]] < thing_depth_[key]) {
            order_[j + 1] = order_[j];
            --j;
        }
        order_[j + 1] = key;
    }

    bool ok = true;
    for (int n = 0; n < visible; ++n) {
        const Thing& thing = things_[order_[n]];
        const gfx::Sprite& sprite = gfx::SpriteFor(thing.sprite);
        const float sx = thing.x - p.x;
        const float sy = thing.y - p.y;
        const float transform_x = inv_det * (p.dir_y * sx - p.dir_x * sy);
        const float transform_y = thing_depth_[order_[n]];
        const int screen_x = static_cast<int>((width / 2) * (1.0F + transform_x / transform_y));
        const float wall_height = static_cast<float>(height) / transform_y;
        const int sprite_height = static_cast<int>(wall_height * thing.height);
        if (sprite_height <= 0) {
            continue;
        }
        const int bottom = static_cast<int>(half + wall_height * 0.5F - wall_height * thing.lift);
        const int top = bottom - sprite_height;
        const int sprite_width = sprite_height * sprite.width / sprite.height;
        if (sprite_width <= 0) {
            continue;
        }
        const int left = screen_x - sprite_width / 2;
        const int right = left + sprite_width;
        if (bottom <= 0 || top >= height || right <= 0 || left >= width) {
            continue;
        }
        const bool self_lit = thing.sprite == gfx::kSprFireballA || thing.sprite == gfx::kSprFireballB ||
                              gfx::IsTorchSprite(thing.sprite);
        const int light = self_lit ? gfx::kLightLevels - 1 : LightFor(transform_y);

        const int32_t u_step = (sprite.width << 16) / sprite_width;
        const int32_t v_step = (sprite.height << 16) / sprite_height;
        const int y0 = Clamp(top, 0, height - 1);
        const int y1 = Clamp(bottom - 1, 0, height - 1);
        const int32_t v_start = (y0 - top) * v_step;
        const int x0 = Clamp(left, 0, width - 1);
        const int x1 = Clamp(right - 1, 0, width - 1);
        const auto slot = static_cast<uint8_t>(kSpriteSlotBase + thing.sprite);
        for (int stripe = x0; stripe <= x1; ++stripe) {
            if (transform_y >= zbuffer_[stripe]) {
                continue;
            }
            const int u = ((stripe - left) * u_step) >> 16;
            ok = list.Column(static_cast<uint16_t>(stripe), static_cast<int16_t>(y0), static_cast<int16_t>(y1), slot,
                             static_cast<uint8_t>(light), static_cast<uint16_t>(u), v_start, v_step, true) &&
                 ok;
        }
    }
    return ok;
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
