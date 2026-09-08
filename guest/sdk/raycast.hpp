#ifndef MICROPIXEL_SDK_RAYCAST_HPP
#define MICROPIXEL_SDK_RAYCAST_HPP

#include <stdint.h>

#include <span>

#include "sdk/graphics.hpp"
#include "sdk/raster_world.hpp"

namespace micropixel {

// Grid raycaster front end over the Host raster kernels (Graphics 1.6).
//
// The App owns the map grid, the camera and the list of billboards; the Raycaster
// runs the per-column geometry (ray DDA, wall and slab projection, floor row
// setup, coverage tests, billboard sort and per-column z-test) and appends
// SPAN_PAIR and COLUMN records to a RasterDrawList. Every pixel is written by
// the Host from those records, so the App never maps a frame buffer and the
// Guest does no per-pixel work. Weapons, HUD and text are appended by the App
// after DrawBillboards() with the ordinary RasterDrawList methods.
//
// World units: the map is a grid of unit cells and every wall is one unit
// high; the camera sits at half wall height. Distances are measured along the
// camera direction (perpendicular distance), which is what the Host column
// kernels want for the projected height.

// Camera in map coordinates. `dir` is the unit view direction; `plane` is the
// half-width of the view plane at distance 1 (its length is tan(fov / 2)) and
// must be perpendicular to `dir`.
struct RaycastCamera final {
    float x{};
    float y{};
    float dir_x{1.0F};
    float dir_y{};
    float plane_x{};
    float plane_y{0.66F};
};

enum class RaycastCellKind : uint8_t {
    kEmpty = 0,
    // Solid unit cube drawn with `texture_slot` (kColumnMajor, side texture_size).
    kWall = 1,
    // A wall slab that slides up into the ceiling: `open` is the raised
    // fraction in 0..1, open >= 1 is passable. The ray records the nearest slab
    // it crosses and keeps walking to the wall behind it. A slab with
    // open < kSlabBlocksBillboards occludes billboards behind it.
    kSlab = 2,
};

// One map cell, 4 bytes so a 64 x 64 map is 16 KiB. `open` is the slab's
// raised fraction in 1/kOpenScale units; only kSlab reads it.
struct RaycastCell final {
    static constexpr uint16_t kOpenScale = 32768;

    RaycastCellKind kind{RaycastCellKind::kEmpty};
    uint8_t texture_slot{};
    uint16_t open{};

    [[nodiscard]] static constexpr RaycastCell Empty() { return {}; }
    [[nodiscard]] static constexpr RaycastCell Wall(uint8_t texture_slot) {
        return {RaycastCellKind::kWall, texture_slot, 0};
    }
    [[nodiscard]] static constexpr RaycastCell Slab(uint8_t texture_slot, float open_fraction) {
        const float clamped = open_fraction < 0.0F ? 0.0F : (open_fraction > 1.0F ? 1.0F : open_fraction);
        return {RaycastCellKind::kSlab, texture_slot, static_cast<uint16_t>(clamped * kOpenScale + 0.5F)};
    }
    [[nodiscard]] constexpr float open_fraction() const { return static_cast<float>(open) / kOpenScale; }
};
static_assert(sizeof(RaycastCell) == 4, "RaycastCell is read in the DDA inner loop; keep it one word");

// Row-major view of the App's map: cells[y * width + x]. The App owns the
// storage and rewrites cells when walls or slabs change; the Raycaster only
// reads it during Cast(). Coordinates outside the grid read as a wall so a
// ray can never leave the map.
struct RaycastGrid final {
    const RaycastCell* cells{};
    uint16_t width{};
    uint16_t height{};

