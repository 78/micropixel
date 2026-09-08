#include "sdk/raycast.hpp"

namespace micropixel {
namespace {

constexpr int kMaxDdaSteps = 96;
constexpr float kMinDepth = 0.02F;
constexpr float kFarDepth = 1e30F;
// Billboards closer than this to the view plane are skipped instead of blowing
// up the projected size.
constexpr float kMinBillboardDepth = 0.08F;

// Guests link without libm; these are Wasm instructions.
[[nodiscard]] float Floor(float value) { return __builtin_floorf(value); }
[[nodiscard]] int FloorInt(float value) { return static_cast<int>(__builtin_floorf(value)); }
[[nodiscard]] float Fabs(float value) { return __builtin_fabsf(value); }
[[nodiscard]] int Clamp(int value, int low, int high) { return value < low ? low : (value > high ? high : value); }

}  // namespace

bool LightTable::Build(const DistanceLighting& lighting) {
    if (lighting.levels == 0 || lighting.minimum >= lighting.levels || !(lighting.curve_levels > 0.0F) ||
        !(lighting.full_distance > 0.0F) || !(lighting.falloff >= 0.0F)) {
        return false;
    }
    for (int i = 0; i < kEntries; ++i) {
        const float distance = static_cast<float>(i) / 16.0F;
        const int light = static_cast<int>(lighting.curve_levels * lighting.full_distance /
                                               (lighting.full_distance + distance * lighting.falloff) +
                                           0.5F);
        entries_[i] = static_cast<uint8_t>(Clamp(light, lighting.minimum, lighting.levels - 1));
    }
    brightest_ = static_cast<uint8_t>(lighting.levels - 1);
    levels_ = lighting.levels;
    side_shade_ = lighting.side_shade;
    return true;
}

uint8_t LightTable::LightFor(float distance) const {
    const int index = static_cast<int>(distance * 16.0F);
    return entries_[Clamp(index, 0, kEntries - 1)];
}

bool Raycaster::Initialize(const RaycastConfig& config) {
    if (config.width <= 0 || config.height <= 0 || config.texture_shift == 0 || config.texture_shift > 15 ||
        !light_.Build(config.lighting)) {
        return false;
    }
    config_ = config;
    if (config_.width > kMaxColumns) {
        config_.width = kMaxColumns;
    }
    half_height_ = config_.height / 2;
    for (int x = 0; x < kMaxColumns; ++x) {
        depth_[x] = kFarDepth;
        walls_[x] = Slice{};
        slabs_[x] = Slice{};
        cover_top_[x] = static_cast<int16_t>(config_.height);
        cover_bottom_[x] = -1;
    }
    return true;
}

float Raycaster::Depth(int column) const { return column >= 0 && column < config_.width ? depth_[column] : kFarDepth; }

bool Raycaster::Project(float x, float y, int& column_out, float& depth_out) const {
    const RaycastCamera& p = camera_;
    const float det = p.plane_x * p.dir_y - p.dir_x * p.plane_y;
    if (det == 0.0F) {
        return false;
    }
    const float inv_det = 1.0F / det;
    const float sx = x - p.x;
    const float sy = y - p.y;
    const float transform_x = inv_det * (p.dir_y * sx - p.dir_x * sy);
    const float transform_y = inv_det * (-p.plane_y * sx + p.plane_x * sy);
    if (transform_y <= 0.0F) {
        return false;
    }
    column_out = static_cast<int>((config_.width / 2) * (1.0F + transform_x / transform_y));
    depth_out = transform_y;
    return true;
}

void Raycaster::Cast(const RaycastCamera& camera, const RaycastGrid& grid) {
    camera_ = camera;
    const int width = config_.width;
    const int height = config_.height;
    const int half = half_height_;
    const float height_f = static_cast<float>(height);
    const int texture_size = 1 << config_.texture_shift;
    const int texture_mask = texture_size - 1;
    const RaycastCamera& p = camera_;

    for (int x = 0; x < width; ++x) {
        walls_[x] = Slice{};
        slabs_[x] = Slice{};
        // Nothing covered until a wall column says otherwise.
        cover_top_[x] = static_cast<int16_t>(height);
        cover_bottom_[x] = -1;
        depth_[x] = kFarDepth;
        const float camera_x = 2.0F * static_cast<float>(x) / static_cast<float>(width) - 1.0F;
        const float ray_x = p.dir_x + p.plane_x * camera_x;
        const float ray_y = p.dir_y + p.plane_y * camera_x;
        int map_x = FloorInt(p.x);
        int map_y = FloorInt(p.y);
        const float delta_x = ray_x == 0.0F ? kFarDepth : Fabs(1.0F / ray_x);
        const float delta_y = ray_y == 0.0F ? kFarDepth : Fabs(1.0F / ray_y);
        const int step_x = ray_x < 0.0F ? -1 : 1;
        const int step_y = ray_y < 0.0F ? -1 : 1;
        float side_x = ray_x < 0.0F ? (p.x - static_cast<float>(map_x)) * delta_x
                                    : (static_cast<float>(map_x) + 1.0F - p.x) * delta_x;
        float side_y = ray_y < 0.0F ? (p.y - static_cast<float>(map_y)) * delta_y
                                    : (static_cast<float>(map_y) + 1.0F - p.y) * delta_y;

        int side = 0;
        bool hit = false;
        RaycastCell hit_cell{};
        float hit_dist = kFarDepth;
        bool slab_seen = false;
        float slab_dist = 0.0F;
        RaycastCell slab_cell{};
        int slab_side = 0;

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
            const RaycastCell cell = grid.At(map_x, map_y);
            if (cell.kind == RaycastCellKind::kSlab) {
                if (cell.open >= RaycastCell::kOpenScale) {
                    continue;
                }
                if (cell.open == 0) {
                    hit = true;
                    hit_cell = cell;
                    hit_dist = side == 0 ? side_x - delta_x : side_y - delta_y;
                    break;
                }
                if (!slab_seen) {
                    slab_seen = true;
                    slab_dist = side == 0 ? side_x - delta_x : side_y - delta_y;
                    slab_cell = cell;
                    slab_side = side;
                }
                continue;
            }
            if (cell.kind == RaycastCellKind::kWall) {
                hit = true;
                hit_cell = cell;
                hit_dist = side == 0 ? side_x - delta_x : side_y - delta_y;
                break;
            }
        }
        if (!hit) {
            continue;
        }
        if (hit_dist < kMinDepth) {
            hit_dist = kMinDepth;
        }

        // Far wall (or closed slab).
        {
            float wall_x = side == 0 ? p.y + hit_dist * ray_y : p.x + hit_dist * ray_x;
            wall_x -= Floor(wall_x);
            int u = static_cast<int>(wall_x * static_cast<float>(texture_size)) & texture_mask;
            if ((side == 0 && ray_x > 0.0F) || (side == 1 && ray_y < 0.0F)) {
                u = texture_mask - u;
            }
            const int line_height = static_cast<int>(height_f / hit_dist);
            if (line_height > 0) {
                const int draw_start = Clamp(-line_height / 2 + half, 0, height - 1);
                const int draw_end = Clamp(line_height / 2 + half, 0, height - 1);
                const int32_t step = (texture_size << 16) / line_height;
                const int32_t tex_pos = (draw_start - half + line_height / 2) * step;
                const int light =
                    Clamp(LightFor(hit_dist) - (side == 1 ? light_.side_shade() : 0), 0, light_.levels() - 1);
                walls_[x] = Slice{
                    .y0 = static_cast<int16_t>(draw_start),
                    .y1 = static_cast<int16_t>(draw_end),
                    .u = static_cast<uint16_t>(u),
                    .v_start = tex_pos,
                    .v_step = step,
                    .texture_slot = hit_cell.texture_slot,
                    .light = static_cast<uint8_t>(light),
                };
                cover_top_[x] = static_cast<int16_t>(draw_start);
                cover_bottom_[x] = static_cast<int16_t>(draw_end);
            }
            depth_[x] = hit_dist;
        }

        // Partially raised slab in front of the far wall. The slab slides up
        // into the ceiling, so the visible part is the bottom (1-open) of the
        // texture pinned to the top of the opening.
        if (slab_seen) {
            if (slab_dist < kMinDepth) {
                slab_dist = kMinDepth;
            }
            float wall_x = slab_side == 0 ? p.y + slab_dist * ray_y : p.x + slab_dist * ray_x;
            wall_x -= Floor(wall_x);
            int u = static_cast<int>(wall_x * static_cast<float>(texture_size)) & texture_mask;
            if ((slab_side == 0 && ray_x > 0.0F) || (slab_side == 1 && ray_y < 0.0F)) {
                u = texture_mask - u;
            }
            const int line_height = static_cast<int>(height_f / slab_dist);
            if (line_height > 0) {
                const int top = -line_height / 2 + half;
                const float open = slab_cell.open_fraction();
                const int visible = static_cast<int>((1.0F - open) * static_cast<float>(line_height));
                const int draw_start = Clamp(top, 0, height - 1);
                int draw_end = Clamp(top + visible - 1, -1, height - 1);
                const int32_t step = (texture_size << 16) / line_height;
                const int32_t tex_pos = static_cast<int32_t>(open * static_cast<float>(texture_size) * 65536.0F) +
                                        (draw_start - top) * step;
                // The texture must not wrap past its last row at the slab's
                // bottom edge; the column kernel wraps, so clip the run instead.
                const int32_t last_row = (texture_size << 16) - 1;
                if (step > 0 && tex_pos <= last_row) {
                    const int rows_in_texture = (last_row - tex_pos) / step + 1;
                    if (draw_end - draw_start + 1 > rows_in_texture) {
                        draw_end = draw_start + rows_in_texture - 1;
                    }
                } else {
                    draw_end = draw_start - 1;
                }
                if (draw_end >= draw_start) {
                    const int light =
                        Clamp(LightFor(slab_dist) - (slab_side == 1 ? light_.side_shade() : 0), 0, light_.levels() - 1);
                    slabs_[x] = Slice{
                        .y0 = static_cast<int16_t>(draw_start),
                        .y1 = static_cast<int16_t>(draw_end),
                        .u = static_cast<uint16_t>(u),
                        .v_start = tex_pos,
                        .v_step = step,
                        .texture_slot = slab_cell.texture_slot,
                        .light = static_cast<uint8_t>(light),
                    };
                    if (open < kSlabBlocksBillboards) {
                        depth_[x] = slab_dist;
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

bool Raycaster::DrawWorld(RasterDrawList& list) const { return DrawFloorAndCeiling(list) && DrawWalls(list); }

bool Raycaster::DrawFloorAndCeiling(RasterDrawList& list) const {
    const int width = config_.width;
    const int height = config_.height;
    const int half = half_height_;
    const RaycastCamera& p = camera_;
    // Leftmost and rightmost rays.
    const float ray0_x = p.dir_x - p.plane_x;
    const float ray0_y = p.dir_y - p.plane_y;
    const float ray1_x = p.dir_x + p.plane_x;
    const float ray1_y = p.dir_y + p.plane_y;
    const float pos_z = 0.5F * static_cast<float>(height);
    const float inv_width = 1.0F / static_cast<float>(width);
    const int blocks = (width + kCoverBlock - 1) / kCoverBlock;

    auto span_pair = [&](int y_floor, int y_ceiling, int x0, int x1, int32_t fx, int32_t fy, int32_t sx, int32_t sy,
                         uint8_t light) {
        return list.SpanPair(static_cast<uint16_t>(y_floor), static_cast<uint16_t>(y_ceiling),
                             static_cast<uint16_t>(x0), static_cast<uint16_t>(x1), config_.floor_slot,
                             config_.ceiling_slot, light, fx, fy, sx, sy);
    };

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
        const uint8_t light = LightFor(row_distance);
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
                    ok = span_pair(y, y_ceiling, run_start, x0 - 1, fx + sx * run_start, fy + sy * run_start, sx, sy,
                                   light) &&
                         ok;
                    run_start = -1;
                }
                continue;
            }
            for (int x = x0; x < x1; ++x) {
                const bool hidden = y <= cover_bottom_[x] && y_ceiling >= cover_top_[x];
                if (hidden) {
                    if (run_start >= 0) {
                        ok = span_pair(y, y_ceiling, run_start, x - 1, fx + sx * run_start, fy + sy * run_start, sx, sy,
                                       light) &&
                             ok;
                        run_start = -1;
                    }
                } else if (run_start < 0) {
                    run_start = x;
                }
            }
        }
        if (run_start >= 0) {
            ok = span_pair(y, y_ceiling, run_start, width - 1, fx + sx * run_start, fy + sy * run_start, sx, sy,
                           light) &&
                 ok;
        }
        if (!ok) {
            return false;
        }
    }
    if ((height & 1) == 0) {
        // Even heights leave row half-1 unmirrored; paint it as the far
        // ceiling at the horizon's distance from the ceiling texture origin.
        return list.SpanPair(static_cast<uint16_t>(half - 1), static_cast<uint16_t>(half - 1), 0,
                             static_cast<uint16_t>(width - 1), config_.ceiling_slot, config_.ceiling_slot,
                             LightFor(pos_z), 0, 0, 0, 0);
    }
    return true;
}

bool Raycaster::DrawWalls(RasterDrawList& list) const {
    bool ok = true;
    for (int x = 0; x < config_.width; ++x) {
        const Slice& wall = walls_[x];
        if (wall.y1 >= wall.y0) {
            ok = list.Column(static_cast<uint16_t>(x), wall.y0, wall.y1, wall.texture_slot, wall.light, wall.u,
                             wall.v_start, wall.v_step) &&
                 ok;
        }
        const Slice& slab = slabs_[x];
        if (slab.y1 >= slab.y0) {
            ok = list.Column(static_cast<uint16_t>(x), slab.y0, slab.y1, slab.texture_slot, slab.light, slab.u,
                             slab.v_start, slab.v_step) &&
                 ok;
        }
    }
    return ok;
}

bool Raycaster::DrawBillboards(RasterDrawList& list, std::span<const Billboard> billboards) {
    const int width = config_.width;
    const int height = config_.height;
    const int half = half_height_;
    const RaycastCamera& p = camera_;
    const float det = p.plane_x * p.dir_y - p.dir_x * p.plane_y;
    if (det == 0.0F) {
        return true;
    }
    const float inv_det = 1.0F / det;
    const int count =
        billboards.size() > static_cast<size_t>(kMaxBillboards) ? kMaxBillboards : static_cast<int>(billboards.size());

    // Sort far to near so nearer billboards paint over farther ones.
    int visible = 0;
    for (int i = 0; i < count; ++i) {
        const float sx = billboards[i].x - p.x;
        const float sy = billboards[i].y - p.y;
        const float depth = inv_det * (-p.plane_y * sx + p.plane_x * sy);
        if (depth <= kMinBillboardDepth) {
            continue;
        }
        billboard_depth_[i] = depth;
        billboard_order_[visible++] = static_cast<uint8_t>(i);
    }
    for (int i = 1; i < visible; ++i) {
        const uint8_t key = billboard_order_[i];
        int j = i - 1;
        while (j >= 0 && billboard_depth_[billboard_order_[j]] < billboard_depth_[key]) {
            billboard_order_[j + 1] = billboard_order_[j];
            --j;
        }
        billboard_order_[j + 1] = key;
    }

    bool ok = true;
    for (int n = 0; n < visible; ++n) {
        const Billboard& sprite = billboards[billboard_order_[n]];
        if (sprite.texture_width == 0 || sprite.texture_height == 0) {
            continue;
        }
        const float sx = sprite.x - p.x;
        const float sy = sprite.y - p.y;
        const float transform_x = inv_det * (p.dir_y * sx - p.dir_x * sy);
        const float transform_y = billboard_depth_[billboard_order_[n]];
        const int screen_x = static_cast<int>(static_cast<float>(width / 2) * (1.0F + transform_x / transform_y));
        const float wall_height = static_cast<float>(height) / transform_y;
        const int sprite_height = static_cast<int>(wall_height * sprite.height);
        if (sprite_height <= 0) {
            continue;
        }
        const int bottom = static_cast<int>(static_cast<float>(half) + wall_height * 0.5F - wall_height * sprite.lift);
        const int top = bottom - sprite_height;
        const int sprite_width = sprite_height * sprite.texture_width / sprite.texture_height;
        if (sprite_width <= 0) {
            continue;
        }
        const int left = screen_x - sprite_width / 2;
        const int right = left + sprite_width;
        if (bottom <= 0 || top >= height || right <= 0 || left >= width) {
            continue;
        }
        const uint8_t light = sprite.self_lit ? light_.brightest() : LightFor(transform_y);
        const int32_t u_step = (static_cast<int32_t>(sprite.texture_width) << 16) / sprite_width;
        const int32_t v_step = (static_cast<int32_t>(sprite.texture_height) << 16) / sprite_height;
        const int y0 = Clamp(top, 0, height - 1);
        const int y1 = Clamp(bottom - 1, 0, height - 1);
        const int32_t v_start = (y0 - top) * v_step;
        const int x0 = Clamp(left, 0, width - 1);
        const int x1 = Clamp(right - 1, 0, width - 1);
        for (int stripe = x0; stripe <= x1; ++stripe) {
            if (transform_y >= depth_[stripe]) {
                continue;
            }
            const int u = ((stripe - left) * u_step) >> 16;
            ok = list.Column(static_cast<uint16_t>(stripe), static_cast<int16_t>(y0), static_cast<int16_t>(y1),
                             sprite.texture_slot, light, static_cast<uint16_t>(u), v_start, v_step, true) &&
                 ok;
        }
    }
    return ok;
}

}  // namespace micropixel
