#include <cassert>
#include <cstdint>

#include "platform/lvgl/display/frame_timing.hpp"

using micropixel::platform::lvgl::FrameTiming;

int main() {
    FrameTiming timing;
    assert(timing.p95_upper_us() == 0U);
    assert(timing.fps_milli() == 0U);
    uint64_t now = 0U;
    timing.Record(now);
    for (int frame = 0; frame < 95; ++frame) {
        now += 30000U;
        timing.Record(now);
    }
    for (int frame = 0; frame < 5; ++frame) {
        now += 60000U;
        timing.Record(now);
    }
    assert(timing.intervals() == 100U);
    assert(timing.p95_upper_us() == 30000U);
    assert(timing.elapsed_us() == 3150000U);
    assert(timing.fps_milli() == 31746U);
    now += 60001U;
    timing.Record(now);
    assert(timing.p95_upper_us() == 60000U);
    timing.Reset();
    timing.Record(100U);
    timing.Record(30101U);
    assert(timing.p95_upper_us() == 31000U);
    timing.Record(1U);  // clock reset starts a fresh window
    assert(timing.intervals() == 0U);
    timing.Record(1000001U);
    assert(timing.p95_upper_us() == 0U);  // overflow is explicitly unknown
    return 0;
}