    [[nodiscard]] RaycastCell At(int x, int y) const {
        if (cells == nullptr || x < 0 || y < 0 || x >= width || y >= height) {
            return RaycastCell::Wall(0);
        }
        return cells[y * width + x];
    }
};

struct RaycastConfig final {
    // Target buffer size in pixels. width <= Raycaster::kMaxColumns.
    int width{};
    int height{};
    // Wall/slab textures are square with side 1 << texture_shift.
    uint8_t texture_shift{7};
    // kRowMajor textures for the floor and ceiling SPAN_PAIR rows.
    uint8_t floor_slot{};
    uint8_t ceiling_slot{};
    DistanceLighting lighting{};
};

class Raycaster final {
   public:
    static constexpr int kMaxColumns = 800;
    static constexpr int kMaxBillboards = 96;
    // A slab raised less than this fraction still hides billboards behind it.
    static constexpr float kSlabBlocksBillboards = 0.5F;

    Raycaster() = default;
    Raycaster(const Raycaster&) = delete;
    Raycaster& operator=(const Raycaster&) = delete;

    // Clamps the view to kMaxColumns and rebuilds the light table. Returns
    // false for an empty view, a zero texture size or an invalid
    // DistanceLighting; the previous configuration is kept.
    [[nodiscard]] bool Initialize(const RaycastConfig& config);
    [[nodiscard]] const RaycastConfig& config() const { return config_; }

    // Runs the geometry for one frame. Only Guest state is touched; nothing is
    // sent to the Host until DrawWorld()/DrawBillboards().
    void Cast(const RaycastCamera& camera, const RaycastGrid& grid);
    // Floor and ceiling rows (skipping pixels hidden by walls), then walls and
    // slabs. Returns false when a record could not be encoded.
    [[nodiscard]] bool DrawWorld(RasterDrawList& list) const;
    // Billboards sorted far to near, each column z-tested against the walls
    // cast by the last Cast(). At most kMaxBillboards are drawn.
    [[nodiscard]] bool DrawBillboards(RasterDrawList& list, std::span<const Billboard> billboards);

    // Perpendicular distance of the nearest occluder in `column` after Cast();
    // a large value where the ray hit nothing.
    [[nodiscard]] float Depth(int column) const;
    [[nodiscard]] uint8_t LightFor(float distance) const { return light_.LightFor(distance); }
    // Projects a map point with the camera of the last Cast(): screen column
    // and perpendicular depth. False when the point is behind the camera.
    [[nodiscard]] bool Project(float x, float y, int& column_out, float& depth_out) const;

   private:
    // One textured column produced by the ray cast and painted after the
    // floor, so the floor pass can skip what it covers.
    struct Slice final {
        int16_t y0{};
        int16_t y1{-1};  // y1 < y0: nothing to paint
        uint16_t u{};
        int32_t v_start{};
        int32_t v_step{};
        uint8_t texture_slot{};
        uint8_t light{};
    };

    [[nodiscard]] bool DrawFloorAndCeiling(RasterDrawList& list) const;
    [[nodiscard]] bool DrawWalls(RasterDrawList& list) const;

    static constexpr int kCoverBlock = 16;
    static constexpr int kCoverBlocks = (kMaxColumns + kCoverBlock - 1) / kCoverBlock;

    RaycastConfig config_{};
    RaycastCamera camera_{};
    int half_height_{};
    float depth_[kMaxColumns]{};
    Slice walls_[kMaxColumns]{};
    Slice slabs_[kMaxColumns]{};
    // Rows [cover_top_, cover_bottom_] of each column are hidden by the far
    // wall; floor and ceiling pixels inside that range are never painted.
    int16_t cover_top_[kMaxColumns]{};
    int16_t cover_bottom_[kMaxColumns]{};
    // Per kCoverBlock columns: min/max of cover_bottom_ and max of cover_top_.
    int16_t block_bottom_min_[kCoverBlocks]{};
    int16_t block_bottom_max_[kCoverBlocks]{};
    int16_t block_top_max_[kCoverBlocks]{};
    LightTable light_{};
    float billboard_depth_[kMaxBillboards]{};
    uint8_t billboard_order_[kMaxBillboards]{};
};

}  // namespace micropixel

#endif
