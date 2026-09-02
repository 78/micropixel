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
    using micropixel::detail::MakeDisplayTransform;
    using micropixel::detail::MapPhysicalInsets;
    using micropixel::detail::MapRect;
    using micropixel::detail::MapSceneRect;
    using micropixel::detail::MapSceneSizedRect;
    using micropixel::detail::MapSceneVectorX;
    using micropixel::detail::MapTextureRect;
    using micropixel::detail::ScaleCoordinate;

    const auto square_720 = MakeDisplayTransform(720U, 720U);
    Check(square_720.logical_width == 720U && square_720.logical_height == 720U,
          "720 square must use the identity design space");
    Check(square_720.scale_numerator == 720U && square_720.scale_denominator == 720U,
          "720 square texture scale must be 1:1");

    const auto square_480 = MakeDisplayTransform(480U, 480U);
    Check(square_480.logical_width == 720U && square_480.logical_height == 720U,
          "480 square must retain the 720 design space");
    Check(square_480.scale_numerator == 480U && square_480.scale_denominator == 720U,
          "480 square texture scale must be 2:3");
    Check(ScaleCoordinate(524, square_480.scale_numerator, square_480.scale_denominator) == 349,
          "texture dimensions must use deterministic nearest rounding");
    const auto board = MapSceneRect(square_480, 47, 76, 625, 625);
    Check(board.x == 31 && board.y == 51 && board.width == 417 && board.height == 416,
          "scene rectangles must lower their two edges into physical coordinates");
    const auto textured_board = MapSceneSizedRect(square_480, 47, 76, 625, 625);
    Check(
        textured_board.x == 31 && textured_board.y == 51 && textured_board.width == 417 && textured_board.height == 417,
        "adaptive texture extents must remain phase-independent for physical 1:1 copies");
    Check(MapSceneVectorX(square_480, -6) == -4, "layer translations must lower without viewport offsets");
    const auto rounded_safe_area = MapPhysicalInsets(square_480, 24U, 24U, 24U, 24U);
    Check(rounded_safe_area.top == 36U && rounded_safe_area.right == 36U && rounded_safe_area.bottom == 36U &&
              rounded_safe_area.left == 36U,
          "physical safe-area insets must scale into the Guest logical coordinate space");
    const auto rounded_up_safe_area = MapPhysicalInsets(square_480, 1U, 1U, 1U, 1U);
    Check(rounded_up_safe_area.top == 2U && rounded_up_safe_area.left == 2U,
          "safe-area scaling must round outward rather than expose a clipped physical pixel");

    const auto atlas_frame = micropixel::detail::MapSizedRect(25, 50, 25, 25, 100U, 100U, 67U, 67U);
    Check(atlas_frame.x == 17 && atlas_frame.y == 34 && atlas_frame.width == 17 && atlas_frame.height == 17,
          "adaptive atlas source rectangles must map against decoded texture dimensions");
    const auto atlas_lower_half = MapTextureRect(0, 32, 192, 32, 192U, 64U, 128U, 43U);
    Check(atlas_lower_half.x == 0 && atlas_lower_half.y == 22 && atlas_lower_half.width == 128 &&
              atlas_lower_half.height == 21,
          "adaptive atlas source rectangles must not round past the decoded far edge");

    const auto landscape = MakeDisplayTransform(1280U, 720U);
    Check(landscape.logical_width == 1280U && landscape.logical_height == 720U,
          "a landscape display must keep a 720 logical short edge");
    Check(landscape.physical_width == 1280U && landscape.physical_height == 720U && landscape.offset_x == 0 &&
              landscape.offset_y == 0,
          "the SDK must expose the complete physical display without a manifest viewport");

    const auto portrait = MakeDisplayTransform(720U, 1280U);
    Check(portrait.logical_width == 720U && portrait.logical_height == 1280U,
          "a portrait display must keep a 720 logical short edge");

    Check(MakeDisplayTransform(0U, 720U).scale_denominator == 0U, "zero-sized displays must be rejected");
    std::cout << "guest display transform tests passed\n";
    return 0;
}
