#ifndef MICROPIXEL_SDK_UI_LAYOUT_HPP
#define MICROPIXEL_SDK_UI_LAYOUT_HPP

#include <stdint.h>

#include <array>
#include <span>

#include "sdk/geometry.hpp"
#include "sdk/result.hpp"

namespace micropixel::ui {

struct Insets final {
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
    int32_t left{};
};

enum class FlexDirection : uint8_t {
    kHorizontal,
    kVertical,
};

enum class FlexDistribution : uint8_t {
    kStart,
    kCenter,
    kEnd,
    kSpaceBetween,
};

enum class FlexAlignment : uint8_t {
    kStart,
    kCenter,
    kEnd,
    kStretch,
};

class LayoutLength final {
   public:
    [[nodiscard]] static constexpr LayoutLength Pixels(uint32_t pixels) { return LayoutLength{Kind::kPixels, pixels}; }

    [[nodiscard]] static constexpr LayoutLength Grow(uint16_t weight = 1U) { return LayoutLength{Kind::kGrow, weight}; }

    [[nodiscard]] static constexpr LayoutLength Fill() { return LayoutLength{Kind::kFill, 0U}; }

   private:
    enum class Kind : uint8_t {
        kPixels,
        kGrow,
        kFill,
    };

    constexpr LayoutLength(Kind kind, uint32_t value) : kind_(kind), value_(value) {}

    Kind kind_{};
    uint32_t value_{};

    friend Result<void> ComputeFlexLayout(Rect, const struct FlexLayout&, std::span<const struct FlexItem>,
                                          std::span<Rect>);
};

struct FlexItem final {
    LayoutLength main_size{LayoutLength::Grow()};
    LayoutLength cross_size{LayoutLength::Fill()};

    [[nodiscard]] static constexpr FlexItem Fixed(uint32_t pixels) {
        return FlexItem{LayoutLength::Pixels(pixels), LayoutLength::Fill()};
    }

    [[nodiscard]] static constexpr FlexItem Grow(uint16_t weight = 1U) {
        return FlexItem{LayoutLength::Grow(weight), LayoutLength::Fill()};
    }
};

struct FlexLayout final {
    FlexDirection direction{FlexDirection::kVertical};
    Insets padding{};
    int32_t gap_pixels{};
    FlexDistribution distribution{FlexDistribution::kStart};
    FlexAlignment alignment{FlexAlignment::kStretch};
};

struct GridLayout final {
    FlexLayout rows{.direction = FlexDirection::kVertical};
    FlexLayout columns{.direction = FlexDirection::kHorizontal};
};

inline constexpr size_t kMaxGridTracks = 8U;

// Computes physical rectangles into caller-owned storage. This is a pure Guest-side
// geometry operation: it performs no allocation and never calls a Host service.
[[nodiscard]] inline Result<void> ComputeFlexLayout(Rect bounds, const FlexLayout& layout,
                                                    std::span<const FlexItem> items, std::span<Rect> output) {
    if (output.size() < items.size()) {
        return unexpected(Error{ErrorCode::kBufferTooSmall});
    }
    if (bounds.empty() || layout.padding.top < 0 || layout.padding.right < 0 || layout.padding.bottom < 0 ||
        layout.padding.left < 0 || layout.gap_pixels < 0) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    const int64_t bounds_right = static_cast<int64_t>(bounds.x) + bounds.width;
    const int64_t bounds_bottom = static_cast<int64_t>(bounds.y) + bounds.height;
    if (bounds_right > INT32_MAX || bounds_bottom > INT32_MAX) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }

    const int64_t inner_width = static_cast<int64_t>(bounds.width) - layout.padding.left - layout.padding.right;
    const int64_t inner_height = static_cast<int64_t>(bounds.height) - layout.padding.top - layout.padding.bottom;
    if (inner_width < 0 || inner_height < 0) {
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }

    const bool horizontal = layout.direction == FlexDirection::kHorizontal;
    const int64_t inner_main = horizontal ? inner_width : inner_height;
    const int64_t inner_cross = horizontal ? inner_height : inner_width;
    const int64_t gap_count = items.empty() ? 0 : static_cast<int64_t>(items.size() - 1U);
    const int64_t fixed_gaps = gap_count * layout.gap_pixels;
    if (fixed_gaps > inner_main) {
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }

    int64_t fixed_pixels = 0;
    uint64_t grow_weight = 0U;
    for (const FlexItem& item : items) {
        if (item.main_size.kind_ == LayoutLength::Kind::kFill ||
            (item.main_size.kind_ == LayoutLength::Kind::kGrow && item.main_size.value_ == 0U) ||
            item.cross_size.kind_ == LayoutLength::Kind::kGrow) {
            return unexpected(Error{ErrorCode::kInvalidArgument});
        }
        if (item.main_size.kind_ == LayoutLength::Kind::kPixels) {
            fixed_pixels += item.main_size.value_;
        } else {
            grow_weight += item.main_size.value_;
        }
        if (item.cross_size.kind_ == LayoutLength::Kind::kPixels && item.cross_size.value_ > inner_cross) {
            return unexpected(Error{ErrorCode::kResourceExhausted});
        }
    }
    if (fixed_pixels + fixed_gaps > inner_main) {
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }

