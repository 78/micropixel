#ifndef MICROPIXEL_RUNTIME_AUDIO_LINEAR_UPSAMPLER_HPP
#define MICROPIXEL_RUNTIME_AUDIO_LINEAR_UPSAMPLER_HPP

#include <cstdint>

namespace micropixel::runtime {

// Integer-ratio upsampler with linear interpolation for mono int16 PCM.
//
// The mixer renders at the board's output rate while Opus clips decode at
// 16 kHz and Guest PCM streams may be written at any integer divisor of the
// mix rate. Instead of paying for a proper resampler on the audio task this
// emits `factor` output samples per source sample, walking a straight line
// from the previous source sample to the next one. Output therefore lags the
// source by one source period, which is inaudible at these rates.
//
// Source samples are pulled one at a time through a callable so the caller's
// ring buffer bookkeeping stays where it is; the interpolator only keeps the
// last source sample and the phase inside the current group.
class LinearUpsampler final {
   public:
    void Reset(uint32_t factor) {
        factor_ = factor == 0U ? 1U : factor;
        phase_ = 0U;
        previous_ = 0;
        next_ = 0;
    }

    [[nodiscard]] uint32_t factor() const { return factor_; }  // NOLINT(readability-identifier-naming)
    // True when every output sample of the last pulled source sample is out,
    // i.e. the stream can finish without leaving a partial group behind.
    [[nodiscard]] bool Idle() const { return phase_ == 0U; }

    // Fills up to `capacity` output samples. `pull(int16_t& sample)` returns
    // false once the source is exhausted; the number of samples produced is
    // returned and may be short only in that case.
    template <typename Pull>
    uint32_t Produce(int16_t* output, uint32_t capacity, Pull&& pull) {
        uint32_t produced = 0U;
        while (produced < capacity) {
            if (phase_ == 0U) {
                int16_t sample = 0;
                if (!pull(sample)) {
                    break;
                }
                previous_ = next_;
                next_ = sample;
            }
            const int32_t delta = static_cast<int32_t>(next_) - static_cast<int32_t>(previous_);
            const int32_t value = static_cast<int32_t>(previous_) +
                                  delta * static_cast<int32_t>(phase_ + 1U) / static_cast<int32_t>(factor_);
            output[produced++] = static_cast<int16_t>(value);
            if (++phase_ == factor_) {
                phase_ = 0U;
            }
        }
        return produced;
    }

   private:
    uint32_t factor_{1U};
    uint32_t phase_{};
    int16_t previous_{};
    int16_t next_{};
};

}  // namespace micropixel::runtime

#endif
