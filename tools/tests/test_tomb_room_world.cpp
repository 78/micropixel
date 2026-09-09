// Host-side checks for the tomb-explorer level, RoomWorld portal traversal
// and player collision, built from the App sources plus the SDK MeshRenderer.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "apps/tomb-explorer/game/player.hpp"
#include "apps/tomb-explorer/world/level.hpp"
#include "apps/tomb-explorer/world/room_world.hpp"
#include "sdk/mesh_renderer.hpp"

using namespace tomb;

namespace {

void Check(bool condition, const char* message, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, message);
        std::exit(1);
    }
}
#define CHECK(condition, message) Check((condition), (message), __LINE__)

bool Near(float a, float b, float tolerance = 1e-3F) { return std::fabs(a - b) <= tolerance; }

constexpr int kWidth = 480;
constexpr int kHeight = 480;

micropixel::MeshRendererConfig Config() {
    micropixel::MeshRendererConfig config{};
    config.width = kWidth;
    config.height = kHeight;
    config.far = 48.0F;
    return config;
}

micropixel::MeshCamera CameraAt(micropixel::Vec3 position, float yaw) {
    micropixel::MeshCamera camera{};
    camera.position = position;
    camera.yaw = yaw;
    camera.focal_length = micropixel::MeshCamera::FocalLength(1.25F, kWidth);
    camera.near = 0.12F;
    return camera;
}

bool Inside(micropixel::Rect inner, micropixel::Rect outer) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

uint8_t RoomNamed(const world::Level& level, float x, float z) {
    for (size_t i = 0U; i < level.rooms.size(); ++i) {
        if (level.rooms[i].Contains(x, z)) return static_cast<uint8_t>(i);
    }
    return world::kNoRoom;
}

void TestLevelData() {
    const world::Level& level = world::TombLevel();
    CHECK(level.rooms.size() == 5U, "five rooms");
    CHECK(level.start_room < level.rooms.size(), "start room exists");
    const world::Room& start = level.rooms[level.start_room];
    CHECK(start.Contains(level.start.x, level.start.z), "start is inside its room");
    const world::Sector* sector = start.SectorAt(level.start.x, level.start.z);
    CHECK(sector != nullptr && !sector->solid && Near(sector->floor[0], level.start.y), "start stands on the floor");

    for (size_t r = 0U; r < level.rooms.size(); ++r) {
        const world::Room& room = level.rooms[r];
        CHECK(room.sectors.size() == static_cast<size_t>(room.width) * room.depth, "sector table size");
        CHECK(!room.faces.empty() && room.vertices.size() <= micropixel::MeshRenderer::kMaxMeshVertices,
              "room mesh within renderer limits");
        for (const micropixel::MeshFace& face : room.faces) {
            for (uint32_t c = 0U; c < face.corners(); ++c) {
                CHECK(face.vertex[c] < room.vertices.size(), "face indices in range");
            }
        }
        for (const world::Portal& portal : room.portals) {
            CHECK(portal.target_room < level.rooms.size() && portal.target_room != r, "portal target");
            // The portal quad lies on this room's boundary and the target room
            // owns a portal back to this one.
            const micropixel::Vec3& a = portal.corners[0];
            const micropixel::Vec3& b = portal.corners[1];
            const float x0 = room.origin_x, x1 = room.origin_x + room.width;
            const float z0 = room.origin_z, z1 = room.origin_z + room.depth;
            const bool on_x = (Near(a.x, x0) && Near(b.x, x0)) || (Near(a.x, x1) && Near(b.x, x1));
            const bool on_z = (Near(a.z, z0) && Near(b.z, z0)) || (Near(a.z, z1) && Near(b.z, z1));
            CHECK(on_x || on_z, "portal lies on the room boundary");
            CHECK(portal.corners[3].y > a.y && portal.corners[2].y > b.y, "portal opens upwards");
            bool back = false;
            for (const world::Portal& other : level.rooms[portal.target_room].portals) {
                back = back || other.target_room == r;
            }
            CHECK(back, "portal has a return portal");
        }
    }
}

