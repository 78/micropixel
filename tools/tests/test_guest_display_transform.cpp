#include <cstdlib>
#include <iostream>
#include <utility>

#include "runtime/bundle/app_requirements.h"
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
    using micropixel::DisplayConfiguration;
    using micropixel::DisplayScaleMode;
    using micropixel::detail::MakeDisplayTransform;
    constexpr DisplayConfiguration legacy{.logical_size = {720U, 720U}, .scale_mode = DisplayScaleMode::kExpand};
    using micropixel::detail::MapPhysicalInsets;
    using micropixel::detail::MapRect;
    using micropixel::detail::MapSceneRect;
    using micropixel::detail::MapSceneSizedRect;
    using micropixel::detail::MapSceneVectorX;
    using micropixel::detail::MapTextureRect;
    using micropixel::detail::ScaleCoordinate;

    for (const auto size : {std::pair{480U, 480U}, std::pair{720U, 720U}, std::pair{320U, 240U}, std::pair{240U, 320U},
                            std::pair{481U, 723U}, std::pair{0U, 480U}}) {
        micropixel_app_environment_t environment{};
        micropixel_app_set_logical_display(&environment, size.first, size.second);
        const auto guest = MakeDisplayTransform(size.first, size.second, legacy);
        Check(environment.width == guest.logical_width && environment.height == guest.logical_height,
              "store compatibility must match the Guest logical display, including Mosaico");
        micropixel_app_requirements_t requirement{};
        requirement.declared = true;
        requirement.min_width = 720U;
        requirement.min_height = 720U;
        requirement.layouts = 7U;
        Check(micropixel_app_is_compatible(&requirement, &environment) == (size.first != 0U),
              "720-unit apps must accept supported physical screens and reject an unknown display");
    }

    const auto square_720 = MakeDisplayTransform(720U, 720U, legacy);
    Check(square_720.logical_width == 720U && square_720.logical_height == 720U,
          "720 square must use the identity design space");
    Check(square_720.scale_numerator == 720U && square_720.scale_denominator == 720U,
          "720 square texture scale must be 1:1");

    const auto square_480 = MakeDisplayTransform(480U, 480U, legacy);
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

    const auto landscape = MakeDisplayTransform(1280U, 720U, legacy);
    Check(landscape.logical_width == 1280U && landscape.logical_height == 720U,
          "a landscape display must keep a 720 logical short edge");
    Check(landscape.physical_width == 1280U && landscape.physical_height == 720U && landscape.offset_x == 0 &&
              landscape.offset_y == 0,
          "the SDK must expose the complete physical display without a manifest viewport");

    const auto portrait = MakeDisplayTransform(720U, 1280U, legacy);
    Check(portrait.logical_width == 720U && portrait.logical_height == 1280U,
          "a portrait display must keep a 720 logical short edge");

    Check(MakeDisplayTransform(0U, 720U, legacy).scale_denominator == 0U, "zero-sized displays must be rejected");
    const auto native = MakeDisplayTransform(320, 240);
    Check(native.logical_width == 320 && native.logical_height == 240 && native.scale_numerator == 1 &&
              native.scale_denominator == 1,
          "unconfigured display must use native pixels");
    const auto fit = MakeDisplayTransform(480, 480, {{320, 240}, DisplayScaleMode::kAspectFit});
    Check(fit.logical_width == 320 && fit.logical_height == 240 && fit.viewport_width == 480 &&
              fit.viewport_height == 360 && fit.offset_y == 60,
          "fit centers a fixed canvas with letterboxing");
    const auto fit_rect = MapSceneRect(fit, 0, 0, 320, 240);
    Check(fit_rect.x == 0 && fit_rect.y == 60 && fit_rect.width == 480 && fit_rect.height == 360,
          "fit maps the full scene to its viewport");
    Check(MapSceneVectorX(fit, 20) == 30, "vectors must not include letterbox offsets");
    Check(ScaleCoordinate(60 - fit.offset_y, fit.logical_height, fit.viewport_height) == 0 &&
              ScaleCoordinate(30 - fit.offset_y, fit.logical_height, fit.viewport_height) == -20,
          "touch inverse maps viewport origin and leaves letterbox touches outside the canvas");
    const auto fit_safe = MapPhysicalInsets(fit, 24, 24, 24, 24);
    Check(fit_safe.top == 0 && fit_safe.bottom == 0 && fit_safe.left == 16 && fit_safe.right == 16,
          "letterboxing absorbs physical safe insets");
    const auto fill = MakeDisplayTransform(480, 480, {{320, 240}, DisplayScaleMode::kAspectFill});
    Check(fill.viewport_width == 640 && fill.viewport_height == 480 && fill.offset_x == -80,
          "fill centers and crops the larger viewport");
    const auto fill_safe = MapPhysicalInsets(fill, 0, 0, 0, 0);
    Check(fill_safe.left == 40 && fill_safe.right == 40, "cropped content lies outside the safe logical area");
    const auto expand = MakeDisplayTransform(480, 480, {{320, 240}, DisplayScaleMode::kExpand});
    Check(expand.logical_width == 320 && expand.logical_height == 320 && expand.offset_y == 0,
          "expand grows the design canvas without letterboxing");
    const auto portrait_fit = MakeDisplayTransform(240, 320, {{320, 240}, DisplayScaleMode::kAspectFit});
    Check(portrait_fit.viewport_height == 180 && portrait_fit.offset_y == 70,
          "portrait fit retains authored orientation");
    Check(MakeDisplayTransform(480, 480, {{0, 240}, DisplayScaleMode::kAspectFit}).logical_width == 0 &&
              MakeDisplayTransform(480, 480, {{320, 0}, DisplayScaleMode::kExpand}).logical_width == 0 &&
              MakeDisplayTransform(480, 480, {{320, 240}, DisplayScaleMode::kNative}).logical_width == 0 &&
              MakeDisplayTransform(480, 480, {{320, 240}, static_cast<DisplayScaleMode>(255)}).logical_width == 0 &&
              MakeDisplayTransform(480, 480, {{1, 32767}, DisplayScaleMode::kAspectFill}).logical_width == 0,
          "invalid and unrepresentable configurations must fail");
    std::cout << "guest display transform tests passed\n";
    return 0;
}
