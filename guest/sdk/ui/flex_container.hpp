#ifndef MICROPIXEL_SDK_UI_FLEX_CONTAINER_HPP
#define MICROPIXEL_SDK_UI_FLEX_CONTAINER_HPP

#include <memory>
#include <vector>

#include "sdk/ui/grid_container.hpp"
#include "sdk/ui/image_button.hpp"
#include "sdk/ui/text_button.hpp"

namespace micropixel::ui {

struct FlexContainerProperties final {
    Rect bounds{};
    FlexLayout layout{};
    bool visible{true};
};

class FlexContainer final {
   public:
    static constexpr uint32_t kDiagnosticBytes = 320U;

    FlexContainer() = default;
    FlexContainer(const FlexContainer&) = delete;
    FlexContainer& operator=(const FlexContainer&) = delete;
    FlexContainer(FlexContainer&&) noexcept = default;
    FlexContainer& operator=(FlexContainer&&) noexcept = default;

    [[nodiscard]] static FlexContainer CreateIn(Container& parent, FlexContainerProperties properties) {
        Assert(!properties.bounds.empty(), "flex container bounds invalid");
        FlexContainer result;
        result.node_ = parent.CreateContainer(
            {.translation = {properties.bounds.x, properties.bounds.y}, .visible = properties.visible});
        result.properties_ = properties;
        return result;
    }

    Label& CreateLabel(const char* text, LabelStyle style = {}) {
        labels_.push_back(std::make_unique<Label>(Label::CreateIn(node_, text, style)));
        AddAuto(ChildKind::kLabel, labels_.size() - 1U, false);
        return *labels_.back();
    }

    GridContainer& CreateGridContainer(GridContainerProperties properties) {
        if (properties.bounds.empty()) {
            properties.bounds = {0, 0, properties_.bounds.width, 1};
        }
        grids_.push_back(std::make_unique<GridContainer>(GridContainer::CreateIn(node_, properties)));
        AddAuto(ChildKind::kGrid, grids_.size() - 1U, false);
        return *grids_.back();
    }

    ImageButton& CreateImageButton(const Texture& texture, const ImageButtonProperties& properties) {
        image_buttons_.push_back(std::make_unique<ImageButton>(node_.CreateImageButton(texture, properties)));
        AddAuto(ChildKind::kImageButton, image_buttons_.size() - 1U, true);
        return *image_buttons_.back();
    }

    TextButton& CreateTextButton(const TextButtonProperties& properties) {
        text_buttons_.push_back(std::make_unique<TextButton>(node_.CreateTextButton(properties)));
        AddAuto(ChildKind::kTextButton, text_buttons_.size() - 1U, true);
        return *text_buttons_.back();
    }

    [[nodiscard]] Label& label(uint16_t index) {
        if (index >= labels_.size()) {
            PanicInvalidChild("Label", index, labels_.size());
        }
        return *labels_[index];
    }

    [[nodiscard]] GridContainer& grid(uint16_t index) {
        if (index >= grids_.size()) {
            PanicInvalidChild("GridContainer", index, grids_.size());
        }
        return *grids_[index];
    }

    [[nodiscard]] ImageButton& image_button(uint16_t index) {
        if (index >= image_buttons_.size()) {
            PanicInvalidChild("ImageButton", index, image_buttons_.size());
        }
        return *image_buttons_[index];
    }

    [[nodiscard]] TextButton& text_button(uint16_t index) {
        if (index >= text_buttons_.size()) {
            PanicInvalidChild("TextButton", index, text_buttons_.size());
        }
        return *text_buttons_[index];
    }

    [[nodiscard]] Result<void> Layout(SceneUpdate& update) {
        std::vector<FlexItem> items(children_.size());
        std::vector<Rect> rects(children_.size());
        const bool horizontal = properties_.layout.direction == FlexDirection::kHorizontal;
        for (size_t index = 0U; index < children_.size(); ++index) {
            const Size size = IntrinsicSize(children_[index]);
            const uint32_t main = horizontal ? size.width : size.height;
            const uint32_t cross = horizontal ? size.height : size.width;
            items[index] = {LayoutLength::Pixels(main),
                            children_[index].fit_cross_axis ? LayoutLength::Pixels(cross) : LayoutLength::Fill()};
        }
        auto laid_out = ComputeFlexLayout({0, 0, properties_.bounds.width, properties_.bounds.height},
                                          properties_.layout, items, rects);
        if (!laid_out.has_value()) {
            return unexpected(laid_out.error());
        }
        for (size_t index = 0U; index < children_.size(); ++index) {
            auto positioned = SetBounds(children_[index], update, rects[index]);
            if (!positioned.has_value()) {
                return unexpected(positioned.error());
            }
        }
        return {};
    }

