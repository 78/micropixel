#ifndef MICROPIXEL_SDK_UI_GRID_CONTAINER_HPP
#define MICROPIXEL_SDK_UI_GRID_CONTAINER_HPP

#include <vector>

#include "sdk/ui/label.hpp"
#include "sdk/ui/layout.hpp"

namespace micropixel::ui {

struct GridContainerProperties final {
    Rect bounds{};
    // Zero means infer the row count from sequentially created children.
    uint8_t rows{};
    uint8_t columns{};
    int32_t row_gap{};
    int32_t column_gap{};
    FlexDistribution vertical_distribution{FlexDistribution::kCenter};
};

class GridContainer final {
   public:
    static constexpr uint32_t kDiagnosticBytes = 320U;

    GridContainer() = default;
    GridContainer(const GridContainer&) = delete;
    GridContainer& operator=(const GridContainer&) = delete;
    GridContainer(GridContainer&&) noexcept = default;
    GridContainer& operator=(GridContainer&&) noexcept = default;

    [[nodiscard]] static GridContainer CreateIn(Container& parent, GridContainerProperties properties) {
        Assert(properties.columns != 0U && properties.columns <= kMaxGridTracks &&
                   (properties.rows == 0U || properties.rows <= kMaxGridTracks) && properties.row_gap >= 0 &&
                   properties.column_gap >= 0,
               "grid container properties invalid");
        GridContainer result;
        result.node_ = parent.CreateContainer({.translation = {properties.bounds.x, properties.bounds.y}});
        result.properties_ = properties;
        result.bounds_ = properties.bounds;
        return result;
    }

    uint16_t CreateLabel(const char* text, LabelStyle style = {}) {
        while (next_cell_ < kMaxGridTracks * properties_.columns) {
            const uint8_t row = static_cast<uint8_t>(next_cell_ / properties_.columns);
            const uint8_t column = static_cast<uint8_t>(next_cell_ % properties_.columns);
            ++next_cell_;
            if (FindCell(row, column) == nullptr) {
                if (properties_.rows != 0U && row >= properties_.rows) {
                    PanicCell("row-capacity-exhausted", row, column);
                }
                return CreateLabel(row, column, text, style);
            }
        }
        PanicCell("capacity-exhausted", kMaxGridTracks, 0U);
    }

    [[nodiscard]] uint16_t CreateLabel(uint8_t row, uint8_t column, const char* text, LabelStyle style = {}) {
        if (row >= kMaxGridTracks || column >= properties_.columns ||
            (properties_.rows != 0U && row >= properties_.rows)) {
            PanicCell("cell-invalid", row, column);
        }
        if (FindCell(row, column) != nullptr) {
            PanicCell("cell-occupied", row, column);
        }
        cells_.push_back({row, column, Label::CreateIn(node_, text, style)});
        const uint8_t occupied_rows = static_cast<uint8_t>(row + 1U);
        if (occupied_rows > inferred_rows_) {
            inferred_rows_ = occupied_rows;
        }
        return static_cast<uint16_t>(cells_.size() - 1U);
    }

    [[nodiscard]] Label& label(uint16_t id) {
        if (id >= cells_.size()) {
            FixedString<512U> diagnostic;
            diagnostic.Append("GridContainer label id invalid: requested=");
            diagnostic.AppendUint(id);
            diagnostic.Append(" count=");
            diagnostic.AppendUint(cells_.size());
            diagnostic.Append(" state={");
            const auto state = ToString();
            diagnostic.Append(state.c_str());
            diagnostic.Append("}");
            Panic(diagnostic.c_str());
        }
        return cells_[id].label;
    }

    [[nodiscard]] Result<void> SetText(SceneUpdate& update, uint8_t row, uint8_t column, const char* text) {
        Cell* cell = FindCell(row, column);
        if (cell == nullptr) {
            return unexpected(Error{ErrorCode::kInvalidArgument});
        }
        return cell->label.SetText(update, text);
    }

    [[nodiscard]] Result<void> SetColor(SceneUpdate& update, uint8_t row, uint8_t column, Color color) {
        Cell* cell = FindCell(row, column);
        if (cell == nullptr) {
            return unexpected(Error{ErrorCode::kInvalidArgument});
        }
        cell->label.SetColor(update, color);
        return {};
    }

    [[nodiscard]] Size intrinsic_size() const {
        uint32_t widest_column = 0U;
        uint32_t height = 0U;
        const uint8_t row_count = RowCount();
        for (uint8_t column = 0U; column < properties_.columns; ++column) {
            const uint32_t column_width = ColumnWidth(column);
            if (column_width > widest_column) {
                widest_column = column_width;
            }
        }
        for (uint8_t row = 0U; row < row_count; ++row) {
            height += RowHeight(row);
        }
        const uint32_t width = widest_column * properties_.columns +
                               static_cast<uint32_t>(properties_.column_gap) * (properties_.columns - 1U);
        height += row_count == 0U ? 0U : static_cast<uint32_t>(properties_.row_gap) * (row_count - 1U);
        return {width, height};
    }

