#include "apps/tomb-explorer/game/player.hpp"

#include "apps/tomb-explorer/game/math.hpp"

namespace tomb::game {
namespace {

using micropixel::Vec3;

constexpr float kHeadroom = 1.75F;
constexpr float kCameraHeight = 1.35F;  // look-at point above the feet
constexpr float kCameraMinDistance = 0.6F;
constexpr float kCameraFollowRate = 2.2F;  // radians per second the orbit drifts behind the player
constexpr float kMaxSlopeSnap = 0.5F;      // floor rise per frame that counts as a slope, not a fall

}  // namespace

void Player::Reset(const world::RoomWorld& world) {
    const world::Level& level = world.level();
    position_ = level.start;
    yaw_ = level.start_yaw;
    room_ = level.start_room;
    speed_ = 0.0F;
    vertical_speed_ = 0.0F;
    grounded_ = true;
    pose_ = Pose{};
    landing_ = 0.0F;
    camera_yaw_ = yaw_;
    camera_pitch_ = -0.22F;
    camera_distance_ = 2.8F;
    PlaceCamera(world);
}

bool Player::CanStand(const world::RoomWorld& world, float x, float z, float y, uint8_t room_hint,
                      float& floor_out) const {
    // The centre and four points on the body's radius must all be on open
    // floor no more than a step above the feet, with headroom.
    constexpr float kOffsets[5][2] = {{0.0F, 0.0F},
                                      {Character::kRadius, 0.0F},
                                      {-Character::kRadius, 0.0F},
                                      {0.0F, Character::kRadius},
                                      {0.0F, -Character::kRadius}};
    float highest_floor = -1e9F;
    for (const auto& offset : kOffsets) {
        const float px = x + offset[0];
        const float pz = z + offset[1];
        const uint8_t room = world.RoomAt(px, pz, room_hint);
        float floor{}, ceiling{};
        if (room == world::kNoRoom || !world.HeightsAt(room, px, pz, floor, ceiling)) return false;
        if (floor > y + kStepUp) return false;
        if (ceiling - (floor > y ? floor : y) < kHeadroom) return false;
        highest_floor = floor > highest_floor ? floor : highest_floor;
    }
    floor_out = highest_floor;
    return true;
}

void Player::MoveHorizontal(const world::RoomWorld& world, float dx, float dz) {
    float floor{};
    const auto try_move = [&](float mx, float mz) {
        const float nx = position_.x + mx;
        const float nz = position_.z + mz;
        if (!CanStand(world, nx, nz, position_.y, room_, floor)) return false;
        position_.x = nx;
        position_.z = nz;
        return true;
    };
    if (!try_move(dx, dz)) {
        // Slide along the wall: keep whichever axis is free.
        if (!try_move(dx, 0.0F)) {
            (void)try_move(0.0F, dz);
        }
    }
    room_ = world.RoomAt(position_.x, position_.z, room_);
}

void Player::MoveVertical(const world::RoomWorld& world, float dt, bool jump) {
    float floor{}, ceiling{};
    if (!world.HeightsAt(room_, position_.x, position_.z, floor, ceiling)) {
        // Should not happen after a successful horizontal move; stay put.
        return;
    }
    if (grounded_ && jump) {
        vertical_speed_ = kJumpSpeed;
        grounded_ = false;
    }
    if (grounded_) {
        // Follow slopes and steps; a floor well below the feet starts a fall.
        if (floor >= position_.y - kMaxSlopeSnap) {
            position_.y = floor;
            vertical_speed_ = 0.0F;
        } else {
            grounded_ = false;
        }
    }
    if (!grounded_) {
        vertical_speed_ -= kGravity * dt;
        position_.y += vertical_speed_ * dt;
        if (position_.y + kHeadroom > ceiling && vertical_speed_ > 0.0F) {
            position_.y = ceiling - kHeadroom;
            vertical_speed_ = 0.0F;
        }
        if (position_.y <= floor) {
            position_.y = floor;
            grounded_ = true;
            landing_ = vertical_speed_ < -3.0F ? 0.25F : 0.1F;
            vertical_speed_ = 0.0F;
        }
    }
}

void Player::Update(const world::RoomWorld& world, const Controls& controls, float dt) {
    camera_yaw_ = math::WrapAngle(camera_yaw_ + controls.orbit);
    camera_pitch_ = math::Clamp(camera_pitch_ + controls.tilt, -0.9F, 0.35F);

    // Stick direction is relative to the camera; the character turns to face it.
    const float magnitude_sq = controls.forward * controls.forward + controls.strafe * controls.strafe;
    float target_speed = 0.0F;
    if (magnitude_sq > 0.01F) {
        float magnitude = math::Sqrt(magnitude_sq);
        if (magnitude > 1.0F) magnitude = 1.0F;
        // A heading of 0 walks along +z; stick right (+strafe) turns towards +x,
        // measured from the camera's own yaw.
        const float heading = math::WrapAngle(camera_yaw_ + math::Atan2(controls.strafe, controls.forward));
        yaw_ = math::ApproachAngle(yaw_, heading, 9.0F * dt);
        target_speed = kWalkSpeed * magnitude;
        // Accelerate quickly, and move along the current facing so turns feel weighty.
        speed_ += (target_speed - speed_) * math::Clamp(dt * 10.0F, 0.0F, 1.0F);
    } else {
        speed_ += (0.0F - speed_) * math::Clamp(dt * 12.0F, 0.0F, 1.0F);
        if (speed_ < 0.05F) speed_ = 0.0F;
    }
    if (speed_ > 0.0F) {
        MoveHorizontal(world, math::Sin(yaw_) * speed_ * dt, math::Cos(yaw_) * speed_ * dt);
    }
    MoveVertical(world, dt, controls.jump);

    // Pose: stride phase from distance walked, weights eased towards their targets.
    pose_.walk_phase = math::WrapAngle(pose_.walk_phase + speed_ * dt * 5.5F);
    const float walk_target = grounded_ ? math::Clamp(speed_ / kWalkSpeed, 0.0F, 1.0F) : 0.0F;
    pose_.walk_weight += (walk_target - pose_.walk_weight) * math::Clamp(dt * 8.0F, 0.0F, 1.0F);
    const float air_target = grounded_ ? 0.0F : 1.0F;
    pose_.airborne += (air_target - pose_.airborne) * math::Clamp(dt * 10.0F, 0.0F, 1.0F);
    landing_ = landing_ > dt ? landing_ - dt : 0.0F;
    const float crouch_target = landing_ > 0.0F ? 1.0F : 0.0F;
    pose_.crouch += (crouch_target - pose_.crouch) * math::Clamp(dt * 14.0F, 0.0F, 1.0F);

    // The orbit drifts behind the character while walking, unless the player
    // is steering it.
    // Pure strafing (facing across the view) leaves the camera alone so the
    // player can circle an object without the view spinning.
    if (controls.orbit == 0.0F && speed_ > 0.3F && math::Fabs(math::WrapAngle(yaw_ - camera_yaw_)) < 1.1F) {
        camera_yaw_ = math::ApproachAngle(camera_yaw_, yaw_, kCameraFollowRate * dt * math::Clamp(speed_, 0.0F, 1.0F));
    }
    PlaceCamera(world);
}

void Player::PlaceCamera(const world::RoomWorld& world) {
    const Vec3 target = position_ + Vec3{0.0F, kCameraHeight, 0.0F};
    const float cos_pitch = math::Cos(camera_pitch_);
    const Vec3 back{-math::Sin(camera_yaw_) * cos_pitch, -math::Sin(camera_pitch_),
                    -math::Cos(camera_yaw_) * cos_pitch};
    // March out from the look-at point and stop before leaving open space.
    constexpr int kSteps = 14;
    Vec3 best = target + back * kCameraMinDistance;
    uint8_t best_room = world.RoomAt(best.x, best.z, room_);
    if (best_room == world::kNoRoom) {
        best_room = room_;
    }
    for (int step = 1; step <= kSteps; ++step) {
        const float d = kCameraMinDistance +
                        (camera_distance_ - kCameraMinDistance) * static_cast<float>(step) / static_cast<float>(kSteps);
        const Vec3 candidate = target + back * d;
        const uint8_t room = world.RoomAt(candidate.x, candidate.z, best_room);
        float floor{}, ceiling{};
        if (room == world::kNoRoom || !world.HeightsAt(room, candidate.x, candidate.z, floor, ceiling) ||
            candidate.y < floor + 0.15F || candidate.y > ceiling - 0.1F) {
            break;
        }
        best = candidate;
        best_room = room;
    }
    camera_position_ = best;
    camera_room_ = best_room;
}

micropixel::MeshCamera Player::Camera(const world::RoomWorld& world, float focal_length) const {
    (void)world;
    micropixel::MeshCamera camera{};
    camera.position = camera_position_;
    camera.yaw = camera_yaw_;
    camera.pitch = camera_pitch_;
    camera.focal_length = focal_length;
    camera.near = 0.12F;
    return camera;
}

}  // namespace tomb::game
