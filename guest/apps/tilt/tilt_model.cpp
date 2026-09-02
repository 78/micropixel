#include "apps/tilt/tilt_model.hpp"

namespace tilt {
namespace {

constexpr float kMaximumSpeed = 420.0F;
constexpr float kTiltAcceleration = 820.0F;
constexpr float kPulsedFanMinimumScale = 0.94F;
constexpr float kPulsedFanEntryImpulse = 20.0F;
constexpr float kNormalDampingPerStep = 0.985F;
constexpr float kIceDampingPerStep = 0.9995F;
constexpr float kIceEntrySpeedMultiplier = 1.18F;
constexpr float kIceControlScale = 0.60F;
constexpr float kWallRestitution = 0.48F;
constexpr float kBumperRestitution = 1.12F;
constexpr float kWallSoundImpactSpeed = 70.0F;
constexpr uint64_t kMaximumDeltaUs = 66668U;

[[nodiscard]] float Square(float value) { return value * value; }

[[nodiscard]] float LengthSquared(float x, float y) { return Square(x) + Square(y); }

[[nodiscard]] float Sqrt(float value) { return __builtin_sqrtf(value); }

[[nodiscard]] float Closest(float value, float minimum, float maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

[[nodiscard]] bool Overlaps(RectFeature rect, PointF point, float radius) {
    if (rect.width <= 0 || rect.height <= 0) {
        return false;
    }
    const float closest_x = Closest(point.x, static_cast<float>(rect.x), static_cast<float>(rect.x + rect.width));
    const float closest_y = Closest(point.y, static_cast<float>(rect.y), static_cast<float>(rect.y + rect.height));
    return LengthSquared(point.x - closest_x, point.y - closest_y) <= radius * radius;
}

}  // namespace

void TiltModel::Reset(uint32_t level_index) {
    level_index_ = level_index < kLevelCount ? level_index : 0U;
    elapsed_us_ = 0U;
    dynamic_phase_offset_us_ = 0U;
    collected_star_mask_ = 0U;
    collected_stars_ = 0U;
    fall_count_ = 0U;
    pressure_gate_open_ = false;
    portal_armed_ = true;
    was_on_ice_ = false;
    completed_ = false;
    ResetBall();
}

void TiltModel::ResetBall() {
    ball_ = level().start;
    velocity_ = {};
    was_on_ice_ = false;
    for (uint32_t index = 0U; index < kMaximumFanCount; ++index) {
        was_in_active_fan_[index] = false;
    }
}

PointF TiltModel::bumper_position() const {
    const PointF start{static_cast<float>(level().bumper.x), static_cast<float>(level().bumper.y)};
    if (level().bumper_period_ms == 0U) {
        return start;
    }
    const uint64_t period_us = static_cast<uint64_t>(level().bumper_period_ms) * 1000U;
    const float cycle =
        static_cast<float>((elapsed_us_ + dynamic_phase_offset_us_) % period_us) / static_cast<float>(period_us);
    const float progress = cycle <= 0.5F ? cycle * 2.0F : (1.0F - cycle) * 2.0F;
    return {start.x + (level().bumper_end.x - start.x) * progress,
            start.y + (level().bumper_end.y - start.y) * progress};
}

WallRect TiltModel::moving_wall_rect() const {
    const MovingWallFeature& moving = level().moving_wall;
    if (moving.start.width == 0 || moving.start.height == 0 || moving.period_ms == 0U) {
        return moving.start;
    }
    const uint64_t period_us = static_cast<uint64_t>(moving.period_ms) * 1000U;
    const float cycle =
        static_cast<float>((elapsed_us_ + dynamic_phase_offset_us_) % period_us) / static_cast<float>(period_us);
    const float progress = cycle <= 0.5F ? cycle * 2.0F : (1.0F - cycle) * 2.0F;
    return {static_cast<int16_t>(RoundToInt(static_cast<float>(moving.start.x) +
                                            (moving.end.x - static_cast<float>(moving.start.x)) * progress)),
            static_cast<int16_t>(RoundToInt(static_cast<float>(moving.start.y) +
                                            (moving.end.y - static_cast<float>(moving.start.y)) * progress)),
            moving.start.width, moving.start.height};
}

bool TiltModel::fan_active(uint32_t index) const {
    if (index >= level().fan_count) {
        return false;
    }
    const FanFeature fan = level().fans[index];
    if (fan.period_ms == 0U) {
        return true;
    }
    const uint64_t elapsed_ms = (elapsed_us_ + dynamic_phase_offset_us_) / 1000U;
    return (elapsed_ms + fan.phase_ms) % fan.period_ms < fan.active_ms;
}

bool TiltModel::gate_open() const {
    const TimedGateFeature& gate = level().gate;
    if (gate.rect.width == 0 || gate.rect.height == 0 || gate.period_ms == 0U) {
        return true;
    }
    const uint64_t elapsed_ms = (elapsed_us_ + dynamic_phase_offset_us_) / 1000U;
    return (elapsed_ms + gate.phase_ms) % gate.period_ms < gate.open_ms;
}

uint32_t TiltModel::mastery_rating() const {
    if (!completed_) {
        return 0U;
    }
    uint32_t rating = 1U;
    if (collected_stars_ == kStarCount && fall_count_ == 0U) {
        rating = 2U;
        if (level().par_time_ms == 0U || elapsed_us_ / 1000U <= level().par_time_ms) {
            rating = 3U;
        }
    }
    return rating;
}

void TiltModel::ApplyFans(float step_seconds) {
    for (uint32_t index = 0U; index < level().fan_count; ++index) {
        if (!fan_active(index)) {
            was_in_active_fan_[index] = false;
            continue;
        }
        const FanFeature fan = level().fans[index];
        const float dx = ball_.x - static_cast<float>(fan.x);
        const float dy = ball_.y - static_cast<float>(fan.y);
        const float distance_squared = LengthSquared(dx, dy);
        const float radius = static_cast<float>(fan.radius);
        if (distance_squared >= radius * radius) {
            was_in_active_fan_[index] = false;
            continue;
        }
        const float distance = distance_squared > 0.0001F ? Sqrt(distance_squared) : 0.0F;
        const float falloff = 1.0F - distance / radius;
        // Always-on fans remain a gentle teaching mechanic. Pulsed fans are
        // deliberate gust zones: while active, their force stays strong across
        // the marked radius so the player cannot bypass the timing challenge by
        // skimming its edge.
        const float force_scale =
            fan.period_ms == 0U ? falloff : kPulsedFanMinimumScale + (1.0F - kPulsedFanMinimumScale) * falloff;
        if (fan.period_ms != 0U && !was_in_active_fan_[index]) {
            const float force_magnitude =
                Sqrt(LengthSquared(static_cast<float>(fan.force_x), static_cast<float>(fan.force_y)));
            if (force_magnitude > 0.0F) {
                velocity_.x += static_cast<float>(fan.force_x) / force_magnitude * kPulsedFanEntryImpulse;
                velocity_.y += static_cast<float>(fan.force_y) / force_magnitude * kPulsedFanEntryImpulse;
            }
        }
        was_in_active_fan_[index] = true;
        velocity_.x += static_cast<float>(fan.force_x) * force_scale * step_seconds;
        velocity_.y += static_cast<float>(fan.force_y) * force_scale * step_seconds;
    }
}

void TiltModel::UpdatePressureGate(ModelOutcome& outcome) {
    const CircleFeature plate = level().pressure_gate.plate;
    if (pressure_gate_open_ || plate.radius <= 0) {
        return;
    }
    const float dx = ball_.x - static_cast<float>(plate.x);
    const float dy = ball_.y - static_cast<float>(plate.y);
    if (LengthSquared(dx, dy) > Square(static_cast<float>(plate.radius) + kBallRadius * 0.35F)) {
        return;
    }
    pressure_gate_open_ = true;
    outcome.pressure_plate_activated = true;
}

void TiltModel::UpdatePortals(ModelOutcome& outcome) {
    const PortalPairFeature portals = level().portals;
    if (portals.first.radius <= 0 || portals.second.radius <= 0) {
        return;
    }
    const auto inside = [&](CircleFeature portal) {
        const float dx = ball_.x - static_cast<float>(portal.x);
        const float dy = ball_.y - static_cast<float>(portal.y);
        return LengthSquared(dx, dy) <= Square(static_cast<float>(portal.radius));
    };
    const bool inside_first = inside(portals.first);
    const bool inside_second = inside(portals.second);
    if (!portal_armed_) {
        portal_armed_ = inside_first || inside_second ? false : true;
        return;
    }
    if (!inside_first) {
        return;
    }
    const CircleFeature destination = portals.second;
    ball_ = {static_cast<float>(destination.x), static_cast<float>(destination.y)};
    velocity_.x *= 0.72F;
    velocity_.y *= 0.72F;
    portal_armed_ = false;
    outcome.teleported = true;
}

bool TiltModel::ResolveWall(const WallRect& wall, ModelOutcome& outcome) {
    const float left = static_cast<float>(wall.x);
    const float top = static_cast<float>(wall.y);
    const float right = static_cast<float>(wall.x + wall.width);
    const float bottom = static_cast<float>(wall.y + wall.height);
    const float closest_x = Closest(ball_.x, left, right);
    const float closest_y = Closest(ball_.y, top, bottom);
    float dx = ball_.x - closest_x;
    float dy = ball_.y - closest_y;
    float distance_squared = LengthSquared(dx, dy);
    if (distance_squared >= kBallRadius * kBallRadius) {
        return false;
    }

    float normal_x = 0.0F;
    float normal_y = 0.0F;
    float penetration = 0.0F;
    if (distance_squared > 0.0001F) {
        const float distance = Sqrt(distance_squared);
        normal_x = dx / distance;
        normal_y = dy / distance;
        penetration = kBallRadius - distance;
    } else {
        const float to_left = ball_.x - left;
        const float to_right = right - ball_.x;
        const float to_top = ball_.y - top;
        const float to_bottom = bottom - ball_.y;
        float minimum = to_left;
        normal_x = -1.0F;
        penetration = kBallRadius + to_left;
        if (to_right < minimum) {
            minimum = to_right;
            normal_x = 1.0F;
            normal_y = 0.0F;
            penetration = kBallRadius + to_right;
        }
        if (to_top < minimum) {
            minimum = to_top;
            normal_x = 0.0F;
            normal_y = -1.0F;
            penetration = kBallRadius + to_top;
        }
        if (to_bottom < minimum) {
            normal_x = 0.0F;
            normal_y = 1.0F;
            penetration = kBallRadius + to_bottom;
        }
    }
    ball_.x += normal_x * penetration;
    ball_.y += normal_y * penetration;
    const float normal_speed = velocity_.x * normal_x + velocity_.y * normal_y;
    if (normal_speed < 0.0F) {
        velocity_.x -= (1.0F + kWallRestitution) * normal_speed * normal_x;
        velocity_.y -= (1.0F + kWallRestitution) * normal_speed * normal_y;
    }
    outcome.wall_hit = outcome.wall_hit || normal_speed < -kWallSoundImpactSpeed;
    return true;
}

bool TiltModel::ResolveBumper(ModelOutcome& outcome) {
    if (level().bumper.radius <= 0) {
        return false;
    }
    const PointF bumper = bumper_position();
    const float dx = ball_.x - bumper.x;
    const float dy = ball_.y - bumper.y;
    const float minimum_distance = kBallRadius + static_cast<float>(level().bumper.radius);
    const float distance_squared = LengthSquared(dx, dy);
    if (distance_squared >= minimum_distance * minimum_distance) {
        return false;
    }
    const float distance = distance_squared > 0.0001F ? Sqrt(distance_squared) : 1.0F;
    const float normal_x = distance_squared > 0.0001F ? dx / distance : 1.0F;
    const float normal_y = distance_squared > 0.0001F ? dy / distance : 0.0F;
    ball_.x += normal_x * (minimum_distance - distance);
    ball_.y += normal_y * (minimum_distance - distance);
    const float normal_speed = velocity_.x * normal_x + velocity_.y * normal_y;
    const float impulse_speed = normal_speed < 80.0F ? 180.0F : -normal_speed * kBumperRestitution;
    velocity_.x += (impulse_speed - normal_speed) * normal_x;
    velocity_.y += (impulse_speed - normal_speed) * normal_y;
    outcome.bumper_hit = true;
    return true;
}

ModelOutcome TiltModel::Advance(uint64_t delta_us, Vec2 tilt) {
    ModelOutcome outcome{};
    if (completed_) {
        return outcome;
    }
    delta_us = delta_us > kMaximumDeltaUs ? kMaximumDeltaUs : delta_us;
    elapsed_us_ += delta_us;
    uint32_t steps = static_cast<uint32_t>((delta_us + kRenderTargetPeriodUs - 1U) / kRenderTargetPeriodUs);
    steps = steps == 0U ? 1U : (steps > 4U ? 4U : steps);
    const float step_seconds = static_cast<float>(delta_us) / (1000000.0F * static_cast<float>(steps));
    for (uint32_t step = 0U; step < steps; ++step) {
        const bool on_ice = Overlaps(level().ice, ball_, kBallRadius);
        if (on_ice && !was_on_ice_) {
            velocity_.x *= kIceEntrySpeedMultiplier;
            velocity_.y *= kIceEntrySpeedMultiplier;
        }
        was_on_ice_ = on_ice;

        const float control_scale = on_ice ? kIceControlScale : 1.0F;
        const float tilt_x = ClampFloat(tilt.x, -1.0F, 1.0F) * control_scale;
        const float tilt_y = ClampFloat(tilt.y, -1.0F, 1.0F) * control_scale;
        velocity_.x += tilt_x * kTiltAcceleration * step_seconds;
        velocity_.y += tilt_y * kTiltAcceleration * step_seconds;
        ApplyFans(step_seconds);
        const float damping = on_ice ? kIceDampingPerStep : kNormalDampingPerStep;
        velocity_.x *= damping;
        velocity_.y *= damping;
        const float speed_squared = LengthSquared(velocity_.x, velocity_.y);
        if (speed_squared > kMaximumSpeed * kMaximumSpeed) {
            const float scale = kMaximumSpeed / Sqrt(speed_squared);
            velocity_.x *= scale;
            velocity_.y *= scale;
        }
        ball_.x += velocity_.x * step_seconds;
        ball_.y += velocity_.y * step_seconds;

        if (ball_.x < kBallRadius) {
            ball_.x = kBallRadius;
            outcome.wall_hit = outcome.wall_hit || velocity_.x < -kWallSoundImpactSpeed;
            velocity_.x = velocity_.x < 0.0F ? -velocity_.x * kWallRestitution : velocity_.x;
        } else if (ball_.x > static_cast<float>(kLevelSize) - kBallRadius) {
            ball_.x = static_cast<float>(kLevelSize) - kBallRadius;
            outcome.wall_hit = outcome.wall_hit || velocity_.x > kWallSoundImpactSpeed;
            velocity_.x = velocity_.x > 0.0F ? -velocity_.x * kWallRestitution : velocity_.x;
        }
        if (ball_.y < kBallRadius) {
            ball_.y = kBallRadius;
            outcome.wall_hit = outcome.wall_hit || velocity_.y < -kWallSoundImpactSpeed;
            velocity_.y = velocity_.y < 0.0F ? -velocity_.y * kWallRestitution : velocity_.y;
        } else if (ball_.y > static_cast<float>(kLevelSize) - kBallRadius) {
            ball_.y = static_cast<float>(kLevelSize) - kBallRadius;
            outcome.wall_hit = outcome.wall_hit || velocity_.y > kWallSoundImpactSpeed;
            velocity_.y = velocity_.y > 0.0F ? -velocity_.y * kWallRestitution : velocity_.y;
        }
        for (uint32_t index = 0U; index < level().wall_count; ++index) {
            (void)ResolveWall(level().walls[index], outcome);
        }
        if (level().moving_wall.start.width != 0 && level().moving_wall.start.height != 0) {
            (void)ResolveWall(moving_wall_rect(), outcome);
        }
        if (!gate_open()) {
            (void)ResolveWall(level().gate.rect, outcome);
        }
        UpdatePressureGate(outcome);
        if (!pressure_gate_open_ && level().pressure_gate.gate.width != 0 && level().pressure_gate.gate.height != 0) {
            (void)ResolveWall(level().pressure_gate.gate, outcome);
        }
        (void)ResolveBumper(outcome);
        UpdatePortals(outcome);
    }

    for (uint32_t index = 0U; index < kStarCount; ++index) {
        if (star_collected(index)) {
            continue;
        }
        const float dx = ball_.x - level().stars[index].x;
        const float dy = ball_.y - level().stars[index].y;
        if (LengthSquared(dx, dy) <= Square(kBallRadius + 18.0F)) {
            collected_star_mask_ |= 1U << index;
            ++collected_stars_;
            outcome.star_collected = true;
        }
    }

    const float pit_dx = ball_.x - static_cast<float>(level().pit.x);
    const float pit_dy = ball_.y - static_cast<float>(level().pit.y);
    if (level().pit.radius > 0 &&
        LengthSquared(pit_dx, pit_dy) <= Square(static_cast<float>(level().pit.radius) - kBallRadius * 0.35F)) {
        outcome.fell = true;
        ++fall_count_;
        ResetBall();
    }

    const float goal_dx = ball_.x - static_cast<float>(level().goal.x);
    const float goal_dy = ball_.y - static_cast<float>(level().goal.y);
    if (goal_unlocked() && LengthSquared(goal_dx, goal_dy) <= Square(static_cast<float>(level().goal.radius))) {
        completed_ = true;
        velocity_ = {};
        outcome.completed = true;
    }
    return outcome;
}

}  // namespace tilt
