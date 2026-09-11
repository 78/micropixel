#include "physics.hpp"

#include <cassert>
#include <cstdio>

using gravity_balls::Ball;
using gravity_balls::Vector;
using gravity_balls::World;
int main() {
    Ball a{{-0.4F, 0, 0}, {4, 0, 0}, 0.5F}, b{{0.4F, 0, 0}, {-4, 0, 0}, 0.5F};
    World::Collide(a, b);
    assert(a.velocity.x < 0 && b.velocity.x > 0);
    assert(std::abs(a.velocity.x + b.velocity.x) < 1e-6F);
    assert(a.velocity.Dot(a.velocity) + b.velocity.Dot(b.velocity) <= 32);
    a = {{0, 0, 0}, {}, 0.5F};
    b = a;
    World::Collide(a, b);
    assert(std::isfinite(a.position.x) && a.position.x < b.position.x);
    World world;
    world.SetCount(World::kMaxCount);
    world.Reset();
    for (unsigned step = 0; step < 36000; ++step) {
        if (step % 1200 == 0) world.Kick();
        world.Step({Vector{8 * std::sin(step * 0.002F), 8 * std::cos(step * 0.003F), 7} * World::kMetersToUnits});
        for (unsigned i = 0; i < world.count; ++i) {
            const auto& ball = world.balls[i];
            assert(std::isfinite(ball.position.x) && std::isfinite(ball.position.y) && std::isfinite(ball.position.z));
            assert(std::isfinite(ball.velocity.Dot(ball.velocity)));
            assert(std::abs(ball.position.x) <= world.extent.x - ball.radius + 1e-5F);
            assert(std::abs(ball.position.y) <= world.extent.y - ball.radius + 1e-5F);
            assert(std::abs(ball.position.z) <= world.extent.z - ball.radius + 1e-5F);
        }
    }
    world.SetCount(World::kDefaultCount);
    world.Reset();
    for (unsigned step = 0; step < 4800; ++step) world.Step({{0, 0, 9.8F * World::kMetersToUnits}});
    for (unsigned i = 0; i < world.count; ++i) {
        const auto& ball = world.balls[i];
        assert(std::abs(ball.position.z - (world.extent.z - ball.radius)) < 0.01F);
        assert(ball.velocity.Dot(ball.velocity) < 0.01F);
    }
    // Spinning the box about z with no gravity flings a resting ball outward
    // (centrifugal), and the Euler term pushes it against the spin-up.
    for (unsigned i = 0; i < world.count; ++i) {
        world.balls[i] = {{-3.0F, -3.5F + 0.3F * static_cast<float>(i), 0}, {}, 0.1F};
    }
    world.balls[0] = {{1.0F, 0, 0}, {}, 0.3F};
    gravity_balls::Forces spin{{0, 0, 0}, {0, 0, 4.0F}, {}};
    world.Step(spin);
    assert(world.balls[0].velocity.x > 0);
    gravity_balls::Forces spin_up{{0, 0, 0}, {}, {0, 0, 10.0F}};
    world.balls[0] = {{1.0F, 0, 0}, {}, 0.3F};
    world.Step(spin_up);
    assert(world.balls[0].velocity.y < 0);
    std::puts("gravity-balls: collision, coincident centers, 64-ball 150s stress, settling, rotation passed");
}
