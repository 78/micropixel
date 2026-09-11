#ifndef GRAVITY_BALLS_PHYSICS_HPP
#define GRAVITY_BALLS_PHYSICS_HPP

#include <algorithm>
#include <array>
#include <cmath>

namespace gravity_balls {
struct Vector final {
    float x{}, y{}, z{};
    Vector operator+(Vector b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vector operator-(Vector b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vector operator*(float s) const { return {x * s, y * s, z * s}; }
    float Dot(Vector b) const { return x * b.x + y * b.y + z * b.z; }
    Vector Cross(Vector b) const { return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x}; }
};
struct Ball final {
    Vector position{}, velocity{};
    float radius{0.42F};
};
// Inertial forces felt inside the box, all in box (device) coordinates.
// `gravity` is minus the accelerometer's specific force: at rest it points
// down, and when the device is jerked it already contains the wall pushing
// the balls. The angular terms add what a rotating frame contributes:
// centrifugal -w x (w x r), Euler -dw x r and Coriolis -2 w x v.
struct Forces final {
    Vector gravity{0, 0, 9.8F * 16.0F};
    Vector angular_velocity{};
    Vector angular_acceleration{};
};
class World final {
   public:
    static constexpr unsigned kMaxCount = 64;
    static constexpr unsigned kDefaultCount = 24;
    // One world unit is 1/kMetersToUnits metre, so the 8.8-unit box behaves
    // like a 55 cm crate of marbles: sensor accelerations in m/s^2 are
    // multiplied by this before they become gravity.
    static constexpr float kMetersToUnits = 16.0F;
    static constexpr float kStep = 1.0F / 240.0F;
    static constexpr float kMaxSpeed = 60.0F;
    // Speed below which a wall or ball contact stops bouncing.
    static constexpr float kRestSpeed = 2.0F;
    // Only the first `count` balls take part; the rest are ignored.
    std::array<Ball, kMaxCount> balls{};
    unsigned count = kDefaultCount;
    // Half sizes of the box; positions are relative to its centre.
    Vector extent{4.4F, 4.4F, 4.5F};
    // Starting slot of ball i: layers of 6 x 4 near the glass, at rest.
    static Ball Spawn(unsigned i) {
        const unsigned layer = i / 24U, slot = i % 24U;
        return {{(static_cast<float>(slot % 6) - 2.5F) * 1.15F, (static_cast<float>(slot / 6) - 1.5F) * 1.15F,
                 -3.6F + 1.15F * static_cast<float>(layer) + 0.3F * (i % 3)},
                {},
                0.36F + 0.035F * (i % 4)};
    }
    void Reset() {
        for (unsigned i = 0; i < count; ++i) balls[i] = Spawn(i);
    }
    // Grows or shrinks the active set; new balls appear in their spawn slots
    // and the existing ones keep moving.
    void SetCount(unsigned n) {
        n = std::min(n, kMaxCount);
        for (unsigned i = count; i < n; ++i) balls[i] = Spawn(i);
        count = n;
    }
    void Kick() {
        for (unsigned i = 0; i < count; ++i) {
            const Vector direction{static_cast<float>(static_cast<int>(i % 5) - 2) * 1.8F,
                                   static_cast<float>(static_cast<int>(i % 7) - 3) * 1.4F, -6.0F};
            balls[i].velocity = direction * (kMetersToUnits * 0.25F);
        }
    }
    static void Collide(Ball& a, Ball& b) {
        Vector d = b.position - a.position;
        const float distance2 = d.Dot(d);
        const float radii = a.radius + b.radius;
        if (distance2 >= radii * radii) return;
        const float distance = std::sqrt(distance2);
        const Vector n = distance > 0.00001F ? d * (1.0F / distance) : Vector{1, 0, 0};
        const float ia = 1.0F / (a.radius * a.radius * a.radius);
        const float ib = 1.0F / (b.radius * b.radius * b.radius);
        const float correction = std::max(0.0F, radii - distance - 0.0001F) * 0.85F / (ia + ib);
        a.position = a.position - n * (correction * ia);
        b.position = b.position + n * (correction * ib);
        const Vector relative = b.velocity - a.velocity;
        const float speed = relative.Dot(n);
        if (speed >= 0) return;
        const float restitution = speed < -kRestSpeed ? 0.65F : 0.0F;
        const float impulse = -(1 + restitution) * speed / (ia + ib);
        const Vector tangent = relative - n * speed;
        const float tangent_length = std::sqrt(tangent.Dot(tangent));
        const float friction =
            tangent_length > 0.00001F ? std::min(0.12F * impulse, tangent_length / (ia + ib)) / tangent_length : 0;
        const Vector impulse_vector = n * impulse - tangent * friction;
        a.velocity = a.velocity - impulse_vector * ia;
        b.velocity = b.velocity + impulse_vector * ib;
    }
    void Step(const Forces& forces) {
        const Vector w = forces.angular_velocity;
        const bool rotating = w.Dot(w) > 1e-6F || forces.angular_acceleration.Dot(forces.angular_acceleration) > 1e-6F;
        for (unsigned i = 0; i < count; ++i) {
            Ball& b = balls[i];
            Vector acceleration = forces.gravity;
            if (rotating) {
                acceleration = acceleration - w.Cross(w.Cross(b.position)) -
                               forces.angular_acceleration.Cross(b.position) - w.Cross(b.velocity) * 2.0F;
            }
            b.velocity = (b.velocity + acceleration * kStep) * 0.9995F;  // faint air drag
            const float speed2 = b.velocity.Dot(b.velocity);
            if (speed2 > kMaxSpeed * kMaxSpeed) b.velocity = b.velocity * (kMaxSpeed / std::sqrt(speed2));
            b.position = b.position + b.velocity * kStep;
        }
        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            for (unsigned i = 0; i < count; ++i)
                for (unsigned j = i + 1; j < count; ++j) Collide(balls[i], balls[j]);
            for (unsigned i = 0; i < count; ++i) {
                Ball& b = balls[i];
                Wall(b.position.x, b.velocity.x, extent.x - b.radius);
                Wall(b.position.y, b.velocity.y, extent.y - b.radius);
                Wall(b.position.z, b.velocity.z, extent.z - b.radius);
            }
        }
    }

   private:
    static void Wall(float& p, float& v, float limit) {
        if (p < -limit) {
            p = -limit;
            if (v < 0) v = v < -kRestSpeed ? -v * 0.55F : 0;
        }
        if (p > limit) {
            p = limit;
            if (v > 0) v = v > kRestSpeed ? -v * 0.55F : 0;
        }
    }
};
}  // namespace gravity_balls
#endif