    [[nodiscard]] Result<void> SetBounds(SceneUpdate& update, Rect bounds) {
        if (bounds.empty()) {
            return unexpected(Error{ErrorCode::kInvalidArgument});
        }
        bounds_ = bounds;
        node_.SetTranslation(update, {bounds.x, bounds.y});
        const uint8_t row_count = RowCount();
        if (row_count == 0U) {
            return {};
        }

        std::vector<FlexItem> rows(row_count);
        std::vector<FlexItem> columns(properties_.columns);
        for (uint8_t row = 0U; row < row_count; ++row) {
            rows[row] = FlexItem::Fixed(RowHeight(row));
        }
        for (FlexItem& column : columns) {
            column = FlexItem::Grow();
        }
        std::vector<Rect> cell_bounds(static_cast<size_t>(row_count) * properties_.columns);
        const auto laid_out = ComputeGridLayout(
            {0, 0, bounds.width, bounds.height},
            {.rows = {.direction = FlexDirection::kVertical,
                      .gap_pixels = properties_.row_gap,
                      .distribution = properties_.vertical_distribution},
             .columns = {.direction = FlexDirection::kHorizontal, .gap_pixels = properties_.column_gap}},
            rows, columns, cell_bounds);
        if (!laid_out.has_value()) {
            return unexpected(laid_out.error());
        }
        for (Cell& cell : cells_) {
            auto positioned = cell.label.SetBounds(
                update, cell_bounds[static_cast<size_t>(cell.row) * properties_.columns + cell.column]);
            if (!positioned.has_value()) {
                return unexpected(positioned.error());
            }
        }
        return {};
    }

    void SetVisible(SceneUpdate& update, bool visible) { node_.SetVisible(update, visible); }

    [[nodiscard]] FixedString<kDiagnosticBytes> ToString() const {
        FixedString<kDiagnosticBytes> description;
        description.Append("GridContainer bounds=(x=");
        description.AppendInt(bounds_.x);
        description.Append(",y=");
        description.AppendInt(bounds_.y);
        description.Append(",w=");
        description.AppendInt(bounds_.width);
        description.Append(",h=");
        description.AppendInt(bounds_.height);
        description.Append(") rows=");
        description.AppendUint(properties_.rows);
        description.Append(" inferred_rows=");
        description.AppendUint(inferred_rows_);
        description.Append(" columns=");
        description.AppendUint(properties_.columns);
        description.Append(" cells=");
        description.AppendUint(cells_.size());
        description.Append(" row_gap=");
        description.AppendInt(properties_.row_gap);
        description.Append(" column_gap=");
        description.AppendInt(properties_.column_gap);
        return description;
    }

   private:
    struct Cell final {
        uint8_t row{};
        uint8_t column{};
        Label label{};
    };

    [[nodiscard]] uint8_t RowCount() const { return properties_.rows != 0U ? properties_.rows : inferred_rows_; }

    [[noreturn]] void PanicCell(const char* action, uint32_t row, uint32_t column) const {
        FixedString<512U> diagnostic;
        diagnostic.Append("GridContainer cell error: action=");
        diagnostic.Append(action);
        diagnostic.Append(" row=");
        diagnostic.AppendUint(row);
        diagnostic.Append(" column=");
        diagnostic.AppendUint(column);
        diagnostic.Append(" state={");
        const auto state = ToString();
        diagnostic.Append(state.c_str());
        diagnostic.Append("}");
        Panic(diagnostic.c_str());
    }

    [[nodiscard]] Cell* FindCell(uint8_t row, uint8_t column) {
        for (Cell& cell : cells_) {
            if (cell.row == row && cell.column == column) {
                return &cell;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Cell* FindCell(uint8_t row, uint8_t column) const {
        for (const Cell& cell : cells_) {
            if (cell.row == row && cell.column == column) {
                return &cell;
            }
        }
        return nullptr;
    }

    [[nodiscard]] uint32_t RowHeight(uint8_t row) const {
        uint32_t height = 1U;
        for (const Cell& cell : cells_) {
            if (cell.row == row && cell.label.intrinsic_size().height > height) {
                height = cell.label.intrinsic_size().height;
            }
        }
        return height;
    }

    [[nodiscard]] uint32_t ColumnWidth(uint8_t column) const {
        uint32_t width = 1U;
        for (const Cell& cell : cells_) {
            if (cell.column == column && cell.label.intrinsic_size().width > width) {
                width = cell.label.intrinsic_size().width;
            }
        }
        return width;
    }

    ContainerNode node_{};
    GridContainerProperties properties_{};
    Rect bounds_{};
    std::vector<Cell> cells_{};
    size_t next_cell_{};
    uint8_t inferred_rows_{};
};

}  // namespace micropixel::ui

namespace micropixel {

inline ui::GridContainer Container::CreateGridContainer(const ui::GridContainerProperties& properties) {
    return ui::GridContainer::CreateIn(*this, properties);
}

}  // namespace micropixel

#endif
