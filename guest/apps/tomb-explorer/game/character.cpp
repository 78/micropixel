#include "apps/tomb-explorer/game/character.hpp"

#include "apps/tomb-explorer/game/math.hpp"
#include "apps/tomb-explorer/gfx/palette.hpp"
#include "apps/tomb-explorer/gfx/textures.hpp"

namespace tomb::game {
namespace {

using micropixel::MeshFace;
using micropixel::MeshVertex;
using micropixel::Transform3;
using micropixel::Vec3;

enum PartIndex : uint32_t {
    kPelvis = 0U,
    kTorso,
    kHead,
    kLeftUpperArm,
    kLeftLowerArm,
    kRightUpperArm,
    kRightLowerArm,
    kLeftUpperLeg,
    kLeftLowerLeg,
    kRightUpperLeg,
    kRightLowerLeg,
};
static_assert(kRightLowerLeg + 1U == Character::kParts);

// Faces of an axis-aligned box, counter-clockwise from outside. Corner bits:
// 1 = +x, 2 = +y, 4 = +z.
struct BoxFace final {
    uint8_t corners[4];
    uint8_t light;  // relative brightness 0..255 of this side
};
constexpr BoxFace kBoxFaces[6] = {
    {{2U, 3U, 7U, 6U}, 255U},  // top (+y)
    {{4U, 5U, 1U, 0U}, 120U},  // bottom (-y)
    {{5U, 4U, 6U, 7U}, 230U},  // front (+z), the character's facing side
    {{0U, 1U, 3U, 2U}, 170U},  // back (-z)
    {{1U, 5U, 7U, 3U}, 200U},  // +x
    {{4U, 0U, 2U, 6U}, 200U},  // -x
};

constexpr uint16_t kTex = static_cast<uint16_t>(gfx::kTextureSize);
constexpr uint8_t kNoFlat = 0xFFU;

}  // namespace

Character::Character() {
    const uint8_t skin = gfx::Index(gfx::kSkin, 12U);
    const uint8_t boots = gfx::Index(gfx::kLeather, 8U);
    const uint8_t belt = gfx::Index(gfx::kLeather, 6U);
    // width, height, depth, pivot offset (box centre relative to the joint), texture, flat colour
    BuildBox(parts_[kPelvis], 0.34F, 0.18F, 0.22F, -0.09F, gfx::kTexCloth, belt);
    BuildBox(parts_[kTorso], 0.40F, 0.50F, 0.24F, 0.25F, gfx::kTexCloth, kNoFlat);
    BuildBox(parts_[kHead], 0.22F, 0.26F, 0.22F, 0.15F, gfx::kTexFace, kNoFlat);
    BuildBox(parts_[kLeftUpperArm], 0.12F, 0.30F, 0.12F, -0.15F, gfx::kTexCloth, kNoFlat);
    BuildBox(parts_[kLeftLowerArm], 0.10F, 0.30F, 0.10F, -0.15F, gfx::kTexCloth, skin);
    BuildBox(parts_[kRightUpperArm], 0.12F, 0.30F, 0.12F, -0.15F, gfx::kTexCloth, kNoFlat);
    BuildBox(parts_[kRightLowerArm], 0.10F, 0.30F, 0.10F, -0.15F, gfx::kTexCloth, skin);
    BuildBox(parts_[kLeftUpperLeg], 0.15F, 0.42F, 0.16F, -0.21F, gfx::kTexCloth, kNoFlat);
    BuildBox(parts_[kLeftLowerLeg], 0.14F, 0.42F, 0.14F, -0.21F, gfx::kTexCloth, boots);
    BuildBox(parts_[kRightUpperLeg], 0.15F, 0.42F, 0.16F, -0.21F, gfx::kTexCloth, kNoFlat);
    BuildBox(parts_[kRightLowerLeg], 0.14F, 0.42F, 0.14F, -0.21F, gfx::kTexCloth, boots);
    SetBrightness(255U);
}

void Character::BuildBox(Part& part, float width, float height, float depth, float pivot_y, uint8_t texture_slot,
                         uint8_t flat_color) {
    const float hx = width * 0.5F;
    const float hy = height * 0.5F;
    const float hz = depth * 0.5F;
    for (uint32_t corner = 0U; corner < 8U; ++corner) {
        part.vertices[corner] = {(corner & 1U) != 0U ? hx : -hx, pivot_y + ((corner & 2U) != 0U ? hy : -hy),
                                 (corner & 4U) != 0U ? hz : -hz};
    }
    for (uint32_t side = 0U; side < 6U; ++side) {
        MeshFace& face = part.faces[side];
        for (uint32_t c = 0U; c < 4U; ++c) face.vertex[c] = kBoxFaces[side].corners[c];
        // Bottom-left, bottom-right, top-right, top-left of every side.
        const uint16_t us[4] = {0U, kTex, kTex, 0U};
        const uint16_t vs[4] = {kTex, kTex, 0U, 0U};
        for (uint32_t c = 0U; c < 4U; ++c) {
            face.u[c] = us[c];
            face.v[c] = vs[c];
        }
        if (texture_slot == gfx::kTexFace && side != 2U) {
            // Only the front of the head shows the face; the other sides sample
            // the hair rows at the top of the texture (the underside the skin).
            const uint16_t v0 = side == 1U ? 50U : 2U;
            const uint16_t v1 = side == 1U ? 62U : 16U;
            face.v[0] = v1;
            face.v[1] = v1;
            face.v[2] = v0;
            face.v[3] = v0;
        }
        if (flat_color != kNoFlat) {
            face.flags = micropixel::kMeshFaceFlatColor;
            face.u[0] = flat_color;
        } else {
            face.texture_slot = texture_slot;
        }
    }
}

void Character::SetBrightness(uint8_t brightness) {
    if (brightness == brightness_) return;
    brightness_ = brightness;
    for (Part& part : parts_) {
        for (uint32_t side = 0U; side < 6U; ++side) {
            const uint32_t value = static_cast<uint32_t>(brightness) * kBoxFaces[side].light / 255U;
            for (uint32_t c = 0U; c < 4U; ++c) part.faces[side].brightness[c] = static_cast<uint8_t>(value);
        }
    }
}

bool Character::SubmitPart(micropixel::MeshRenderer& renderer, uint32_t index, const Transform3& transform,
                           const micropixel::MeshSubmitOptions& options) {
    const Part& part = parts_[index];
    return renderer.Submit({std::span<const MeshVertex>(part.vertices, 8U), std::span<const MeshFace>(part.faces, 6U)},
                           transform, options);
}

bool Character::Submit(micropixel::MeshRenderer& renderer, Vec3 position, float yaw, const Pose& pose,
                       uint8_t brightness, uint8_t group, micropixel::Rect scissor) {
    SetBrightness(brightness);
    micropixel::MeshSubmitOptions options{};
    options.group = group;
    options.scissor = scissor;
    // The character stands on large floor quads whose depth key is their centre;
    // a small bias keeps the feet in front of the floor.
    options.depth_bias = -0.3F;

    const float swing = math::Sin(pose.walk_phase) * 0.6F * pose.walk_weight;
    const float knee = (0.5F + 0.5F * math::Sin(pose.walk_phase + 1.2F)) * 0.9F * pose.walk_weight;
    const float bob = math::Fabs(math::Cos(pose.walk_phase)) * 0.04F * pose.walk_weight - pose.crouch * 0.25F;
    const float hip_height = 0.86F + bob;
    const float arm_up = pose.airborne * 1.6F;

    const Transform3 root = Transform3::Uniform(position, yaw);
    const Transform3 pelvis = root * Transform3::Translation({0.0F, hip_height, 0.0F});
    const Transform3 torso = pelvis * Transform3::Translation({0.0F, 0.0F, 0.0F}) *
                             Transform3::RotationX(pose.walk_weight * 0.08F + pose.crouch * 0.3F);
    const Transform3 head = torso * Transform3::Translation({0.0F, 0.52F, 0.0F});

    const auto limb = [&](const Transform3& parent, float x, float y, float angle_upper, float angle_lower,
                          float length) {
        const Transform3 upper = parent * Transform3::Translation({x, y, 0.0F}) * Transform3::RotationX(angle_upper);
        const Transform3 lower =
            upper * Transform3::Translation({0.0F, -length, 0.0F}) * Transform3::RotationX(angle_lower);
        struct Pair {
            Transform3 upper;
            Transform3 lower;
        };
        return Pair{upper, lower};
    };
    // Arms swing opposite to the leg on the same side; elbows stay slightly bent.
    const auto left_arm = limb(torso, -0.26F, 0.45F, -swing * 0.8F - arm_up, -0.35F - pose.airborne * 0.6F, 0.30F);
    const auto right_arm = limb(torso, 0.26F, 0.45F, swing * 0.8F - arm_up, -0.35F - pose.airborne * 0.6F, 0.30F);
    // Knees bend on the leg swinging forward and when crouching or airborne.
    const float tuck = pose.crouch * 0.9F + pose.airborne * 0.7F;
    const auto left_leg =
        limb(pelvis, -0.11F, -0.02F, swing - tuck * 0.5F, knee * (swing > 0.0F ? 1.0F : 0.2F) + tuck, 0.42F);
    const auto right_leg =
        limb(pelvis, 0.11F, -0.02F, -swing - tuck * 0.5F, knee * (swing < 0.0F ? 1.0F : 0.2F) + tuck, 0.42F);

    bool ok = SubmitPart(renderer, kPelvis, pelvis, options);
    ok = SubmitPart(renderer, kTorso, torso, options) && ok;
    ok = SubmitPart(renderer, kHead, head, options) && ok;
    ok = SubmitPart(renderer, kLeftUpperArm, left_arm.upper, options) && ok;
    ok = SubmitPart(renderer, kLeftLowerArm, left_arm.lower, options) && ok;
    ok = SubmitPart(renderer, kRightUpperArm, right_arm.upper, options) && ok;
    ok = SubmitPart(renderer, kRightLowerArm, right_arm.lower, options) && ok;
    ok = SubmitPart(renderer, kLeftUpperLeg, left_leg.upper, options) && ok;
    ok = SubmitPart(renderer, kLeftLowerLeg, left_leg.lower, options) && ok;
    ok = SubmitPart(renderer, kRightUpperLeg, right_leg.upper, options) && ok;
    ok = SubmitPart(renderer, kRightLowerLeg, right_leg.lower, options) && ok;
    return ok;
}

}  // namespace tomb::game
