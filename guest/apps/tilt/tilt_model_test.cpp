#include "apps/tilt/tilt_model.hpp"

#include <cassert>

int main() {
    tilt::TiltModel model;
    model.Reset();
    const tilt::PointF start = model.ball();
    for (uint32_t index = 0U; index < 120U; ++index) {
        (void)model.Advance(16667U, {1.0F, 0.0F});
    }
    assert(model.ball().x != start.x || model.ball().y != start.y);
    assert(model.collected_stars() <= tilt::kStarCount);

    for (uint32_t level = 0U; level < tilt::kLevelCount; ++level) {
        model.Reset(level);
        for (uint32_t index = 0U; index < model.level().fan_count; ++index) {
            const tilt::FanFeature fan = model.level().fans[index];
            assert(fan.period_ms > 0U);
            assert(fan.active_ms > 0U && fan.active_ms < fan.period_ms);
        }
        model.SetBallForTesting(model.level().stars[0]);
        const tilt::ModelOutcome star = model.Advance(1U, {});
        assert(star.star_collected);
        assert(model.collected_stars() == 1U);

        if (model.level().pit.radius > 0) {
            model.SetBallForTesting({static_cast<float>(model.level().pit.x), static_cast<float>(model.level().pit.y)});
            const tilt::ModelOutcome fall = model.Advance(1U, {});
            assert(fall.fell);
            assert(model.ball().x == model.level().start.x && model.ball().y == model.level().start.y);
        }

        model.Reset(level);
        for (uint32_t index = 0U; index < tilt::kStarCount; ++index) {
            model.SetBallForTesting(model.level().stars[index]);
            assert(model.Advance(1U, {}).star_collected);
        }
        assert(model.goal_unlocked());
        model.SetBallForTesting({static_cast<float>(model.level().goal.x), static_cast<float>(model.level().goal.y)});
        const tilt::ModelOutcome complete = model.Advance(1U, {});
        assert(complete.completed && model.completed());
    }

    model.Reset(1U);
    const tilt::FanFeature fan = model.level().fans[0];
    const uint32_t until_active_ms = fan.phase_ms < fan.active_ms ? 0U : fan.period_ms - fan.phase_ms;
    model.SetElapsedForTesting(static_cast<uint64_t>(until_active_ms) * 1000U);
    assert(model.fan_active(0U));
    model.SetBallForTesting({static_cast<float>(fan.x), static_cast<float>(fan.y)});
    const tilt::Vec2 opposing_tilt{
        fan.force_x > 0   ? -1.0F
        : fan.force_x < 0 ? 1.0F
                          : 0.0F,
        fan.force_y > 0   ? -1.0F
        : fan.force_y < 0 ? 1.0F
                          : 0.0F,
    };
    (void)model.Advance(16667U, opposing_tilt);
    assert(model.velocity().x * fan.force_x + model.velocity().y * fan.force_y > 0.0F);
    assert(model.velocity().x * model.velocity().x + model.velocity().y * model.velocity().y > 20.0F * 20.0F);

    model.Reset(0U);
    const tilt::PointF bumper_start = model.bumper_position();
    for (uint32_t index = 0U; index < 12U; ++index) {
        (void)model.Advance(66668U, {});
    }
    const tilt::PointF bumper_moved = model.bumper_position();
    assert(bumper_moved.x != bumper_start.x || bumper_moved.y != bumper_start.y);

    const tilt::RectFeature tutorial_ice = model.level().ice;
    const tilt::PointF tutorial_ice_center{
        static_cast<float>(tutorial_ice.x + tutorial_ice.width / 2),
        static_cast<float>(tutorial_ice.y + tutorial_ice.height / 2),
    };
    model.SetBallForTesting(tutorial_ice_center, {50.0F, 0.0F});
    for (uint32_t index = 0U; index < 10U; ++index) {
        (void)model.Advance(16667U, {});
    }
    assert(model.velocity().x > 58.5F);

    model.SetBallForTesting(tutorial_ice_center);
    (void)model.Advance(16667U, {1.0F, 0.0F});
    assert(model.velocity().x > 8.0F && model.velocity().x < 8.5F);

    model.Reset(5U);
    assert(!model.goal_unlocked());
    assert(model.fan_active(0U));
    for (uint32_t index = 0U; index < tilt::kStarCount; ++index) {
        model.SetBallForTesting(model.level().stars[index]);
        assert(model.Advance(1U, {}).star_collected);
        assert(model.goal_unlocked() == (index + 1U == tilt::kStarCount));
    }
    assert(model.goal_unlocked());
    model.SetBallForTesting({static_cast<float>(model.level().goal.x), static_cast<float>(model.level().goal.y)});
    assert(model.Advance(1U, {}).completed);
    assert(model.mastery_rating() == 3U);

    model.Reset(5U);
    for (uint32_t index = 0U; index < tilt::kStarCount; ++index) {
        model.SetBallForTesting(model.level().stars[index]);
        assert(model.Advance(1U, {}).star_collected);
    }
    model.SetBallForTesting({static_cast<float>(model.level().goal.x), static_cast<float>(model.level().goal.y)});
    assert(model.Advance(1U, {}).completed);
    assert(model.mastery_rating() == 3U);

    model.Reset(5U);
    for (uint32_t index = 0U; index < tilt::kStarCount; ++index) {
        model.SetBallForTesting(model.level().stars[index]);
        assert(model.Advance(1U, {}).star_collected);
    }
    model.SetElapsedForTesting(static_cast<uint64_t>(model.level().par_time_ms + 1U) * 1000U);
    model.SetBallForTesting({static_cast<float>(model.level().goal.x), static_cast<float>(model.level().goal.y)});
    assert(model.Advance(1U, {}).completed);
    assert(model.mastery_rating() == 2U);

    model.Reset(5U);
    model.SetElapsedForTesting(static_cast<uint64_t>(model.level().fans[0].active_ms + 100U) * 1000U);
    assert(!model.fan_active(0U));

    model.Reset(7U);
    const tilt::WallRect blocker_start = model.moving_wall_rect();
    model.SetElapsedForTesting(static_cast<uint64_t>(model.level().moving_wall.period_ms) * 500U);
    const tilt::WallRect blocker_end = model.moving_wall_rect();
    assert(blocker_start.x != blocker_end.x || blocker_start.y != blocker_end.y);

    model.Reset(7U);
    assert(model.gate_open());
    model.SetElapsedForTesting(static_cast<uint64_t>(model.level().gate.open_ms + 100U) * 1000U);
    assert(!model.gate_open());

    model.Reset(9U);
    const tilt::CircleFeature pressure_plate = model.level().pressure_gate.plate;
    const tilt::WallRect pressure_door = model.level().pressure_gate.gate;
    const tilt::PointF pressure_door_center{
        static_cast<float>(pressure_door.x + pressure_door.width / 2),
        static_cast<float>(pressure_door.y + pressure_door.height / 2),
    };
    assert(!model.pressure_gate_open());
    model.SetBallForTesting(pressure_door_center);
    (void)model.Advance(1U, {});
    assert(model.ball().x != pressure_door_center.x || model.ball().y != pressure_door_center.y);
    model.SetBallForTesting({static_cast<float>(pressure_plate.x), static_cast<float>(pressure_plate.y)});
    assert(model.Advance(1U, {}).pressure_plate_activated);
    assert(model.pressure_gate_open());
    model.SetBallForTesting(pressure_door_center);
    (void)model.Advance(1U, {});
    assert(model.ball().x == pressure_door_center.x && model.ball().y == pressure_door_center.y);

    model.Reset(8U);
    const tilt::PortalPairFeature portals = model.level().portals;
    model.SetBallForTesting({static_cast<float>(portals.first.x), static_cast<float>(portals.first.y)});
    assert(model.Advance(1U, {}).teleported);
    assert(model.ball().x == static_cast<float>(portals.second.x));
    assert(model.ball().y == static_cast<float>(portals.second.y));
    assert(!model.Advance(1U, {}).teleported);
    model.SetBallForTesting(model.level().start);
    assert(!model.Advance(1U, {}).teleported);
    model.SetBallForTesting({static_cast<float>(portals.second.x), static_cast<float>(portals.second.y)});
    assert(!model.Advance(1U, {}).teleported);
    assert(model.ball().x == static_cast<float>(portals.second.x));
    assert(model.ball().y == static_cast<float>(portals.second.y));

    model.Reset(0U);
    for (uint32_t index = 0U; index < tilt::kStarCount; ++index) {
        model.SetBallForTesting(model.level().stars[index]);
        assert(model.Advance(1U, {}).star_collected);
    }
    model.SetBallForTesting({static_cast<float>(model.level().pit.x), static_cast<float>(model.level().pit.y)});
    assert(model.Advance(1U, {}).fell);
    assert(model.fall_count() == 1U);
    model.SetBallForTesting({static_cast<float>(model.level().goal.x), static_cast<float>(model.level().goal.y)});
    assert(model.Advance(1U, {}).completed);
    assert(model.mastery_rating() == 1U);

    model.Reset();
    model.SetBallForTesting({tilt::kBallRadius, 500.0F});
    for (uint32_t frame = 0U; frame < 60U; ++frame) {
        assert(!model.Advance(16667U, {-1.0F, 0.0F}).wall_hit);
    }

    model.Reset();
    model.SetBallForTesting({22.0F, 500.0F}, {-240.0F, 0.0F});
    bool impact_sound = false;
    for (uint32_t frame = 0U; frame < 2U; ++frame) {
        impact_sound = impact_sound || model.Advance(16667U, {}).wall_hit;
    }
    assert(impact_sound);

    model.Reset(999U);
    assert(model.level_index() == 0U);
    assert(model.elapsed_us() == 0U);
    assert(!model.completed());
    return 0;
}
