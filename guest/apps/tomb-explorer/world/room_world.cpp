#include "apps/tomb-explorer/world/room_world.hpp"

namespace tomb::world {
namespace {

// Camera distance to a portal plane below which the portal covers the view.
constexpr float kDoorwayDistance = 0.6F;

[[nodiscard]] micropixel::Rect Intersect(micropixel::Rect a, micropixel::Rect b) {
    const int32_t x0 = a.x > b.x ? a.x : b.x;
    const int32_t y0 = a.y > b.y ? a.y : b.y;
    const int32_t x1 = (a.x + a.width) < (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
    const int32_t y1 = (a.y + a.height) < (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

[[nodiscard]] int32_t FloorInt(float value) {
    const auto truncated = static_cast<int32_t>(value);
    return static_cast<float>(truncated) > value ? truncated - 1 : truncated;
}

}  // namespace

bool RoomWorld::PortalScreenRect(const micropixel::MeshRenderer& renderer, const Portal& portal,
                                 micropixel::Rect scissor, micropixel::Rect& rect_out) {
    // The portal faces into its room; a camera on or beyond its plane cannot
    // look through it from this side.
    const micropixel::Vec3 edge_right = portal.corners[1] - portal.corners[0];
    const micropixel::Vec3 edge_up = portal.corners[3] - portal.corners[0];
    const micropixel::Vec3 normal = edge_right.Cross(edge_up);  // points away from the room
    const float signed_distance = (renderer.camera().position - portal.corners[0]).Dot(normal) / normal.Length();
    if (signed_distance >= 0.0F) return false;
    if (-signed_distance < kDoorwayDistance) {
        // The camera is about to pass through: the near plane may already cut
        // into the next room, so it gets the whole current view.
        rect_out = scissor;
        return true;
    }

    // Clip the quad against the near plane in view space, then project what is
    // left: a doorway the camera stands in still yields a tight rectangle.
    micropixel::Vec3 view[4];
    for (uint32_t c = 0U; c < 4U; ++c) view[c] = renderer.ToView(portal.corners[c]);
    const float near = renderer.camera().near;
    micropixel::Vec3 clipped[8];
    uint32_t count = 0U;
    for (uint32_t c = 0U; c < 4U; ++c) {
        const micropixel::Vec3& a = view[c];
        const micropixel::Vec3& b = view[(c + 1U) & 3U];
        const bool a_in = a.z >= near;
        const bool b_in = b.z >= near;
        if (a_in) clipped[count++] = a;
        if (a_in != b_in) {
            const float t = (near - a.z) / (b.z - a.z);
            micropixel::Vec3 m = a + (b - a) * t;
            m.z = near;
            clipped[count++] = m;
        }
    }
    if (count < 3U) return false;
    const float focal = renderer.camera().focal_length;
    const float center_x = static_cast<float>(renderer.config().width) * 0.5F;
    const float center_y = static_cast<float>(renderer.config().height) * 0.5F;
    float min_x = 1e9F, max_x = -1e9F, min_y = 1e9F, max_y = -1e9F;
    for (uint32_t c = 0U; c < count; ++c) {
        const float scale = focal / clipped[c].z;
        const float x = center_x + clipped[c].x * scale;
        const float y = center_y - clipped[c].y * scale;
        min_x = x < min_x ? x : min_x;
        max_x = x > max_x ? x : max_x;
        min_y = y < min_y ? y : min_y;
        max_y = y > max_y ? y : max_y;
    }
    // Clamp before converting: a corner at the near plane can project far out.
    const float limit = 1e6F;
    min_x = min_x < -limit ? -limit : min_x;
    min_y = min_y < -limit ? -limit : min_y;
    max_x = max_x > limit ? limit : max_x;
    max_y = max_y > limit ? limit : max_y;
    const micropixel::Rect bounds{FloorInt(min_x), FloorInt(min_y), FloorInt(max_x) - FloorInt(min_x) + 1,
                                  FloorInt(max_y) - FloorInt(min_y) + 1};
    rect_out = Intersect(bounds, scissor);
    return rect_out.width > 0 && rect_out.height > 0;
}

uint32_t RoomWorld::ComputeVisible(const micropixel::MeshRenderer& renderer, uint8_t camera_room, micropixel::Rect view,
                                   VisibleRoom* out, uint32_t capacity) const {
    if (level_ == nullptr || camera_room >= level_->rooms.size() || capacity == 0U) return 0U;
    if (capacity > kMaxVisible) capacity = kMaxVisible;

    // Breadth-first over portals: `frames` doubles as the queue and the visit
    // list, so each room is entered once, through the nearest portal chain.
    Frame frames[kMaxVisible];
    uint32_t count = 1U;
    frames[0] = Frame{camera_room, kNoRoom, 0U, view};
    for (uint32_t head = 0U; head < count && count < capacity; ++head) {
        const Frame frame = frames[head];
        if (frame.depth >= kMaxPortalDepth) continue;
        for (const Portal& portal : level_->rooms[frame.room].portals) {
            if (count >= capacity) break;
            bool visited = false;
            for (uint32_t i = 0U; i < count; ++i) visited = visited || frames[i].room == portal.target_room;
            if (visited) continue;
            micropixel::Rect rect{};
            if (!PortalScreenRect(renderer, portal, frame.scissor, rect)) continue;
            frames[count++] = Frame{portal.target_room, frame.room, static_cast<uint8_t>(frame.depth + 1U), rect};
        }
    }
    // Farthest first: reverse breadth-first order (depth never decreases along
    // the queue), so every room gets a group after the rooms seen through it.
    for (uint32_t i = 0U; i < count; ++i) {
        const Frame& frame = frames[count - 1U - i];
        out[i] = VisibleRoom{frame.room, static_cast<uint8_t>(i), frame.depth, frame.scissor};
    }
    return count;
}

uint8_t RoomWorld::RoomAt(float x, float z, uint8_t hint) const {
    if (level_ == nullptr) return kNoRoom;
    if (hint < level_->rooms.size() && level_->rooms[hint].Contains(x, z)) return hint;
    for (size_t index = 0U; index < level_->rooms.size(); ++index) {
        if (level_->rooms[index].Contains(x, z)) return static_cast<uint8_t>(index);
    }
    return kNoRoom;
}

bool RoomWorld::HeightsAt(uint8_t room, float x, float z, float& floor_out, float& ceiling_out) const {
    if (level_ == nullptr || room >= level_->rooms.size()) return false;
    const Room& r = level_->rooms[room];
    const Sector* sector = r.SectorAt(x, z);
    if (sector == nullptr || sector->solid) return false;
    const float local_x = (x - r.origin_x) / kSectorSize;
    const float local_z = (z - r.origin_z) / kSectorSize;
    const float fx = local_x - static_cast<float>(FloorInt(local_x));
    const float fz = local_z - static_cast<float>(FloorInt(local_z));
    const auto bilinear = [fx, fz](const float(&h)[4]) {
        const float north = h[0] * (1.0F - fx) + h[1] * fx;
        const float south = h[3] * (1.0F - fx) + h[2] * fx;
        return north * (1.0F - fz) + south * fz;
    };
    floor_out = bilinear(sector->floor);
    ceiling_out = bilinear(sector->ceiling);
    return true;
}

}  // namespace tomb::world
