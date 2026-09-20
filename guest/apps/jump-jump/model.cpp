// SPDX-License-Identifier: Apache-2.0
#include "model.hpp"

namespace jump_jump {
namespace {
float Abs(float x) { return x < 0.0F ? -x : x; }
uint64_t Age(uint64_t now, uint64_t then) { return now >= then ? now - then : 0U; }
}  // namespace

float Length(Vec2 v) { return __builtin_sqrtf(v.x * v.x + v.z * v.z); }
float Clamp(float value, float low, float high) { return value < low ? low : (value > high ? high : value); }
float Ease(float t) {
    t = Clamp(t, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

uint32_t Model::Random() {
    rng_ ^= rng_ << 13U;
    rng_ ^= rng_ >> 17U;
    rng_ ^= rng_ << 5U;
    return rng_;
}

void Model::Reset(uint32_t seed, uint64_t now_us) {
    rng_ = seed == 0U ? 1U : seed;
    phase_ = Phase::kReady;
    now_us_ = phase_at_us_ = camera_at_us_ = idle_at_us_ = reward_at_us_ = now_us;
    score_ = combo_ = jumps_ = last_award_ = feedback_ = 0U;
    last_special_jump_ = 0U;
    last_special_slot_ = UINT32_MAX;
    size_steps_ = 0U;
    has_previous_ = false;
    previous_ = {};
    current_ = {};
    current_.size = 86.0F;
    player_ = launch_ = destination_ = reward_position_ = {};
    next_ = {{190.0F, 0.0F}, 82.0F, PlatformKind::kCylinder, 1U, false};
    camera_from_ = camera_to_ = (current_.position + next_.position) * 0.5F;
    last_landing_ = Landing::kSafe;
}

void Model::Restart(uint64_t now_us) { Reset(Random(), now_us); }

float Model::Distance(uint64_t hold_us) {
    if (hold_us > kMaximumChargeUs) hold_us = kMaximumChargeUs;
    return static_cast<float>(hold_us) * 0.00024F;
}

Landing Model::Classify(const Platform& platform, Vec2 position) {
    const Vec2 delta = position - platform.position;
    const bool round = platform.kind == PlatformKind::kCylinder || platform.kind == PlatformKind::kDrain ||
                       platform.kind == PlatformKind::kRecord || platform.kind == PlatformKind::kStool;
    const float distance = round ? Length(delta) : (Abs(delta.x) > Abs(delta.z) ? Abs(delta.x) : Abs(delta.z));
    const float radius = platform.size * 0.5F;
    if (distance > radius + 3.0F) return Landing::kMiss;
    if (distance > radius - 3.0F) return Landing::kEdge;
    return Length(delta) <= platform.size * 0.12F ? Landing::kCenter : Landing::kSafe;
}

uint32_t Model::Bonus(PlatformKind kind) {
    switch (kind) {
        case PlatformKind::kDrain:
            return 5U;
        case PlatformKind::kCube:
            return 10U;
        case PlatformKind::kShop:
            return 15U;
        case PlatformKind::kRecord:
            return 30U;
        default:
            return 0U;
    }
}

bool Model::Press(uint64_t now_us) {
    Advance(now_us);
    if (phase_ != Phase::kReady) return false;
    phase_ = Phase::kCharging;
    phase_at_us_ = now_us;
    return true;
}

bool Model::Release(uint64_t now_us) {
    if (phase_ != Phase::kCharging || now_us < phase_at_us_) return false;
    launch_ = player_;
    const Vec2 direction = next_.position - player_;
    const float length = Length(direction);
    destination_ = launch_ + direction * (Distance(now_us - phase_at_us_) / (length > 0.0F ? length : 1.0F));
    phase_ = Phase::kFlying;
    phase_at_us_ = now_us;
    feedback_ |= kJump;
    Advance(now_us_ > now_us ? now_us_ : now_us);
    return true;
}

void Model::Cancel(uint64_t now_us) {
    Advance(now_us);
    if (phase_ == Phase::kCharging) {
        phase_ = Phase::kReady;
        phase_at_us_ = idle_at_us_ = now_us_;
    }
}

void Model::GenerateNext() {
    const uint32_t difficulty = jumps_ < 30U ? jumps_ : 30U;
    const float distance = 144.0F + static_cast<float>(Random() % (52U + difficulty));
    next_.position = (Random() & 1U) != 0U ? Vec2{distance, 0.0F} : Vec2{0.0F, distance};
    const float shrink = static_cast<float>(size_steps_) * 0.005F;
    const float minimum = Clamp(0.8F - shrink, 0.25F, 0.8F);
    const float maximum = Clamp(1.0F - shrink, 0.6F, 1.0F);
    const float scale = minimum + (maximum - minimum) * static_cast<float>(Random() % 1001U) / 1000.0F;
    next_.size = 86.0F * scale;
    next_.color = static_cast<uint8_t>(Random() % 5U);
    const uint32_t ordinary_slot = Random() % 13U;
    if (ordinary_slot >= 9U) next_.size = 86.0F * Clamp(scale, 0.6F, 1.0F);
    next_.kind = ordinary_slot == 2U || ordinary_slot == 7U ? PlatformKind::kStool
                 : (ordinary_slot & 1U) != 0U               ? PlatformKind::kBox
                                                            : PlatformKind::kCylinder;
    // Historical classic cadence: special scenery is spaced by successful
    // jumps, and only four of its 17 slots award points. Unimplemented scenery
    // uses a plain platform; it must not become an extra scoring opportunity.
    const uint32_t interval = score_ > 1000U ? 6U : 5U;
    if (jumps_ - last_special_jump_ >= interval) {
        const uint32_t count = score_ < 100U ? 16U : 17U;
        uint32_t slot = Random() % count;
        if (slot == last_special_slot_) slot = (slot + 1U + Random() % (count - 1U)) % count;
        last_special_jump_ = jumps_;
        last_special_slot_ = slot;
        next_.kind = (slot & 1U) != 0U ? PlatformKind::kBox : PlatformKind::kCylinder;
        next_.size = 86.0F * Clamp(scale, 0.7F, 1.0F);
        switch (slot) {
            case 4U:
                next_.kind = PlatformKind::kCube;
                break;
            case 6U:
                next_.kind = PlatformKind::kRecord;
                break;
            case 11U:
                next_.kind = PlatformKind::kShop;
                break;
            case 13U:
                next_.kind = PlatformKind::kDrain;
                break;
            default:
                break;
        }
    }
    next_.rewarded = false;
}

void Model::Award(uint32_t amount, uint64_t at_us) {
    if (size_steps_ < 110U) ++size_steps_;
    score_ = amount > UINT32_MAX - score_ ? UINT32_MAX : score_ + amount;
    last_award_ = amount;
    reward_at_us_ = at_us;
    reward_position_ = player_;
}

void Model::Land(uint64_t at_us) {
    player_ = destination_;
    last_landing_ = Classify(next_, player_);
    if (last_landing_ == Landing::kCenter || last_landing_ == Landing::kSafe) {
        // Rebase after every landing: long games never lose float precision.
        const Vec2 offset = next_.position;
        const Vec2 old_camera = camera();
        previous_ = current_;
        previous_.position = previous_.position - offset;
        has_previous_ = true;
        current_ = next_;
        current_.position = {};
        player_ = player_ - offset;
        launch_ = launch_ - offset;
        destination_ = destination_ - offset;
        if (jumps_ != UINT32_MAX) ++jumps_;
        if (last_landing_ == Landing::kCenter) {
            if (combo_ < 16U) ++combo_;
            Award(combo_ * 2U, at_us);
            feedback_ |= kPerfect;
        } else {
            combo_ = 0U;
            Award(1U, at_us);
            feedback_ |= kLand;
        }
        GenerateNext();
        camera_from_ = old_camera - offset;
        camera_to_ = (current_.position + next_.position) * 0.5F;
        camera_at_us_ = at_us;
        phase_ = Phase::kSettling;
    } else {
        const Landing old = Classify(current_, player_);
        if (old == Landing::kCenter || old == Landing::kSafe) {
            // A short hop back onto the same platform cannot farm points.
            combo_ = 0U;
            phase_ = Phase::kSettling;
            feedback_ |= kLand;
        } else {
            if (old == Landing::kEdge) last_landing_ = old;
            phase_ = Phase::kFalling;
            combo_ = 0U;
            feedback_ |= kFail;
        }
    }
    phase_at_us_ = idle_at_us_ = at_us;
}

void Model::Advance(uint64_t now_us) {
    if (now_us < now_us_) return;
    now_us_ = now_us;
    if (phase_ == Phase::kFlying && phase_age_us() >= kFlightUs) Land(phase_at_us_ + kFlightUs);
    if (phase_ == Phase::kSettling && phase_age_us() >= kSettleUs) {
        phase_ = Phase::kReady;
        phase_at_us_ += kSettleUs;
    }
    if (phase_ == Phase::kFalling && phase_age_us() >= kFallUs) {
        phase_ = Phase::kGameOver;
        phase_at_us_ += kFallUs;
    }
    if (phase_ == Phase::kReady && !current_.rewarded && Bonus(current_.kind) != 0U &&
        Age(now_us_, idle_at_us_) >= kBonusUs) {
        current_.rewarded = true;
        Award(Bonus(current_.kind), idle_at_us_ + kBonusUs);
        feedback_ |= kBonus;
    }
}

uint64_t Model::phase_age_us() const { return Age(now_us_, phase_at_us_); }
uint64_t Model::reward_age_us() const { return Age(now_us_, reward_at_us_); }
float Model::charge() const {
    return phase_ == Phase::kCharging ? Clamp(static_cast<float>(phase_age_us()) / 1200000.0F, 0.0F, 1.0F) : 0.0F;
}
Vec2 Model::camera() const {
    return camera_from_ +
           (camera_to_ - camera_from_) * Ease(static_cast<float>(Age(now_us_, camera_at_us_)) / 360000.0F);
}
Pose Model::pose() const {
    Pose pose{player_, kPlatformHeight, charge() * 0.38F, 0.0F};
    pose.height -= kPlatformHeight * charge() * 0.20F;
    if (phase_ == Phase::kFlying) {
        const float t = Clamp(static_cast<float>(phase_age_us()) / static_cast<float>(kFlightUs), 0.0F, 1.0F);
        pose.position = launch_ + (destination_ - launch_) * t;
        pose.height += 4.0F * 98.0F * t * (1.0F - t);
        pose.rotation = 6.2831853F * Ease(t);
    } else if (phase_ == Phase::kSettling) {
        const float t = Clamp(static_cast<float>(phase_age_us()) / static_cast<float>(kSettleUs), 0.0F, 1.0F);
        pose.squash = 0.25F * (1.0F - t) * (1.0F - t);
    } else if (phase_ == Phase::kFalling || phase_ == Phase::kGameOver) {
        const float t = phase_ == Phase::kGameOver
                            ? 1.0F
                            : Clamp(static_cast<float>(phase_age_us()) / static_cast<float>(kFallUs), 0.0F, 1.0F);
        const Vec2 direction = destination_ - launch_;
        const float length = Length(direction);
        pose.position = player_ + direction * (24.0F * t / (length > 0.0F ? length : 1.0F));
        pose.height -= 210.0F * t * t;
        pose.rotation = (direction.x >= direction.z ? 1.0F : -1.0F) * t * 2.0F;
    }
    return pose;
}
uint32_t Model::TakeFeedback() {
    const uint32_t result = feedback_;
    feedback_ = 0U;
    return result;
}

bool Input::Down(Model& model, uint64_t token, uint64_t at_us) {
    if (active_) return false;
    model.Advance(at_us);
    restart_press_ = model.phase() == Phase::kGameOver && model.phase_age_us() >= 250000U;
    if (!restart_press_ && !model.Press(at_us)) return false;
    token_ = token;
    active_ = true;
    return !restart_press_;
}
bool Input::Up(Model& model, uint64_t token, uint64_t at_us) {
    if (!active_ || token != token_) return false;
    active_ = false;
    if (restart_press_) {
        restart_press_ = false;
        model.Restart(at_us);
        return false;
    }
    return model.Release(at_us);
}
void Input::Cancel(Model& model, uint64_t token, uint64_t at_us) {
    if (active_ && token == token_) Reset(model, at_us);
}
void Input::Reset(Model& model, uint64_t at_us) {
    active_ = false;
    restart_press_ = false;
    model.Cancel(at_us);
}
}  // namespace jump_jump
