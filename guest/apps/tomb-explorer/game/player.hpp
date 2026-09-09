#ifndef MICROPIXEL_APPS_TOMB_EXPLORER_GAME_PLAYER_HPP
#define MICROPIXEL_APPS_TOMB_EXPLORER_GAME_PLAYER_HPP

#include <stdint.h>

#include "apps/tomb-explorer/game/character.hpp"
#include "apps/tomb-explorer/world/room_world.hpp"
#include "sdk/mesh_renderer.hpp"

namespace tomb::game {

// Per-frame input in camera-relative terms.
struct Controls final {
    float forward{};  // -1..1, away from the camera
    float strafe{};   // -1..1, camera right
    float orbit{};    // radians to turn the camera around the player this frame
    float tilt{};     // radians to pitch the camera this frame
    bool jump{};
};

// The explorer: walks on the sector floors with step-up, drop and jump
// handling, blocked by walls, solids and low ceilings; moves between rooms
// through portals. Also owns the third-person follow camera.
class Player final {
   public:
    static constexpr float kStepUp = 0.36F;
    static constexpr float kWalkSpeed = 2.4F;
    static constexpr float kGravity = 11.0F;
    static constexpr float kJumpSpeed = 4.4F;

    void Reset(const world::RoomWorld& world);
    void Update(const world::RoomWorld& world, const Controls& controls, float dt);

    [[nodiscard]] micropixel::Vec3 position() const { return position_; }
    [[nodiscard]] float yaw() const { return yaw_; }
    [[nodiscard]] uint8_t room() const { return room_; }
    [[nodiscard]] bool grounded() const { return grounded_; }
    [[nodiscard]] const Pose& pose() const { return pose_; }
    [[nodiscard]] float speed() const { return speed_; }

    // Camera for this frame: orbits the player, pulled in when a wall is in the way.
    [[nodiscard]] micropixel::MeshCamera Camera(const world::RoomWorld& world, float focal_length) const;
    [[nodiscard]] uint8_t camera_room() const { return camera_room_; }
    [[nodiscard]] float camera_yaw() const { return camera_yaw_; }

   private:
    // True when a body of kRadius can stand at (x, z) with its feet at `y`
    // (floor no higher than y + kStepUp and headroom under the ceiling).
    [[nodiscard]] bool CanStand(const world::RoomWorld& world, float x, float z, float y, uint8_t room_hint,
                                float& floor_out) const;
    void MoveHorizontal(const world::RoomWorld& world, float dx, float dz);
    void MoveVertical(const world::RoomWorld& world, float dt, bool jump);
    void PlaceCamera(const world::RoomWorld& world);

    micropixel::Vec3 position_{};
    float yaw_{};
    float speed_{};
    float vertical_speed_{};
    bool grounded_{true};
    uint8_t room_{};
    Pose pose_{};
    float landing_{};

    float camera_yaw_{};
    float camera_pitch_{-0.22F};
    float camera_distance_{2.8F};
    micropixel::Vec3 camera_position_{};
    uint8_t camera_room_{};
};

}  // namespace tomb::game

#endif
