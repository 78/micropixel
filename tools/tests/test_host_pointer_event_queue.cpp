#include <cstdio>

#include "platform/lvgl/host_pointer_event_queue.hpp"

namespace {

using micropixel::device::TouchPhase;
using micropixel::device::TouchSample;
using micropixel::platform::lvgl::HostPointerEventQueue;
using micropixel::platform::lvgl::HostPointerPushResult;

TouchSample Sample(TouchPhase phase, uint32_t sequence, uint32_t id = 1U) {
    return TouchSample{
        .timestamp_us = sequence,
        .id = id,
        .x = static_cast<int16_t>(sequence),
        .y = static_cast<int16_t>(sequence + 1U),
        .pressure_per_mille = 500U,
        .phase = phase,
    };
}

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

bool MovesCoalesceWithoutChangingBoundaries() {
    HostPointerEventQueue<4U> queue;
    if (!Check(queue.Push(Sample(TouchPhase::kDown, 1U)) == HostPointerPushResult::kEnqueued, "Down must enqueue") ||
        !Check(queue.Push(Sample(TouchPhase::kMove, 2U)) == HostPointerPushResult::kEnqueued,
               "first Move must enqueue") ||
        !Check(queue.Push(Sample(TouchPhase::kMove, 3U)) == HostPointerPushResult::kCoalescedMove,
               "consecutive Move must coalesce") ||
        !Check(queue.Push(Sample(TouchPhase::kUp, 4U)) == HostPointerPushResult::kEnqueued, "Up must enqueue")) {
        return false;
    }

    TouchSample sample{};
    return Check(queue.Pop(sample) && sample.phase == TouchPhase::kDown && sample.timestamp_us == 1U,
                 "Down must retain its position") &&
           Check(queue.Pop(sample) && sample.phase == TouchPhase::kMove && sample.timestamp_us == 3U,
                 "Move must contain the newest coordinates") &&
           Check(queue.Pop(sample) && sample.phase == TouchPhase::kUp && sample.timestamp_us == 4U,
                 "Up must retain its position") &&
           Check(queue.empty(), "queue must be empty after the sequence");
}

bool RequiredBoundaryDisplacesMoveWhenFull() {
    HostPointerEventQueue<4U> queue;
    (void)queue.Push(Sample(TouchPhase::kDown, 1U));
    (void)queue.Push(Sample(TouchPhase::kUp, 2U));
    (void)queue.Push(Sample(TouchPhase::kDown, 3U));
    (void)queue.Push(Sample(TouchPhase::kMove, 4U));
    if (!Check(queue.Push(Sample(TouchPhase::kUp, 5U)) == HostPointerPushResult::kEnqueued,
               "required Up must displace a Move")) {
        return false;
    }

    constexpr TouchPhase expected[]{TouchPhase::kDown, TouchPhase::kUp, TouchPhase::kDown, TouchPhase::kUp};
    TouchSample sample{};
    for (TouchPhase phase : expected) {
        if (!Check(queue.Pop(sample) && sample.phase == phase, "boundary ordering must remain intact")) {
            return false;
        }
    }
    return true;
}

bool OnlyMovesMayBeDiscarded() {
    HostPointerEventQueue<2U> queue;
    (void)queue.Push(Sample(TouchPhase::kDown, 1U));
    (void)queue.Push(Sample(TouchPhase::kUp, 2U));
    if (!Check(queue.Push(Sample(TouchPhase::kMove, 3U)) == HostPointerPushResult::kDiscardedMove,
               "Move may be discarded when only boundaries are queued") ||
        !Check(queue.Push(Sample(TouchPhase::kDown, 4U)) == HostPointerPushResult::kFull,
               "required boundary must report backpressure when no Move can be removed") ||
        !Check(queue.size() == 2U, "failed boundary push must not alter the queue")) {
        return false;
    }

    TouchSample sample{};
    return Check(queue.Pop(sample) && sample.phase == TouchPhase::kDown, "Down must survive pressure") &&
           Check(queue.Pop(sample) && sample.phase == TouchPhase::kUp, "Up must survive pressure");
}

}  // namespace

int main() {
    return MovesCoalesceWithoutChangingBoundaries() && RequiredBoundaryDisplacesMoveWhenFull() &&
                   OnlyMovesMayBeDiscarded()
               ? 0
               : 1;
}
