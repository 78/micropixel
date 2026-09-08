#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "sdk/ui/button.hpp"
#include "sdk/ui/flex_container.hpp"
#include "sdk/ui/image_button.hpp"
#include "sdk/ui/label.hpp"
#include "sdk/ui/layout.hpp"
#include "sdk/ui/text_button.hpp"

namespace micropixel {

class Application final {
   public:
    [[nodiscard]] static constexpr TouchEvent Touch(TouchPhase phase, uint32_t id, int32_t x, int32_t y,
                                                    uint64_t timestamp_us = 0U) {
        return TouchEvent{TimePoint{timestamp_us}, phase, id, x, y, false, 0U};
    }
};

}  // namespace micropixel

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpandedHitAreaCapturesWithoutChangingVisualBounds() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    Check(button.bounds() == micropixel::Rect{100, 100, 32, 32}, "visual bounds must remain unchanged");
    Check(button.hit_bounds() == micropixel::Rect{94, 94, 44, 44}, "hit padding must expand all four edges");

    const auto down = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kDown, 7U, 95, 110, 100U));
    Check(down.handled && down.visual_changed && !down.clicked && button.pressed(),
          "a down inside only the expanded area must capture and press the button");
    const auto up = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kUp, 7U, 95, 110, 200U));
    Check(up.handled && up.clicked && !button.tracking(), "release inside the expanded area must click once");
}

void BackgroundTouchCannotRetargetOnRelease() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    const auto down = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kDown, 3U, 93, 110, 100U));
    Check(!down.handled && !button.tracking(), "a down outside the expanded area must remain unclaimed");
    const auto up = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kUp, 3U, 110, 110, 200U));
    Check(!up.handled && !up.clicked, "an unclaimed background touch must not retarget to the button on release");
}

void CapturedTouchCanLeaveAndReturn() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    (void)button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kDown, 11U, 95, 110));
    const auto leave = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kMove, 11U, 90, 110));
    Check(leave.handled && leave.visual_changed && !button.pressed(),
          "leaving the expanded area must clear pressed feedback");
    const auto enter = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kMove, 11U, 95, 110));
    Check(enter.handled && enter.visual_changed && button.pressed(),
          "returning to the expanded area must restore pressed feedback");
    const auto up = button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kUp, 11U, 95, 110));
    Check(up.clicked, "a captured touch returning before release must click");
}