    const int64_t free_pixels = inner_main - fixed_pixels - fixed_gaps;
    int64_t leading_pixels = 0;
    int64_t distributed_gap_pixels = 0;
    int64_t distributed_gap_remainder = 0;
    if (grow_weight == 0U) {
        switch (layout.distribution) {
            case FlexDistribution::kStart:
                break;
            case FlexDistribution::kCenter:
                leading_pixels = free_pixels / 2;
                break;
            case FlexDistribution::kEnd:
                leading_pixels = free_pixels;
                break;
            case FlexDistribution::kSpaceBetween:
                if (gap_count > 0) {
                    distributed_gap_pixels = free_pixels / gap_count;
                    distributed_gap_remainder = free_pixels % gap_count;
                }
                break;
        }
    }

    int64_t main_position = (horizontal ? static_cast<int64_t>(bounds.x) + layout.padding.left
                                        : static_cast<int64_t>(bounds.y) + layout.padding.top) +
                            leading_pixels;
    const int64_t cross_origin = horizontal ? static_cast<int64_t>(bounds.y) + layout.padding.top
                                            : static_cast<int64_t>(bounds.x) + layout.padding.left;
    uint64_t grow_remainder = 0U;
    for (size_t index = 0U; index < items.size(); ++index) {
        const FlexItem& item = items[index];
        int64_t main_size = item.main_size.value_;
        if (item.main_size.kind_ == LayoutLength::Kind::kGrow) {
            const uint64_t numerator = static_cast<uint64_t>(free_pixels) * item.main_size.value_ + grow_remainder;
            main_size = static_cast<int64_t>(numerator / grow_weight);
            grow_remainder = numerator % grow_weight;
        }

        int64_t cross_size = inner_cross;
        int64_t cross_position = cross_origin;
        if (item.cross_size.kind_ == LayoutLength::Kind::kPixels) {
            cross_size = item.cross_size.value_;
            switch (layout.alignment) {
                case FlexAlignment::kStart:
                case FlexAlignment::kStretch:
                    break;
                case FlexAlignment::kCenter:
                    cross_position += (inner_cross - cross_size) / 2;
                    break;
                case FlexAlignment::kEnd:
                    cross_position += inner_cross - cross_size;
                    break;
            }
        }

        output[index] = horizontal ? Rect{static_cast<int32_t>(main_position), static_cast<int32_t>(cross_position),
                                          static_cast<int32_t>(main_size), static_cast<int32_t>(cross_size)}
                                   : Rect{static_cast<int32_t>(cross_position), static_cast<int32_t>(main_position),
                                          static_cast<int32_t>(cross_size), static_cast<int32_t>(main_size)};
        main_position += main_size;
        if (index + 1U < items.size()) {
            main_position += layout.gap_pixels + distributed_gap_pixels;
            if (static_cast<int64_t>(index) < distributed_gap_remainder) {
                ++main_position;
            }
        }
    }
    return {};
}

// Computes a row-major grid by composing one vertical and one horizontal Flex
// layout. Row and column tracks can independently use fixed or grow sizing.
// The fixed capacity keeps the helper allocation-free and makes failure atomic:
// output is modified only after every cell has been computed successfully.
[[nodiscard]] inline Result<void> ComputeGridLayout(Rect bounds, const GridLayout& layout,
                                                    std::span<const FlexItem> rows, std::span<const FlexItem> columns,
                                                    std::span<Rect> output) {
    if (rows.empty() || columns.empty() || layout.rows.direction != FlexDirection::kVertical ||
        layout.columns.direction != FlexDirection::kHorizontal) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    if (rows.size() > kMaxGridTracks || columns.size() > kMaxGridTracks) {
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }
    const size_t cell_count = rows.size() * columns.size();
    if (output.size() < cell_count) {
        return unexpected(Error{ErrorCode::kBufferTooSmall});
    }

    std::array<Rect, kMaxGridTracks> row_rects{};
    auto rows_laid_out = ComputeFlexLayout(bounds, layout.rows, rows, std::span{row_rects}.first(rows.size()));
    if (!rows_laid_out.has_value()) {
        return unexpected(rows_laid_out.error());
    }

    std::array<Rect, kMaxGridTracks * kMaxGridTracks> cells{};
    for (size_t row = 0U; row < rows.size(); ++row) {
        auto row_output = std::span{cells}.subspan(row * columns.size(), columns.size());
        auto columns_laid_out = ComputeFlexLayout(row_rects[row], layout.columns, columns, row_output);
        if (!columns_laid_out.has_value()) {
            return unexpected(columns_laid_out.error());
        }
    }
    for (size_t index = 0U; index < cell_count; ++index) {
        output[index] = cells[index];
    }
    return {};
}

}  // namespace micropixel::ui

#endif
