#ifndef MICROPIXEL_HOST_UI_LVGL_SQUARE_COMMON_HALL_CAROUSEL_HPP
#define MICROPIXEL_HOST_UI_LVGL_SQUARE_COMMON_HALL_CAROUSEL_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common {

class HallVelocityTracker final {
   public:
    static constexpr uint32_t kMaximumSamples = 12U;
    static constexpr uint64_t kSampleHorizonUs = 100000U;

    void Reset(int32_t position, uint64_t timestamp_us) {
        count_ = 1U;
        samples_[0] = {.position = position, .timestamp_us = timestamp_us};
    }

    void Add(int32_t position, uint64_t timestamp_us) {
        if (count_ == 0U || timestamp_us < samples_[count_ - 1U].timestamp_us) {
            Reset(position, timestamp_us);
            return;
        }
        if (timestamp_us == samples_[count_ - 1U].timestamp_us) {
            samples_[count_ - 1U].position = position;
            return;
        }
        if (count_ == samples_.size()) {
            std::move(samples_.begin() + 1, samples_.end(), samples_.begin());
            --count_;
        }
        samples_[count_++] = {.position = position, .timestamp_us = timestamp_us};
        while (count_ > 1U && timestamp_us - samples_[0].timestamp_us > kSampleHorizonUs) {
            std::move(samples_.begin() + 1, samples_.begin() + count_, samples_.begin());
            --count_;
        }
    }

    void AddRelease(int32_t position, uint64_t timestamp_us) {
        // Some touch controllers report UP later with the final MOVE position.
        // Treat that as idle time instead of a new duplicate fit point, which
        // can make a quadratic extrapolation reverse direction at release.
        if (count_ == 0U || samples_[count_ - 1U].position != position) {
            Add(position, timestamp_us);
        }
    }

    [[nodiscard]] int32_t Velocity(int32_t maximum_velocity, uint64_t evaluation_timestamp_us = 0U) const {
        if (count_ < 2U || maximum_velocity <= 0) {
            return 0;
        }
        const uint64_t newest_us = samples_[count_ - 1U].timestamp_us;
        const uint64_t span_us = newest_us - samples_[0].timestamp_us;
        if (span_us < 8000U) {
            return 0;
        }

        // Weighted linear regression is deliberately used instead of LSQ2.
        // GT911 can deliver uneven intervals and a delayed duplicate release;
        // a quadratic extrapolation can overshoot or even reverse in those
        // cases. Recent samples receive more weight, while a linear fit keeps a
        // monotonic finger trajectory monotonic at release.
        float weight_sum = 0.0F;
        float weighted_time = 0.0F;
        float weighted_position = 0.0F;
        for (uint32_t index = 0U; index < count_; ++index) {
            const float age =
                static_cast<float>(newest_us - samples_[index].timestamp_us) / static_cast<float>(kSampleHorizonUs);
            const float weight = 1.0F - 0.75F * std::clamp(age, 0.0F, 1.0F);
            const float time =
                -static_cast<float>(newest_us - samples_[index].timestamp_us) / static_cast<float>(span_us);
            weight_sum += weight;
            weighted_time += weight * time;
            weighted_position += weight * static_cast<float>(samples_[index].position);
        }
        if (weight_sum <= 0.0F) {
            return 0;
        }
        const float mean_time = weighted_time / weight_sum;
        const float mean_position = weighted_position / weight_sum;
        float covariance = 0.0F;
        float variance = 0.0F;
        for (uint32_t index = 0U; index < count_; ++index) {
            const float age =
                static_cast<float>(newest_us - samples_[index].timestamp_us) / static_cast<float>(kSampleHorizonUs);
            const float weight = 1.0F - 0.75F * std::clamp(age, 0.0F, 1.0F);
            const float time =
                -static_cast<float>(newest_us - samples_[index].timestamp_us) / static_cast<float>(span_us);
            const float centered_time = time - mean_time;
            covariance += weight * centered_time * (static_cast<float>(samples_[index].position) - mean_position);
            variance += weight * centered_time * centered_time;
        }
        if (variance < 1.0e-6F) {
            return 0;
        }
        float velocity = covariance / variance * 1000000.0F / static_cast<float>(span_us);

        uint32_t recent_anchor = count_ - 2U;
        while (recent_anchor > 0U && newest_us - samples_[recent_anchor - 1U].timestamp_us <= 60000U) {
            --recent_anchor;
        }
        const int32_t recent_delta = samples_[count_ - 1U].position - samples_[recent_anchor].position;
        if (std::abs(recent_delta) < 3 || (recent_delta > 0) != (velocity > 0.0F)) {
            velocity = 0.0F;
        }
        return ApplyIdleDecay(velocity, newest_us, evaluation_timestamp_us, maximum_velocity);
    }

   private:
    struct Sample final {
        int32_t position{};
        uint64_t timestamp_us{};
    };

