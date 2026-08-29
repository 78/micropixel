#include <cstdlib>
#include <iostream>

#include "runtime/display_transform.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using micropixel::detail::AppDisplayProfile;
    using micropixel::detail::MakeDisplayTransform;
    using micropixel::detail::ScaleCoordinate;

    const auto square_720 = MakeDisplayTransform(AppDisplayProfile::kSquare, 720U, 720U);
    Check(square_720.logical_width == 720U && square_720.logical_height == 720U,
          "720 square must use the identity design space");
    Check(square_720.scale_numerator == 720U && square_720.scale_denominator == 720U,
          "720 square texture scale must be 1:1");

    const auto square_480 = MakeDisplayTransform(AppDisplayProfile::kSquare, 480U, 480U);
    Check(square_480.logical_width == 720U && square_480.logical_height == 720U,
          "480 square must retain the 720 design space");
    Check(square_480.scale_numerator == 480U && square_480.scale_denominator == 720U,
          "480 square texture scale must be 2:3");
    Check(ScaleCoordinate(524, square_480.scale_numerator, square_480.scale_denominator) == 349,
          "texture dimensions must use deterministic nearest rounding");

    const auto square_on_landscape = MakeDisplayTransform(AppDisplayProfile::kSquare, 1280U, 720U);
    Check(square_on_landscape.logical_width == 720U && square_on_landscape.logical_height == 720U,
          "square App must retain its square design space on a landscape display");
    Check(square_on_landscape.physical_width == 720U && square_on_landscape.physical_height == 720U &&
              square_on_landscape.offset_x == 280 && square_on_landscape.offset_y == 0,
          "square App must be centered on a landscape display");
    Check(ScaleCoordinate(280 - square_on_landscape.offset_x, square_on_landscape.logical_width,
                          square_on_landscape.physical_width) == 0,
          "touch at the viewport edge must map to the logical origin");

    const auto square_on_portrait = MakeDisplayTransform(AppDisplayProfile::kSquare, 720U, 1280U);
    Check(square_on_portrait.physical_width == 720U && square_on_portrait.physical_height == 720U &&
              square_on_portrait.offset_x == 0 && square_on_portrait.offset_y == 280,
          "square App must be centered on a portrait display");

    const auto landscape = MakeDisplayTransform(AppDisplayProfile::kLandscape, 1280U, 720U);
    Check(landscape.logical_width == 1280U && landscape.logical_height == 720U,
          "landscape must keep a 720 logical short edge");

    const auto portrait = MakeDisplayTransform(AppDisplayProfile::kPortrait, 720U, 1280U);
    Check(portrait.logical_width == 720U && portrait.logical_height == 1280U,
          "portrait must keep a 720 logical short edge");

    Check(MakeDisplayTransform(AppDisplayProfile::kLandscape, 720U, 1280U).scale_denominator == 0U,
          "landscape profile must reject a portrait display");
    Check(MakeDisplayTransform(AppDisplayProfile::kSquare, 0U, 720U).scale_denominator == 0U,
          "zero-sized displays must be rejected");
    std::cout << "guest display transform tests passed\n";
    return 0;
}
