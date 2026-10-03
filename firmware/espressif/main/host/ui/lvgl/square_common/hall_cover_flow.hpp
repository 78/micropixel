#pragma once

#include <array>
#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common {

// Cover Flow presentation of the Hall carousel. Cards keep a linear track of
// one card step each; the centred card is full size and its neighbours shrink,
// move in under it, dim and fade, so the visual spacing tightens toward the
// edges. Distances are in 1/256 of a card step from the viewport centre.
// Applied without LVGL layers: the card is resized, its cover is drawn at that
// size, `dim` recolors the cover toward black and `opacity` fades the card out
// near the hidden distance.
struct CoverFlowCardStyle final {
    int32_t scale{256};
    int32_t translate_x{};
    uint8_t dim{};
    uint8_t opacity{255};
    bool visible{true};
};

namespace cover_flow {

inline constexpr int32_t kUnit = 256;
// Up to three card steps away; beyond that a card is hidden.
inline constexpr int32_t kHiddenDistance = 3 * kUnit;
inline constexpr int32_t kFadeDistance = 5 * kUnit / 2;
// Size relative to the centred card, horizontal offset of the card centre in
// card widths, and dimming, at 0, 1, 2 and 3 steps from the centre.
inline constexpr std::array<int32_t, 4> kScale{256, 186, 146, 120};
inline constexpr std::array<int32_t, 4> kCentreOffset{0, 158, 256, 330};
inline constexpr std::array<int32_t, 4> kDim{0, 120, 175, 215};

[[nodiscard]] constexpr int32_t Interpolate(const std::array<int32_t, 4>& table, int32_t distance) {
    int32_t index = distance / kUnit;
    int32_t fraction = distance % kUnit;
    if (index >= 3) {
        index = 2;
        fraction = kUnit;
    }
    return table[index] + (table[index + 1] - table[index]) * fraction / kUnit;
}

}  // namespace cover_flow

// Leading and trailing padding of the carousel content, so the first and last
// cards can be centred in the viewport.
[[nodiscard]] constexpr int32_t CoverFlowEdgeWidth(int32_t viewport_width, int32_t card_width) {
    return viewport_width > card_width ? (viewport_width - card_width) / 2 : 0;
}

// `distance` is the signed distance of the card centre from the viewport
// centre along the linear track, in 1/256 card steps.
[[nodiscard]] constexpr CoverFlowCardStyle CoverFlowStyleFor(int32_t distance, int32_t card_width, int32_t card_step) {
    const int32_t magnitude = distance < 0 ? -distance : distance;
    if (magnitude >= cover_flow::kHiddenDistance) {
        return {.scale = cover_flow::kScale[3], .translate_x = 0, .dim = 0, .opacity = 0, .visible = false};
    }
    const int32_t centre_offset =
        card_width * cover_flow::Interpolate(cover_flow::kCentreOffset, magnitude) / cover_flow::kUnit;
    const int32_t linear_offset = magnitude * card_step / cover_flow::kUnit;
    const int32_t shift = centre_offset - linear_offset;
    int32_t opacity = 255;
    if (magnitude > cover_flow::kFadeDistance) {
        opacity = opacity * (cover_flow::kHiddenDistance - magnitude) /
                  (cover_flow::kHiddenDistance - cover_flow::kFadeDistance);
    }
    return {.scale = cover_flow::Interpolate(cover_flow::kScale, magnitude),
            .translate_x = distance < 0 ? -shift : shift,
            .dim = static_cast<uint8_t>(cover_flow::Interpolate(cover_flow::kDim, magnitude)),
            .opacity = static_cast<uint8_t>(opacity < 0 ? 0 : opacity),
            .visible = true};
}

}  // namespace micropixel::host_ui::lvgl::square_common