    [[nodiscard]] static int32_t ApplyIdleDecay(float velocity, uint64_t newest_us, uint64_t evaluation_timestamp_us,
                                                int32_t maximum_velocity) {
        if (evaluation_timestamp_us > newest_us) {
            const uint64_t idle_us = evaluation_timestamp_us - newest_us;
            if (idle_us >= kSampleHorizonUs) {
                return 0;
            }
            velocity *= static_cast<float>(kSampleHorizonUs - idle_us) / static_cast<float>(kSampleHorizonUs);
        }
        return std::clamp<int32_t>(static_cast<int32_t>(std::lround(velocity)), -maximum_velocity, maximum_velocity);
    }

    std::array<Sample, kMaximumSamples> samples_{};
    uint32_t count_{};
};

template <typename Layout>
struct HallCarouselPolicy final {
    static constexpr int32_t kLeft = Layout::kHallLeft;
    static constexpr int32_t kTop = Layout::kHallTop;
    static constexpr int32_t kViewportWidth = Layout::kHallViewportWidth;
    static constexpr int32_t kCardWidth = Layout::kHallCardWidth;
    static constexpr int32_t kCardHeight = Layout::kHallCardHeight;
    static constexpr int32_t kCardGap = Layout::kHallCardGap;
    static constexpr int32_t kCardStep = kCardWidth + kCardGap;
    static constexpr uint32_t kFullyVisibleCards = 3U;
    static constexpr uint32_t kMaximumIntersectingCards = 4U;
    static constexpr uint32_t kCoverPrefetchCards = 1U;
    static constexpr uint32_t kMaximumCachedCovers = kMaximumIntersectingCards + 2U * kCoverPrefetchCards;
    static constexpr int32_t kDragThreshold = 8;
    static constexpr int32_t kMinimumThrowVelocity = 80;
    static constexpr int32_t kMaximumThrowVelocity = 4000;
    static constexpr int32_t kStopVelocity = 24;
    // Equivalent to an approximately 0.998 per-millisecond velocity decay.
    static constexpr float kFrictionPerSecond = 2.002F;
    static constexpr uint32_t kMaximumInertiaDurationMs = 2600U;

    [[nodiscard]] static constexpr int32_t Abs(int32_t value) { return value < 0 ? -value : value; }

    [[nodiscard]] static constexpr uint32_t MaxScrollIndex(uint32_t app_count) {
        return app_count > kFullyVisibleCards ? app_count - kFullyVisibleCards : 0U;
    }

    [[nodiscard]] static constexpr int32_t MaxOffset(uint32_t app_count) {
        return static_cast<int32_t>(MaxScrollIndex(app_count)) * kCardStep;
    }

    [[nodiscard]] static constexpr int32_t ClampOffset(uint32_t app_count, int32_t offset) {
        return std::clamp<int32_t>(offset, 0, MaxOffset(app_count));
    }

    [[nodiscard]] static constexpr int32_t DragOffset(uint32_t app_count, int32_t start_offset, int32_t delta_x) {
        return ClampOffset(app_count, start_offset - delta_x);
    }

    [[nodiscard]] static constexpr bool IsHorizontalDrag(int32_t delta_x, int32_t delta_y) {
        return Abs(delta_x) >= kDragThreshold && Abs(delta_x) > Abs(delta_y);
    }

    [[nodiscard]] static int32_t InertiaTarget(uint32_t app_count, int32_t current_offset, int32_t velocity) {
        if (Abs(velocity) < kMinimumThrowVelocity) {
            return ClampOffset(app_count, current_offset);
        }
        const float distance = static_cast<float>(Abs(velocity) - kStopVelocity) / kFrictionPerSecond;
        const int32_t signed_distance = static_cast<int32_t>(std::lround(std::copysign(distance, velocity)));
        return ClampOffset(app_count, current_offset + signed_distance);
    }

    [[nodiscard]] static int32_t InertiaVelocity(int32_t initial_velocity, uint32_t elapsed_ms) {
        if (Abs(initial_velocity) < kMinimumThrowVelocity) {
            return 0;
        }
        const float elapsed_seconds = static_cast<float>(elapsed_ms) / 1000.0F;
        const float speed = static_cast<float>(Abs(initial_velocity)) * std::exp(-kFrictionPerSecond * elapsed_seconds);
        if (speed < static_cast<float>(kStopVelocity)) {
            return 0;
        }
        return static_cast<int32_t>(
            std::lround(std::copysign(std::min(speed, static_cast<float>(kMaximumThrowVelocity)), initial_velocity)));
    }

    [[nodiscard]] static int32_t BoostRepeatedThrow(int32_t finger_velocity, int32_t interrupted_velocity,
                                                    uint32_t held_ms) {
        if (Abs(finger_velocity) < kMinimumThrowVelocity) {
            return finger_velocity;
        }
        const int32_t carried_velocity = InertiaVelocity(interrupted_velocity, held_ms);
        if (carried_velocity == 0 || (finger_velocity > 0) != (carried_velocity > 0)) {
            return finger_velocity;
        }
        // A repeated throw in the same direction carries half of the remaining
        // momentum. The new finger velocity remains dominant, so reversing the
        // gesture brakes immediately and a stationary touch still stops it.
        constexpr float kCarryFraction = 0.5F;
        const int32_t boosted = finger_velocity + static_cast<int32_t>(std::lround(carried_velocity * kCarryFraction));
        return std::clamp<int32_t>(boosted, -kMaximumThrowVelocity, kMaximumThrowVelocity);
    }

