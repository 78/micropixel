#include "apps/snake/snake_game.hpp"

namespace snake {

void SnakeGame::AgeValue(uint32_t& age_us, uint32_t duration_us, bool& active, uint64_t delta_us) {
    uint64_t next = static_cast<uint64_t>(age_us) + delta_us;
    if (next >= duration_us) {
        age_us = duration_us;
        active = false;
    } else {
        age_us = static_cast<uint32_t>(next);
    }
}

void SnakeGame::AdvanceEffects(uint64_t delta_us) {
    for (Particle& particle : particles_) {
        if (particle.active) {
            AgeValue(particle.age_us, particle.duration_us, particle.active, delta_us);
        }
    }
    for (Trail& trail : trails_) {
        if (trail.active) {
            AgeValue(trail.age_us, 400000U, trail.active, delta_us);
        }
    }
    for (Popup& popup : popups_) {
        if (popup.active) {
            AgeValue(popup.age_us, 800000U, popup.active, delta_us);
        }
    }
    flash_remaining_us_ = delta_us >= flash_remaining_us_ ? 0U : flash_remaining_us_ - delta_us;
    shake_remaining_us_ = delta_us >= shake_remaining_us_ ? 0U : shake_remaining_us_ - delta_us;
    burst_remaining_us_ = delta_us >= burst_remaining_us_ ? 0U : burst_remaining_us_ - delta_us;
}

void SnakeGame::SpawnTrail(Cell cell) {
    Trail& trail = trails_.Acquire();
    trail = Trail{true, cell, 0U};
}

void SnakeGame::SpawnParticles(Cell origin, uint32_t count, Rgb color, uint32_t scale) {
    static constexpr int16_t kDirections[][2] = {
        {256, 0},  {237, 98},   {181, 181},   {98, 237},   {0, 256},  {-98, 237}, {-181, 181}, {-237, 98},
        {-256, 0}, {-237, -98}, {-181, -181}, {-98, -237}, {0, -256}, {98, -237}, {181, -181}, {237, -98},
    };
    count = count > kParticlePoolSize ? kParticlePoolSize : count;
    for (uint32_t index = 0U; index < count; ++index) {
        Particle& particle = particles_.Acquire();
        uint32_t direction = index % (sizeof(kDirections) / sizeof(kDirections[0]));
        uint32_t distance = (42U + NextRandom(effect_random_) % 58U) * scale;
        particle.active = true;
        particle.origin = origin;
        particle.dx = static_cast<int16_t>(kDirections[direction][0] * static_cast<int32_t>(distance) / 256);
        particle.dy = static_cast<int16_t>(kDirections[direction][1] * static_cast<int32_t>(distance) / 256);
        particle.age_us = 0U;
        particle.duration_us = 600000U;
        particle.color = color;
        particle.size = static_cast<uint8_t>(5U + NextRandom(effect_random_) % 4U);
    }
}

void SnakeGame::SpawnPopup(Cell cell, uint32_t points, Rgb color, micropixel::SystemFont font) {
    Popup& popup = popups_.Acquire();
    popup = Popup{true, cell, points, 0U, color, font};
}

void SnakeGame::TriggerFlash(Rgb color, uint64_t duration_us) {
    flash_color_ = color;
    flash_duration_us_ = duration_us;
    flash_remaining_us_ = duration_us;
}

void SnakeGame::TriggerShake(bool heavy, uint64_t duration_us) {
    // A shake is triggered After the Move that ate the food has already been
    // committed. Keep its retained interpolation intact; OnTimer finishes
    // presenting that one Move while holding back the next logical Move.
    shake_heavy_ = heavy;
    shake_duration_us_ = duration_us;
    shake_remaining_us_ = duration_us;
    // The Surface compositor captures the already presented panel frame. Keep
    // translation disabled until Render has presented the committed head at
    // the destination cell, then give that completed frame to the cache.
    shake_capture_delay_frames_ = 1U;
}

void SnakeGame::TriggerFoodEffects(Cell cell, const MoveOutcome& outcome) {
    Rgb color{52U, 211U, 153U};
    uint64_t flash_duration = 400000U;
    if (outcome.food_type == FoodType::kGolden) {
        color = Rgb{251U, 191U, 36U};
        flash_duration = 600000U;
    } else if (outcome.food_type == FoodType::kPoison) {
        color = Rgb{168U, 85U, 247U};
    } else if (outcome.food_type == FoodType::kSpeed) {
        color = Rgb{34U, 211U, 238U};
    }
    burst_cell_ = cell;
    burst_type_ = outcome.food_type;
    burst_remaining_us_ = kBurstDurationUs;
    const micropixel::SystemFont popup_font =
        outcome.food_type == FoodType::kGolden || outcome.food_type == FoodType::kSpeed
            ? micropixel::SystemFont::kTitle
            : micropixel::SystemFont::kLarge;
    SpawnPopup(cell, outcome.points, MixRgb(color, Rgb{255U, 255U, 255U}, 190U), popup_font);
    if (outcome.food_type != FoodType::kNormal) {
        TriggerFlash(color, flash_duration);
    }
}

[[nodiscard]] int32_t SnakeGame::ShakeComponent(bool x_axis) const {
    if (shake_capture_delay_frames_ != 0U || shake_remaining_us_ == 0U || shake_duration_us_ == 0U) {
        return 0;
    }
    int32_t maximum = shake_heavy_ ? 8 : 4;
    int32_t amplitude = static_cast<int32_t>(shake_remaining_us_ * static_cast<uint64_t>(maximum) / shake_duration_us_);
    uint32_t phase = static_cast<uint32_t>((shake_remaining_us_ / 25000U) & 3U);
    int32_t sign = phase == 0U || phase == 3U ? -1 : 1;
    return x_axis ? sign * amplitude : sign * amplitude / 2;
}

[[nodiscard]] int32_t SnakeGame::ShakeX() const { return ShakeComponent(true); }

[[nodiscard]] int32_t SnakeGame::ShakeY() const { return ShakeComponent(false); }

void SnakeGame::ResetEffects() {
    for (Particle& particle : particles_) {
        particle.active = false;
    }
    for (Trail& trail : trails_) {
        trail.active = false;
    }
    for (Popup& popup : popups_) {
        popup.active = false;
    }
    particles_.ResetCursor();
    trails_.ResetCursor();
    popups_.ResetCursor();
    flash_remaining_us_ = 0U;
    shake_remaining_us_ = 0U;
    shake_capture_delay_frames_ = 0U;
    burst_remaining_us_ = 0U;
}

}  // namespace snake
