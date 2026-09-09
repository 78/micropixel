#ifndef MICROPIXEL_APPS_TOMB_EXPLORER_GAME_CHARACTER_HPP
#define MICROPIXEL_APPS_TOMB_EXPLORER_GAME_CHARACTER_HPP

#include <stdint.h>

#include "sdk/mesh_renderer.hpp"

namespace tomb::game {

// Low-polygon explorer: eleven textured boxes in a rigid hierarchy (pelvis,
// torso, head, two arms and two legs of two segments each) animated
// procedurally. Poses drive the joint angles; Submit() places every part.
struct Pose final {
    float walk_phase{};   // radians, advances with distance walked
    float walk_weight{};  // 0 standing, 1 full stride
    float crouch{};       // 0..1, bends knees (jump take-off and landing)
    float airborne{};     // 0..1, arms up and legs tucked
};

class Character final {
   public:
    static constexpr uint32_t kParts = 11U;
    static constexpr float kHeight = 1.7F;
    static constexpr float kRadius = 0.25F;

    Character();

    // Queues the character standing at `position` (feet) facing `yaw`, lit at
    // `brightness` (room light, 0..255), into ordering `group` with `scissor`.
    [[nodiscard]] bool Submit(micropixel::MeshRenderer& renderer, micropixel::Vec3 position, float yaw,
                              const Pose& pose, uint8_t brightness, uint8_t group, micropixel::Rect scissor);

   private:
    struct Part final {
        micropixel::MeshVertex vertices[8];
        micropixel::MeshFace faces[6];
    };

    void BuildBox(Part& part, float width, float height, float depth, float pivot_y, uint8_t texture_slot,
                  uint8_t flat_color);
    void SetBrightness(uint8_t brightness);
    [[nodiscard]] bool SubmitPart(micropixel::MeshRenderer& renderer, uint32_t index,
                                  const micropixel::Transform3& transform,
                                  const micropixel::MeshSubmitOptions& options);

    Part parts_[kParts]{};
    uint8_t brightness_{};
};

}  // namespace tomb::game

#endif