    [[nodiscard]] static uint32_t InertiaDurationMs(int32_t current_offset, int32_t target_offset, int32_t velocity) {
        const float distance = static_cast<float>(Abs(target_offset - current_offset));
        const float speed = static_cast<float>(Abs(velocity));
        if (distance == 0.0F || speed < static_cast<float>(kMinimumThrowVelocity)) {
            return 0U;
        }
        const float remaining_fraction =
            std::max(static_cast<float>(kStopVelocity) / speed, 1.0F - kFrictionPerSecond * distance / speed);
        const float duration_ms = -std::log(remaining_fraction) * 1000.0F / kFrictionPerSecond;
        return std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(duration_ms)), 80U, kMaximumInertiaDurationMs);
    }

    [[nodiscard]] static int32_t InertiaOffset(int32_t start_offset, int32_t target_offset, int32_t velocity,
                                               uint32_t elapsed_ms) {
        if (elapsed_ms == 0U || start_offset == target_offset || Abs(velocity) < kMinimumThrowVelocity) {
            return start_offset;
        }
        const float elapsed_seconds = static_cast<float>(elapsed_ms) / 1000.0F;
        const float distance = static_cast<float>(Abs(velocity)) / kFrictionPerSecond *
                               (1.0F - std::exp(-kFrictionPerSecond * elapsed_seconds));
        const int32_t offset = start_offset + static_cast<int32_t>(std::lround(std::copysign(distance, velocity)));
        return velocity > 0 ? std::min(offset, target_offset) : std::max(offset, target_offset);
    }

    [[nodiscard]] static constexpr int32_t ContentWidth(uint32_t app_count) {
        if (app_count == 0U) {
            return kViewportWidth;
        }
        return static_cast<int32_t>(app_count) * kCardStep - kCardGap;
    }

    [[nodiscard]] static constexpr int32_t ScrollThumbWidth(int32_t track_width, uint32_t app_count) {
        if (app_count <= kFullyVisibleCards) {
            return track_width;
        }
        return std::max<int32_t>(
            28, track_width * static_cast<int32_t>(kFullyVisibleCards) / static_cast<int32_t>(app_count));
    }

    [[nodiscard]] static constexpr int32_t ScrollThumbX(int32_t track_width, int32_t thumb_width, uint32_t app_count,
                                                        int32_t offset) {
        const int32_t maximum = MaxOffset(app_count);
        return maximum == 0 ? 0 : (track_width - thumb_width) * ClampOffset(app_count, offset) / maximum;
    }

    [[nodiscard]] static constexpr int32_t CardX(uint32_t index, int32_t offset) {
        return kLeft + static_cast<int32_t>(index) * kCardStep - offset;
    }

    [[nodiscard]] static constexpr int32_t RevealOffset(uint32_t app_count, int32_t offset, uint32_t index) {
        const int32_t clamped = ClampOffset(app_count, offset);
        if (index >= app_count) {
            return clamped;
        }
        const int32_t card_x = CardX(index, clamped);
        const int32_t viewport_right = kLeft + kViewportWidth;
        if (card_x < kLeft) {
            return ClampOffset(app_count, clamped - (kLeft - card_x));
        }
        if (card_x + kCardWidth > viewport_right) {
            return ClampOffset(app_count, clamped + card_x + kCardWidth - viewport_right);
        }
        return clamped;
    }

    [[nodiscard]] static constexpr uint32_t CoverWindowFirst(uint32_t app_count, int32_t offset) {
        if (app_count == 0U) {
            return 0U;
        }
        const uint32_t first_visible = static_cast<uint32_t>(ClampOffset(app_count, offset) / kCardStep);
        return first_visible > kCoverPrefetchCards ? first_visible - kCoverPrefetchCards : 0U;
    }

    [[nodiscard]] static constexpr uint32_t CoverWindowLast(uint32_t app_count, int32_t offset) {
        if (app_count == 0U) {
            return 0U;
        }
        const int32_t clamped = ClampOffset(app_count, offset);
        const uint32_t last_intersecting =
            static_cast<uint32_t>((clamped + kViewportWidth + kCardStep - 1) / kCardStep);
        return std::min<uint32_t>(app_count, last_intersecting + kCoverPrefetchCards);
    }

    [[nodiscard]] static constexpr bool CoverInWindow(uint32_t app_count, int32_t offset, uint32_t index) {
        return index >= CoverWindowFirst(app_count, offset) && index < CoverWindowLast(app_count, offset);
    }
};

}  // namespace micropixel::host_ui::lvgl::square_common

#endif