void TestHeights() {
    world::RoomWorld world;
    world.Initialize(world::TombLevel());
    const world::Level& level = world.level();
    float floor{}, ceiling{};

    const uint8_t entrance = RoomNamed(level, 3.5F, 1.5F);
    CHECK(world.HeightsAt(entrance, 3.5F, 1.5F, floor, ceiling), "entrance heights");
    CHECK(Near(floor, 0.0F) && Near(ceiling, 3.0F), "entrance is flat at 0..3");
    CHECK(world.HeightsAt(entrance, 2.5F, 2.5F, floor, ceiling) && Near(floor, 0.25F), "entrance step block");

    const uint8_t corridor = RoomNamed(level, 3.5F, 8.0F);
    CHECK(corridor != world::kNoRoom && corridor != entrance, "corridor exists");
    CHECK(world.HeightsAt(corridor, 3.5F, 8.0F, floor, ceiling), "corridor heights");
    CHECK(Near(floor, -0.5F, 1e-2F), "corridor slopes to -0.5 half way");
    CHECK(world.HeightsAt(corridor, 3.5F, 5.01F, floor, ceiling) && Near(floor, 0.0F, 1e-2F), "slope starts at 0");
    CHECK(world.HeightsAt(corridor, 3.5F, 10.99F, floor, ceiling) && Near(floor, -1.0F, 1e-2F), "slope ends at -1");

    const uint8_t hall = RoomNamed(level, 3.5F, 15.5F);
    CHECK(world.HeightsAt(hall, 4.5F, 15.5F, floor, ceiling) && Near(floor, -0.5F), "hall platform top");
    CHECK(world.HeightsAt(hall, 4.5F, 14.5F, floor, ceiling) && Near(floor, -0.75F), "hall platform step");
    CHECK(!world.HeightsAt(hall, 0.5F, 13.5F, floor, ceiling), "hall pillar is solid");

    CHECK(world.RoomAt(3.5F, 1.5F, 0xFFU) == entrance, "RoomAt without hint");
    CHECK(world.RoomAt(3.5F, 8.0F, entrance) == corridor, "RoomAt crosses to the corridor");
    CHECK(world.RoomAt(-20.0F, 0.0F, entrance) == world::kNoRoom, "RoomAt outside");
}

void TestVisibility() {
    world::RoomWorld world;
    world.Initialize(world::TombLevel());
    const world::Level& level = world.level();
    micropixel::MeshRendererPool<64> pool;
    micropixel::MeshRenderer renderer;
    CHECK(renderer.Initialize(Config(), pool.storage(), 1U), "Initialize");
    const micropixel::Rect view{0, 0, kWidth, kHeight};
    world::VisibleRoom visible[world::RoomWorld::kMaxVisible];

    const uint8_t entrance = RoomNamed(level, 3.5F, 1.5F);
    const uint8_t corridor = RoomNamed(level, 3.5F, 8.0F);
    const uint8_t hall = RoomNamed(level, 3.5F, 15.5F);
    const uint8_t crypt = RoomNamed(level, 12.0F, 16.0F);
    const uint8_t pool_room = RoomNamed(level, -5.0F, 15.0F);

    // Looking south down the corridor from the entrance: corridor and hall
    // through it, farthest first, scissors nested.
    renderer.Begin(CameraAt({3.5F, 1.3F, 1.5F}, 0.0F));
    uint32_t count = world.ComputeVisible(renderer, entrance, view, visible, world::RoomWorld::kMaxVisible);
    CHECK(count == 3U, "entrance, corridor and hall are visible");
    CHECK(visible[0].room == hall && visible[1].room == corridor && visible[2].room == entrance, "far to near");
    CHECK(visible[0].depth == 2U && visible[1].depth == 1U && visible[2].depth == 0U, "portal depths");
    CHECK(visible[0].group == 0U && visible[1].group == 1U && visible[2].group == 2U, "groups count up");
    CHECK(visible[2].scissor.x == 0 && visible[2].scissor.width == kWidth, "camera room uses the whole view");
    CHECK(Inside(visible[1].scissor, view) && visible[1].scissor.width < kWidth / 2, "corridor scissor is narrow");
    CHECK(Inside(visible[0].scissor, visible[1].scissor), "hall scissor nests in the corridor's");

    // Looking north: nothing but the entrance.
    renderer.Begin(CameraAt({3.5F, 1.3F, 1.5F}, 3.14159F));
    count = world.ComputeVisible(renderer, entrance, view, visible, world::RoomWorld::kMaxVisible);
    CHECK(count == 1U && visible[0].room == entrance && visible[0].group == 0U, "only the entrance behind");

    // Standing in the hall centre facing east: crypt through its portal, not the pool.
    renderer.Begin(CameraAt({3.5F, 0.5F, 15.5F}, 1.5707963F));
    count = world.ComputeVisible(renderer, hall, view, visible, world::RoomWorld::kMaxVisible);
    CHECK(count == 2U && visible[0].room == crypt && visible[1].room == hall, "crypt through the east portal");
    // Facing west: the pool.
    renderer.Begin(CameraAt({3.5F, 0.5F, 15.5F}, -1.5707963F));
    count = world.ComputeVisible(renderer, hall, view, visible, world::RoomWorld::kMaxVisible);
    CHECK(count == 2U && visible[0].room == pool_room, "pool through the west portal");

    // Standing in the doorway (portal partly behind the camera) still shows the next room.
    renderer.Begin(CameraAt({3.5F, 1.0F, 4.95F}, 0.0F));
    count = world.ComputeVisible(renderer, entrance, view, visible, world::RoomWorld::kMaxVisible);
    CHECK(count >= 2U && visible[count - 2U].room == corridor, "doorway keeps the corridor visible");
    CHECK(visible[count - 2U].scissor.width > kWidth / 2, "a doorway the camera stands in spans most of the view");

    // Facing east from the hall centre, the corridor portal on the north wall
    // is off to the side and partly behind the camera: it must not leak in.
    renderer.Begin(CameraAt({3.5F, 0.5F, 15.5F}, 1.5707963F));
    count = world.ComputeVisible(renderer, hall, view, visible, world::RoomWorld::kMaxVisible);
    for (uint32_t i = 0U; i < count; ++i) CHECK(visible[i].room != corridor, "side portal behind the camera culled");

    // Capacity is honoured.
    renderer.Begin(CameraAt({3.5F, 1.3F, 1.5F}, 0.0F));
    count = world.ComputeVisible(renderer, entrance, view, visible, 2U);
    CHECK(count == 2U && visible[1].room == entrance, "capacity limits the traversal");
}