void DescribesInteractionState() {
    micropixel::ui::Button button{{100, 100, 32, 32}, 6U};
    (void)button.OnTouch(micropixel::Application::Touch(micropixel::TouchPhase::kDown, 11U, 95, 110));
    const auto description = button.ToString();
    Check(std::strcmp(description.c_str(),
                      "Button bounds=(x=100,y=100,w=32,h=32) hit_bounds=(x=94,y=94,w=44,h=44) "
                      "enabled=true tracking=true pressed=true touch_id=11") == 0,
          "Button::ToString must include geometry and captured touch state");
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

void RejectsInvalidAndOverflowingBounds() {
    auto empty = micropixel::ui::TextButton::CenteredLabelPosition(micropixel::Rect{0, 0, 0, 20},
                                                                   micropixel::TextMetrics{10U, 10U, 8});
    auto overflow = micropixel::ui::TextButton::CenteredLabelPosition(micropixel::Rect{INT32_MAX, 0, 8, 20},
                                                                      micropixel::TextMetrics{4U, 10U, 8});
    Check(!empty.has_value() && !overflow.has_value(), "invalid logical rectangles must be rejected");
}

void TestGuestLayout() {
    using micropixel::ErrorCode;
    using micropixel::Rect;
    using micropixel::ui::ComputeFlexLayout;
    using micropixel::ui::ComputeGridLayout;
    using micropixel::ui::FlexAlignment;
    using micropixel::ui::FlexDirection;
    using micropixel::ui::FlexDistribution;
    using micropixel::ui::FlexItem;
    using micropixel::ui::FlexLayout;
    using micropixel::ui::GridLayout;
    using micropixel::ui::LayoutLength;

    {
        constexpr std::array items{FlexItem::Fixed(40U), FlexItem::Grow(), FlexItem::Grow(2U)};
        std::array<Rect, items.size()> output{};
        auto result = ComputeFlexLayout(
            Rect{0, 0, 300, 80},
            FlexLayout{.direction = FlexDirection::kHorizontal, .padding = {10, 10, 10, 10}, .gap_pixels = 10}, items,
            output);
        Check(result.has_value(), "horizontal fixed/grow layout must succeed");
        Check(output[0] == Rect{10, 10, 40, 60}, "fixed item must preserve its pixel size");
        Check(output[1] == Rect{60, 10, 73, 60}, "first grow item must receive one third of free space");
        Check(output[2] == Rect{143, 10, 147, 60}, "last grow item must receive the exact remainder");
    }

    {
        constexpr std::array items{FlexItem::Fixed(30U), FlexItem::Fixed(30U), FlexItem::Fixed(30U)};
        std::array<Rect, items.size()> output{};
        auto result = ComputeFlexLayout(Rect{0, 0, 101, 60},
                                        FlexLayout{.direction = FlexDirection::kHorizontal,
                                                   .distribution = FlexDistribution::kSpaceBetween,
                                                   .alignment = FlexAlignment::kCenter},
                                        items, output);
        Check(result.has_value(), "space-between layout must succeed");
        Check(output[0] == Rect{0, 0, 30, 60} && output[1] == Rect{36, 0, 30, 60} && output[2] == Rect{71, 0, 30, 60},
              "space-between must distribute odd remainder deterministically");
    }

    {
        constexpr std::array items{
            FlexItem{LayoutLength::Pixels(20U), LayoutLength::Pixels(40U)},
            FlexItem{LayoutLength::Pixels(20U), LayoutLength::Pixels(40U)},
        };
        std::array<Rect, items.size()> output{};
        auto result = ComputeFlexLayout(Rect{5, 7, 100, 80},
                                        FlexLayout{.direction = FlexDirection::kVertical,
                                                   .gap_pixels = 10,
                                                   .distribution = FlexDistribution::kCenter,
                                                   .alignment = FlexAlignment::kEnd},
                                        items, output);
        Check(result.has_value(), "vertical centered layout must succeed");
        Check(output[0] == Rect{65, 22, 40, 20} && output[1] == Rect{65, 52, 40, 20},
              "main and cross alignment must use physical bounds");
    }

    {
        constexpr std::array items{FlexItem::Fixed(60U), FlexItem::Fixed(60U)};
        std::array<Rect, items.size()> output{Rect{1, 2, 3, 4}, Rect{5, 6, 7, 8}};
        auto result = ComputeFlexLayout(Rect{0, 0, 100, 100}, FlexLayout{}, items, output);
        Check(!result.has_value() && result.error().code() == ErrorCode::kResourceExhausted,
              "oversubscribed fixed layout must fail clearly");
        Check(output[0] == Rect{1, 2, 3, 4} && output[1] == Rect{5, 6, 7, 8}, "failed layout must not modify output");
    }

    {
        constexpr std::array items{FlexItem::Fixed(20U), FlexItem::Fixed(20U)};
        std::array<Rect, 1U> output{};
        auto result = ComputeFlexLayout(Rect{0, 0, 100, 100}, FlexLayout{}, items, output);
        Check(!result.has_value() && result.error().code() == ErrorCode::kBufferTooSmall,
              "undersized output must report buffer-too-small");
    }

    {
        constexpr std::array items{FlexItem::Grow()};
        std::array<Rect, 1U> output{};
        auto result = ComputeFlexLayout(Rect{INT32_MAX - 4, 0, 8, 8}, FlexLayout{}, items, output);
        Check(!result.has_value() && result.error().code() == ErrorCode::kInvalidArgument,
              "layout coordinates that exceed int32 must be rejected");
    }

    {
        constexpr std::array rows{FlexItem::Fixed(40U), FlexItem::Fixed(50U)};
        constexpr std::array columns{FlexItem::Grow(), FlexItem::Grow(), FlexItem::Grow()};
        std::array<Rect, rows.size() * columns.size()> output{};
        auto result = ComputeGridLayout(Rect{0, 0, 300, 100},
                                        GridLayout{.rows = {.direction = FlexDirection::kVertical, .gap_pixels = 10},
                                                   .columns = {.direction = FlexDirection::kHorizontal}},
                                        rows, columns, output);
        Check(result.has_value(), "two-dimensional fixed/grow grid must succeed");
        Check(output[0] == Rect{0, 0, 100, 40} && output[2] == Rect{200, 0, 100, 40} &&
                  output[3] == Rect{0, 50, 100, 50} && output[5] == Rect{200, 50, 100, 50},
              "grid cells must be emitted in row-major order");
    }

    {
        constexpr std::array rows{FlexItem::Grow()};
        constexpr std::array columns{FlexItem::Grow()};
        std::array<Rect, 1U> output{Rect{1, 2, 3, 4}};
        auto result = ComputeGridLayout(Rect{0, 0, 100, 100},
                                        GridLayout{.rows = {.direction = FlexDirection::kHorizontal},
                                                   .columns = {.direction = FlexDirection::kHorizontal}},
                                        rows, columns, output);
        Check(!result.has_value() && result.error().code() == ErrorCode::kInvalidArgument,
              "grid must reject non-vertical row layout");
        Check(output[0] == Rect{1, 2, 3, 4}, "failed grid layout must not modify output");
    }
}

void DescribesEmptyFlexContainer() {
    const micropixel::ui::FlexContainer container;
    const auto description = container.ToString();
    Check(std::strcmp(description.c_str(),
                      "FlexContainer bounds=(x=0,y=0,w=0,h=0) direction=vertical children=0 labels=0 "
                      "grids=0 image_buttons=0 text_buttons=0") == 0,
          "FlexContainer::ToString must summarize bounds, direction and typed child counts");
}

void DescribesEmptyGridContainer() {
    const micropixel::ui::GridContainer container;
    const auto description = container.ToString();
    Check(std::strcmp(description.c_str(),
                      "GridContainer bounds=(x=0,y=0,w=0,h=0) rows=0 inferred_rows=0 columns=0 cells=0 "
                      "row_gap=0 column_gap=0") == 0,
          "GridContainer::ToString must summarize bounds, tracks, cells and gaps");
}

void DescribesLabel() {
    const micropixel::ui::Label label;
    const auto description = label.ToString();
    Check(std::strcmp(description.c_str(),
                      "Label bounds=(x=0,y=0,w=0,h=0) measured=(w=0,h=0) horizontal=center "
                      "vertical=center text=[]") == 0,
          "Label::ToString must include bounds, metrics, alignment and text");
}

void DescribesImageButton() {
    const micropixel::ui::ImageButton button;
    const auto description = button.ToString();
    Check(std::strcmp(description.c_str(),
                      "ImageButton bounds=(x=0,y=0,w=0,h=0) measured=(w=0,h=0) overflow=clip "
                      "clipped=false text=[]") == 0,
          "ImageButton::ToString must include bounds, metrics, policy, clipping state and text");
}

void UsesClipAsTheDefaultPolicy() {
    const micropixel::ui::TextButtonProperties properties{};
    const micropixel::ui::ImageButtonProperties image_properties{};
    Check(properties.overflow == micropixel::ui::TextOverflow::kClip &&
              image_properties.overflow == micropixel::ui::TextOverflow::kClip,
          "text and image buttons must clip overflow unless strict rejection is requested");
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

void DescribesTextButton() {
    const micropixel::ui::TextButton button;
    const auto description = button.ToString();
    Check(std::strcmp(description.c_str(),
                      "TextButton bounds=(x=0,y=0,w=0,h=0) measured=(w=0,h=0) overflow=clip "
                      "clipped=false text=[]") == 0,
          "TextButton::ToString must include bounds, metrics, policy, clipping state and text");
}

}  // namespace

int main() {
    ExpandedHitAreaCapturesWithoutChangingVisualBounds();
    BackgroundTouchCannotRetargetOnRelease();
    CapturedTouchCanLeaveAndReturn();
    DescribesInteractionState();
    CentersTheCompleteLineBoxOnBothAxes();
    RejectsTextThatDoesNotFit();
    ClipsAndCentersTextThatDoesNotFit();
    RejectsInvalidAndOverflowingBounds();
    TestGuestLayout();
    DescribesLabel();
    DescribesImageButton();
    UsesClipAsTheDefaultPolicy();
    DescribesTheSpecificOverflowingButton();
    DescribesTextButton();
    DescribesEmptyFlexContainer();
    DescribesEmptyGridContainer();
    return 0;
}
