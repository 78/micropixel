#ifndef MICROPIXEL_PLATFORM_LVGL_HOST_POINTER_EVENT_QUEUE_HPP
#define MICROPIXEL_PLATFORM_LVGL_HOST_POINTER_EVENT_QUEUE_HPP

#include <array>
#include <cstddef>

#include "device/contracts/input.hpp"

namespace micropixel::platform::lvgl {

enum class HostPointerPushResult {
    kEnqueued,
    kCoalescedMove,
    kDiscardedMove,
    kFull,
};

// A small externally synchronized queue for one LVGL pointer stream. Touch
// boundaries retain strict order; only Move samples may be coalesced or
// discarded under pressure.
template <size_t Capacity>
class HostPointerEventQueue final {
   public:
    static_assert(Capacity >= 2U);

    [[nodiscard]] HostPointerPushResult Push(const device::TouchSample& sample) {
        if (sample.phase == device::TouchPhase::kMove) {
            if (size_ != 0U) {
                auto& newest = events_[size_ - 1U];
                if (newest.phase == device::TouchPhase::kMove && newest.id == sample.id) {
                    newest = sample;
                    return HostPointerPushResult::kCoalescedMove;
                }
            }
            if (size_ == Capacity) {
                return HostPointerPushResult::kDiscardedMove;
            }
        } else if (size_ == Capacity && !DiscardOldestMove()) {
            return HostPointerPushResult::kFull;
        }

        events_[size_++] = sample;
        return HostPointerPushResult::kEnqueued;
    }

    [[nodiscard]] bool Pop(device::TouchSample& sample) {
        if (size_ == 0U) {
            return false;
        }
        sample = events_[0];
        for (size_t index = 1U; index < size_; ++index) {
            events_[index - 1U] = events_[index];
        }
        events_[--size_] = {};
        return true;
    }

    void Clear() {
        events_ = {};
        size_ = 0U;
    }

    [[nodiscard]] bool empty() const { return size_ == 0U; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] size_t size() const { return size_; }       // NOLINT(readability-identifier-naming)

   private:
    [[nodiscard]] bool DiscardOldestMove() {
        for (size_t index = 0U; index < size_; ++index) {
            if (events_[index].phase != device::TouchPhase::kMove) {
                continue;
            }
            for (size_t move_index = index + 1U; move_index < size_; ++move_index) {
                events_[move_index - 1U] = events_[move_index];
            }
            events_[--size_] = {};
            return true;
        }
        return false;
    }

    std::array<device::TouchSample, Capacity> events_{};
    size_t size_{};
};

}  // namespace micropixel::platform::lvgl

#endif
