#include <cstdlib>
#include <iostream>

#include "apps/snake/gamekit/canvas_geometry.hpp"
#include "apps/snake/gamekit/swipe_gesture.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void CanvasOriginsStayInsideTheViewport() {
    constexpr int32_t kViewport = 720;
    constexpr int32_t kCanvas = 192;
    for (int32_t requested = -256; requested <= 976; ++requested) {
        const int32_t origin = snake::gamekit::ClampCanvasOrigin(requested, kCanvas, kViewport);
        Check(origin >= 0, "clamped canvas origin must not be negative");
        Check(origin + kCanvas <= kViewport, "clamped canvas must not escape the viewport");
    }

    constexpr int32_t kBottomFoodCenter = 76 + 22 * 25 + 25 / 2;
    constexpr int32_t kBottomOrigin = kBottomFoodCenter - kCanvas / 2;
    static_assert(kBottomOrigin == 542);
    static_assert(snake::gamekit::ClampCanvasOrigin(kBottomOrigin, kCanvas, kViewport) == 528);
}

void SwipeThresholdsPreservePhysicalTravel() {
    using snake::gamekit::ScalePhysicalThreshold;

    Check(ScalePhysicalThreshold(50U, 720U, 720U) == 50, "720 display must use a 50-logical-pixel swipe");
    Check(ScalePhysicalThreshold(50U, 720U, 480U) == 75, "480 display must use a 75-logical-pixel swipe");
    Check(ScalePhysicalThreshold(50U, 720U, 600U) == 60,
          "intermediate displays must preserve physical travel");
    Check(ScalePhysicalThreshold(50U, 720U, 0U) == 50, "invalid physical extent must use a safe fallback");
}

}  // namespace

int main() {
    CanvasOriginsStayInsideTheViewport();
    SwipeThresholdsPreservePhysicalTravel();
    std::cout << "snake gamekit tests passed\n";
    return 0;
}
