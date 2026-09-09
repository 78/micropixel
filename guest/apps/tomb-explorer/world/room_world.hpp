#ifndef MICROPIXEL_APPS_TOMB_EXPLORER_WORLD_ROOM_WORLD_HPP
#define MICROPIXEL_APPS_TOMB_EXPLORER_WORLD_ROOM_WORLD_HPP

#include <stdint.h>

#include "apps/tomb-explorer/world/level.hpp"
#include "sdk/mesh_renderer.hpp"

namespace tomb::world {

constexpr uint8_t kNoRoom = 0xFFU;

// A room the camera can see this frame. Rooms are returned far to near, so
// `group` (the MeshRenderer ordering group) simply counts up along the list;
// dynamic objects standing in the room submit with the same group and scissor.
struct VisibleRoom final {
    uint8_t room;
    uint8_t group;
    uint8_t depth;  // portals crossed from the camera room
    micropixel::Rect scissor;
};

// Visibility and collision queries over the static level: which rooms to draw
// through which portal rectangles, and floor/ceiling heights for movement.
// Kept in the App until a second room-based game needs it.
class RoomWorld final {
   public:
    static constexpr uint32_t kMaxVisible = micropixel::MeshRenderer::kMaxGroups;
    static constexpr uint8_t kMaxPortalDepth = 4U;

    void Initialize(const Level& level) { level_ = &level; }
    [[nodiscard]] const Level& level() const { return *level_; }
    [[nodiscard]] const Room& room(uint8_t index) const { return level_->rooms[index]; }

    // Walks the portals from `camera_room` with the renderer's current camera
    // (after MeshRenderer::Begin). `view` is the buffer rectangle. Returns the
    // number of rooms written, farthest first.
    [[nodiscard]] uint32_t ComputeVisible(const micropixel::MeshRenderer& renderer, uint8_t camera_room,
                                          micropixel::Rect view, VisibleRoom* out, uint32_t capacity) const;

    // Room whose footprint holds (x, z), trying `hint` first. kNoRoom outside every room.
    [[nodiscard]] uint8_t RoomAt(float x, float z, uint8_t hint) const;
    // Floor and ceiling height under (x, z) in `room`. False on a solid sector
    // or outside the room.
    [[nodiscard]] bool HeightsAt(uint8_t room, float x, float z, float& floor_out, float& ceiling_out) const;

   private:
    struct Frame final {
        uint8_t room;
        uint8_t from;
        uint8_t depth;
        micropixel::Rect scissor;
    };

    [[nodiscard]] static bool PortalScreenRect(const micropixel::MeshRenderer& renderer, const Portal& portal,
                                               micropixel::Rect scissor, micropixel::Rect& rect_out);

    const Level* level_{};
};

}  // namespace tomb::world

#endif
