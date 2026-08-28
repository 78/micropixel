#include <cstdlib>
#include <iostream>

#include "sdk/ui/viewport.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    constexpr micropixel::ui::Viewport identity{{720, 720}, {720, 720}};
    static_assert(identity.valid());
    static_assert(identity.identity());
    static_assert(identity.ToPhysical(micropixel::Point{137, 419}) == micropixel::Point{137, 419});

    constexpr micropixel::ui::Viewport compact{{480, 480}, {720, 720}};
    static_assert(compact.valid());
    static_assert(!compact.identity());
    static_assert(compact.ToPhysical(micropixel::Point{720, 720}) == micropixel::Point{480, 480});
    static_assert(compact.ToPhysical(micropixel::Point{360, 360}) == micropixel::Point{240, 240});
    static_assert(compact.ToLogical({240, 240}) == micropixel::Point{360, 360});

    const micropixel::Rect physical_rect = compact.ToPhysical({1, 1, 2, 2});
    Check(physical_rect.x == 1 && physical_rect.y == 1 && physical_rect.width == 1 && physical_rect.height == 1,
          "rectangles must scale their edges without collapsing the right/bottom edge independently");
    Check(compact.DeltaToPhysical({-90, 45}) == micropixel::Point{-60, 30},
          "signed translations must preserve direction");
    Check(compact.ToPhysical(micropixel::SystemFont::kTitle) == micropixel::SystemFont::kLarge,
          "the compact profile must reduce system font roles by one step");

    std::cout << "guest viewport tests passed\n";
    return 0;
}
