#include <cstdlib>
#include <cstring>
#include <iostream>

#include "sdk/ui/text_button.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void CentersTheCompleteLineBoxOnBothAxes() {
    auto position = micropixel::ui::TextButton::CenteredLabelPosition(micropixel::Rect{10, 20, 101, 61},
                                                                      micropixel::TextMetrics{31U, 19U, 15});
    Check(position.has_value() && position->x == 60 && position->y == 41,
          "label anchor and line top must be centered in the complete button rect");
}

void RejectsTextThatDoesNotFit() {
    using micropixel::ui::TextOverflow;
    auto too_wide = micropixel::ui::TextButton::CenteredLabelPosition(
        micropixel::Rect{0, 0, 40, 20}, micropixel::TextMetrics{41U, 10U, 8}, TextOverflow::kReject);
    auto too_tall = micropixel::ui::TextButton::CenteredLabelPosition(
        micropixel::Rect{0, 0, 40, 20}, micropixel::TextMetrics{30U, 21U, 16}, TextOverflow::kReject);
    Check(!too_wide.has_value() && !too_tall.has_value(), "text outside either axis must be rejected");
}

void ClipsAndCentersTextThatDoesNotFit() {
    using micropixel::ui::TextOverflow;
    auto too_wide = micropixel::ui::TextButton::CenteredLabelPosition(
        micropixel::Rect{0, 0, 40, 20}, micropixel::TextMetrics{41U, 10U, 8}, TextOverflow::kClip);
    auto too_tall = micropixel::ui::TextButton::CenteredLabelPosition(
        micropixel::Rect{0, 0, 40, 20}, micropixel::TextMetrics{30U, 24U, 18}, TextOverflow::kClip);
    Check(too_wide.has_value() && *too_wide == micropixel::Point{20, 5},
          "clipped wide text must remain centered in the button");
    Check(too_tall.has_value() && *too_tall == micropixel::Point{20, -2},
          "clipped tall text must remain centered across the clipped bounds");
    Check(micropixel::ui::TextButton::TextExceedsBounds(micropixel::Rect{0, 0, 40, 20},
                                                        micropixel::TextMetrics{41U, 10U, 8}),
          "overflow state must report text wider than its button");
}

void UsesClipAsTheDefaultPolicy() {
    const micropixel::ui::TextButtonProperties properties{};
    Check(properties.overflow == micropixel::ui::TextOverflow::kClip,
          "text buttons must clip overflow unless strict rejection is requested");
}

void DescribesTheSpecificOverflowingButton() {
    const auto diagnostic = micropixel::ui::TextButton::FormatOverflowDiagnostic(
        "clipped", micropixel::Rect{240, 510, 240, 72}, micropixel::TextMetrics{255U, 48U, 39}, "RUN\nFROM 01",
        micropixel::ui::TextOverflow::kClip, true);
    Check(std::strcmp(diagnostic.c_str(),
                      "TextButton overflow: action=clipped bounds=(x=240,y=510,w=240,h=72) "
                      "measured=(w=255,h=48) overflow=clip clipped=true text=[RUN?FROM 01]") == 0,
          "overflow diagnostic must identify the action, bounds, measured size and sanitized label text");
}

void DescribesObjectStateWithoutAllocation() {
    const micropixel::ui::TextButton button;
    const auto description = button.ToString();
    Check(std::strcmp(description.c_str(),
                      "TextButton bounds=(x=0,y=0,w=0,h=0) measured=(w=0,h=0) overflow=clip "
                      "clipped=false text=[]") == 0,
          "TextButton::ToString must include bounds, metrics, policy, clipping state and text");
}

void RejectsInvalidAndOverflowingBounds() {
    auto empty = micropixel::ui::TextButton::CenteredLabelPosition(micropixel::Rect{0, 0, 0, 20},
                                                                   micropixel::TextMetrics{10U, 10U, 8});
    auto overflow = micropixel::ui::TextButton::CenteredLabelPosition(micropixel::Rect{INT32_MAX, 0, 8, 20},
                                                                      micropixel::TextMetrics{4U, 10U, 8});
    Check(!empty.has_value() && !overflow.has_value(), "invalid logical rectangles must be rejected");
}

}  // namespace

int main() {
    CentersTheCompleteLineBoxOnBothAxes();
    RejectsTextThatDoesNotFit();
    ClipsAndCentersTextThatDoesNotFit();
    UsesClipAsTheDefaultPolicy();
    DescribesTheSpecificOverflowingButton();
    DescribesObjectStateWithoutAllocation();
    RejectsInvalidAndOverflowingBounds();
    std::cout << "guest text button tests passed: 7 cases\n";
    return 0;
}