    void SetVisible(SceneUpdate& update, bool visible) { node_.SetVisible(update, visible); }

    [[nodiscard]] FixedString<kDiagnosticBytes> ToString() const {
        FixedString<kDiagnosticBytes> description;
        description.Append("FlexContainer bounds=(x=");
        description.AppendInt(properties_.bounds.x);
        description.Append(",y=");
        description.AppendInt(properties_.bounds.y);
        description.Append(",w=");
        description.AppendInt(properties_.bounds.width);
        description.Append(",h=");
        description.AppendInt(properties_.bounds.height);
        description.Append(") direction=");
        description.Append(properties_.layout.direction == FlexDirection::kHorizontal ? "horizontal" : "vertical");
        description.Append(" children=");
        description.AppendUint(children_.size());
        description.Append(" labels=");
        description.AppendUint(labels_.size());
        description.Append(" grids=");
        description.AppendUint(grids_.size());
        description.Append(" image_buttons=");
        description.AppendUint(image_buttons_.size());
        description.Append(" text_buttons=");
        description.AppendUint(text_buttons_.size());
        return description;
    }

   private:
    enum class ChildKind : uint8_t { kLabel, kGrid, kImageButton, kTextButton };

    struct Child final {
        ChildKind kind{};
        size_t index{};
        bool fit_cross_axis{};
    };

    void AddAuto(ChildKind kind, size_t index, bool fit_cross_axis) {
        children_.push_back({kind, index, fit_cross_axis});
    }

    [[noreturn]] void PanicInvalidChild(const char* kind, size_t index, size_t count) const {
        FixedString<512U> diagnostic;
        diagnostic.Append("FlexContainer child index invalid: kind=");
        diagnostic.Append(kind);
        diagnostic.Append(" requested=");
        diagnostic.AppendUint(index);
        diagnostic.Append(" count=");
        diagnostic.AppendUint(count);
        diagnostic.Append(" state={");
        const auto state = ToString();
        diagnostic.Append(state.c_str());
        diagnostic.Append("}");
        Panic(diagnostic.c_str());
    }

    [[nodiscard]] Size IntrinsicSize(const Child& child) const {
        switch (child.kind) {
            case ChildKind::kLabel:
                return labels_[child.index]->intrinsic_size();
            case ChildKind::kGrid:
                return grids_[child.index]->intrinsic_size();
            case ChildKind::kImageButton:
                return image_buttons_[child.index]->intrinsic_size();
            case ChildKind::kTextButton:
                return text_buttons_[child.index]->intrinsic_size();
        }
        return {};
    }

    [[nodiscard]] Result<void> SetBounds(const Child& child, SceneUpdate& update, Rect bounds) {
        switch (child.kind) {
            case ChildKind::kLabel:
                return labels_[child.index]->SetBounds(update, bounds);
            case ChildKind::kGrid:
                return grids_[child.index]->SetBounds(update, bounds);
            case ChildKind::kImageButton:
                return image_buttons_[child.index]->SetBounds(update, bounds);
            case ChildKind::kTextButton:
                return text_buttons_[child.index]->SetBounds(update, bounds);
        }
        return unexpected(Error{ErrorCode::kInvalidState});
    }

    ContainerNode node_{};
    FlexContainerProperties properties_{};
    std::vector<Child> children_{};
    std::vector<std::unique_ptr<Label>> labels_{};
    std::vector<std::unique_ptr<GridContainer>> grids_{};
    std::vector<std::unique_ptr<ImageButton>> image_buttons_{};
    std::vector<std::unique_ptr<TextButton>> text_buttons_{};
};

}  // namespace micropixel::ui

namespace micropixel {

inline ui::FlexContainer Container::CreateFlexContainer(const ui::FlexContainerProperties& properties) {
    return ui::FlexContainer::CreateIn(*this, properties);
}

}  // namespace micropixel

#endif
