#include <array>
#include <cstdlib>
#include <iostream>

#include "sdk/ui/layout.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
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

    std::cout << "guest layout tests passed\n";
    return 0;
}
