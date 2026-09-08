#ifndef MICROPIXEL_PLATFORM_LVGL_DISPLAY_FRAME_TIMING_HPP
#define MICROPIXEL_PLATFORM_LVGL_DISPLAY_FRAME_TIMING_HPP

#include <array>
#include <cstdint>

namespace micropixel::platform::lvgl {

// Single-task timing of completed panel transfers. Fixed 1 ms histogram bins;
// the last bin is overflow, not a fabricated percentile. Does not allocate.
class FrameTiming final {
   public:
    static constexpr uint32_t kBinWidthUs = 1000U;
    static constexpr uint32_t kBinCount = 512U;

    void Reset() { *this = {}; }

    void Record(uint64_t timestamp_us) {
        if (!started_ || timestamp_us <= previous_us_) {
            Reset();
            started_ = true;
            first_us_ = timestamp_us;
            previous_us_ = timestamp_us;
            return;
        }
        const uint64_t interval = timestamp_us - previous_us_;
        const uint64_t bin = (interval - 1U) / kBinWidthUs;
        ++bins_[bin < kBinCount ? static_cast<uint32_t>(bin) : kBinCount - 1U];
        ++intervals_;
        previous_us_ = timestamp_us;
    }

    [[nodiscard]] uint32_t intervals() const { return intervals_; }
    [[nodiscard]] uint64_t elapsed_us() const { return previous_us_ - first_us_; }
    [[nodiscard]] uint32_t fps_milli() const {
        return elapsed_us() == 0U ? 0U : static_cast<uint32_t>(intervals_ * 1000000000ULL / elapsed_us());
    }
    // Zero means no samples or a percentile in the unbounded overflow bin.
    [[nodiscard]] uint32_t p95_upper_us() const {
        if (intervals_ == 0U) return 0U;
        const uint64_t rank = (static_cast<uint64_t>(intervals_) * 95U + 99U) / 100U;
        uint64_t cumulative = 0U;
        for (uint32_t bin = 0U; bin + 1U < kBinCount; ++bin) {
            cumulative += bins_[bin];
            if (cumulative >= rank) return (bin + 1U) * kBinWidthUs;
        }
        return 0U;
    }

   private:
    std::array<uint32_t, kBinCount> bins_{};
    uint64_t first_us_{};
    uint64_t previous_us_{};
    uint32_t intervals_{};
    bool started_{};
};

}  // namespace micropixel::platform::lvgl

#endif