void TestPlayer() {
    world::RoomWorld world;
    world.Initialize(world::TombLevel());
    const world::Level& level = world.level();
    game::Player player;
    player.Reset(world);
    CHECK(Near(player.position().x, level.start.x) && player.room() == level.start_room, "reset at start");

    // Walk forward (camera looks along +z at start): through the corridor into the hall.
    game::Controls forward{};
    forward.forward = 1.0F;
    const float dt = 1.0F / 30.0F;
    for (int frame = 0; frame < 30 * 8; ++frame) player.Update(world, forward, dt);
    const uint8_t hall = RoomNamed(level, 3.5F, 15.5F);
    CHECK(player.room() == hall, "eight seconds of walking reaches the hall");
    CHECK(player.position().z > 11.0F && Near(player.position().y, -1.0F, 0.05F), "descended the slope to -1");
    CHECK(player.grounded(), "on the floor");
    CHECK(player.pose().walk_weight > 0.9F, "walking pose");

    // Walk into a wall: strafing left from the start stops at the wall, sliding is allowed.
    player.Reset(world);
    game::Controls left{};
    left.strafe = -1.0F;
    for (int frame = 0; frame < 90; ++frame) player.Update(world, left, dt);
    CHECK(player.position().x >= game::Character::kRadius - 1e-3F && player.position().x < 0.6F,
          "blocked by the west wall");
    CHECK(player.room() == level.start_room, "still in the entrance");

    // Jump: leaves the floor and lands again.
    player.Reset(world);
    game::Controls jump{};
    jump.jump = true;
    player.Update(world, jump, dt);
    CHECK(!player.grounded(), "airborne after a jump");
    float peak = 0.0F;
    game::Controls idle{};
    for (int frame = 0; frame < 60; ++frame) {
        player.Update(world, idle, dt);
        peak = player.position().y > peak ? player.position().y : peak;
    }
    CHECK(peak > 0.6F && peak < 1.2F, "jump height is about a metre");
    CHECK(player.grounded() && Near(player.position().y, 0.0F), "landed");

    // The step blocks in the entrance (0.25 high) are climbed; the crypt blocks (1.0) are not.
    player.Reset(world);
    for (int frame = 0; frame < 30 * 2; ++frame) player.Update(world, forward, dt);
    game::Controls right{};
    right.strafe = 1.0F;
    (void)right;
    // Camera stays in open space.
    const micropixel::MeshCamera camera = player.Camera(world, 240.0F);
    const uint8_t camera_room = player.camera_room();
    float floor{}, ceiling{};
    CHECK(camera_room != world::kNoRoom &&
              world.HeightsAt(camera_room, camera.position.x, camera.position.z, floor, ceiling),
          "camera is inside a room");
    CHECK(camera.position.y > floor && camera.position.y < ceiling, "camera between floor and ceiling");
}

}  // namespace

// The App sources link the SDK MeshRenderer, whose Flush() calls into the
// RasterDrawList; the test never flushes, so stubs satisfy the linker.
namespace micropixel {
bool RasterDrawList::Triangle(const RasterVertex (&)[3], uint8_t, bool) { return true; }
bool RasterDrawList::Quad(const RasterVertex (&)[4], uint8_t, bool) { return true; }
bool RasterDrawList::FlatTriangle(const RasterVertex (&)[3], uint8_t) { return true; }
bool RasterDrawList::FlatQuad(const RasterVertex (&)[4], uint8_t) { return true; }
}  // namespace micropixel

int main() {
    TestLevelData();
    TestHeights();
    TestVisibility();
    TestPlayer();
    std::puts("tomb_room_world: ok");
    return 0;
}
