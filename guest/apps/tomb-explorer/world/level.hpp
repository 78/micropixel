#ifndef MICROPIXEL_APPS_TOMB_EXPLORER_WORLD_LEVEL_HPP
#define MICROPIXEL_APPS_TOMB_EXPLORER_WORLD_LEVEL_HPP

#include <stdint.h>

#include <span>

#include "sdk/mesh_renderer.hpp"

namespace tomb::world {

// Read-only level produced by tools/generate_level.py from tools/level.json.
//
// A room is a grid of 1 x 1 sectors in the x/z plane with a floor and a
// ceiling height at each sector corner (so floors may slope), its static mesh
// (floor, ceiling, wall and step quads with baked vertex light) and the
// portals that open onto neighbouring rooms. World y is up; a sector column
// with floor == ceiling is solid (a pillar or the room's outline).

constexpr float kSectorSize = 1.0F;

struct Sector final {
    // Corner heights in the order north-west, north-east, south-east,
    // south-west, where north is -z and east is +x.
    float floor[4];
    float ceiling[4];
    bool solid;
};

struct Portal final {
    // World-space quad, counter-clockwise as seen from inside the owning room.
    micropixel::Vec3 corners[4];
    uint8_t target_room;
};

struct Room final {
    // World position of the room's north-west sector corner.
    float origin_x;
    float origin_z;
    uint8_t width;                    // sectors along +x
    uint8_t depth;                    // sectors along +z
    uint8_t ambient;                  // 0..255, used for dynamic objects inside the room
    std::span<const Sector> sectors;  // depth * width, row major from the north
    std::span<const micropixel::MeshVertex> vertices;
    std::span<const micropixel::MeshFace> faces;
    std::span<const Portal> portals;

    [[nodiscard]] micropixel::Mesh mesh() const { return {vertices, faces}; }
    [[nodiscard]] bool Contains(float x, float z) const {
        return x >= origin_x && z >= origin_z && x < origin_x + static_cast<float>(width) * kSectorSize &&
               z < origin_z + static_cast<float>(depth) * kSectorSize;
    }
    [[nodiscard]] const Sector* SectorAt(float x, float z) const {
        if (!Contains(x, z)) return nullptr;
        const int sx = static_cast<int>((x - origin_x) / kSectorSize);
        const int sz = static_cast<int>((z - origin_z) / kSectorSize);
        return &sectors[static_cast<size_t>(sz) * width + static_cast<size_t>(sx)];
    }
};

struct Level final {
    std::span<const Room> rooms;
    // Player spawn.
    micropixel::Vec3 start;
    float start_yaw;
    uint8_t start_room;
};

[[nodiscard]] const Level& TombLevel();

}  // namespace tomb::world

#endif
