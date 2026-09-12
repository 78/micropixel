// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_APPS_MAZE_BREAK_GAME_RUN_RECORD_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GAME_RUN_RECORD_HPP

#include <stdint.h>

namespace maze_break::game {

class RunRecord final {
   public:
    void Restore(uint32_t best_ms) { best_ms_ = best_ms; }
    void Start() {
        previous_best_ms_ = best_ms_;
        elapsed_us_ = 0;
        new_best_ = false;
    }
    void Advance(uint64_t elapsed_us) {
        constexpr uint64_t kMaxUs = uint64_t{UINT32_MAX} * 1000;
        elapsed_us_ += elapsed_us < kMaxUs - elapsed_us_ ? elapsed_us : kMaxUs - elapsed_us_;
    }
    bool Finish() {
        const uint32_t result = elapsed_ms();
        new_best_ = result != 0 && (best_ms_ == 0 || result < best_ms_);
        if (new_best_) {
            best_ms_ = result;
        }
        return new_best_;
    }
    uint32_t elapsed_ms() const { return static_cast<uint32_t>((elapsed_us_ + 999) / 1000); }
    uint32_t best_ms() const { return best_ms_; }
    uint32_t previous_best_ms() const { return previous_best_ms_; }
    bool new_best() const { return new_best_; }

   private:
    uint64_t elapsed_us_{};
    uint32_t best_ms_{};
    uint32_t previous_best_ms_{};
    bool new_best_{};
};

}  // namespace maze_break::game

#endif
