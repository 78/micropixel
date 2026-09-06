#ifndef MICROPIXEL_DEMO_PAGE_HPP
#define MICROPIXEL_DEMO_PAGE_HPP

#include <stdint.h>

#include <array>
#include <span>
#include <vector>

#include "sdk/micropixel.hpp"

namespace demo {

inline constexpr micropixel::Color BackgroundColor() { return micropixel::Color::Rgb(7U, 14U, 27U); }
inline constexpr micropixel::Color PanelColor() { return micropixel::Color::Rgb(17U, 32U, 52U); }
inline constexpr micropixel::Color AccentColor() { return micropixel::Color::Rgb(67U, 214U, 166U); }
inline constexpr micropixel::Color MutedColor() { return micropixel::Color::Rgb(148U, 168U, 190U); }
inline constexpr micropixel::Color DangerColor() { return micropixel::Color::Rgb(224U, 79U, 96U); }
inline constexpr micropixel::Color BlueColor() { return micropixel::Color::Rgb(68U, 132U, 235U); }
enum class DisplayClass : uint8_t {
    kCompact,
    kExpanded,
};

struct DemoLayout final {
    DisplayClass display_class{};
    micropixel::Rect screen{};
    micropixel::Rect home_header{};
    micropixel::Rect home_content{};
    micropixel::Rect page_header{};
    micropixel::Rect page_content{};
    micropixel::Rect page_actions{};
    micropixel::Rect back_button{};
    micropixel::Rect separator{};
    std::array<micropixel::Rect, 6U> menu_buttons{};

    [[nodiscard]] constexpr bool compact() const { return display_class == DisplayClass::kCompact; }
};

[[nodiscard]] DemoLayout BuildDemoLayout(micropixel::RendererInfo display);

inline constexpr uint32_t kDemoAtlasSheetCount = 3U;
using DemoAtlasTextures = std::array<micropixel::Texture, kDemoAtlasSheetCount>;

class DemoView final {
   public:
    DemoView(micropixel::Scene& scene, micropixel::ContainerNode root_container)
        : scene_(scene),
          shape_container_(root_container.CreateContainer({.z_order = 0})),
          sprite_container_(root_container.CreateContainer({.z_order = 1})),
          label_container_(root_container.CreateContainer({.z_order = 2})) {}

    template <typename RenderFunction>
    void Update(RenderFunction&& render) {
        auto presented = scene_.Update([&](micropixel::SceneUpdate& update) {
            update_ = &update;
            shape_count_ = 0U;
            label_count_ = 0U;
            sprite_count_ = 0U;
            for (micropixel::ShapeNode& shape : shapes_) {
                shape.SetVisible(update, false);
            }
            for (micropixel::LabelNode& label : labels_) {
                label.SetVisible(update, false);
            }
            for (micropixel::SpriteNode& sprite : sprites_) {
                sprite.SetVisible(update, false);
            }
            render(*this);
            DestroyUnused(update, shapes_, shape_count_);
            DestroyUnused(update, labels_, label_count_);
            DestroyUnused(update, sprites_, sprite_count_);
            update_ = nullptr;
        });
        micropixel::Assert(presented.has_value(), "demo: scene update failed");
        shapes_.resize(shape_count_);
        labels_.resize(label_count_);
        sprites_.resize(sprite_count_);
    }

    void Panel(micropixel::Rect rect, micropixel::Color color, uint8_t opacity = 255U) {
        micropixel::Assert(update_ != nullptr, "demo: no active scene update");
        if (shape_count_ == shapes_.size()) {
            shapes_.push_back(shape_container_.CreateShape(rect, color, opacity));
        }
        micropixel::ShapeNode& shape = shapes_[shape_count_++];
        shape.SetRect(*update_, rect);
        shape.SetColor(*update_, color);
        shape.SetOpacity(*update_, opacity);
        shape.SetVisible(*update_, opacity != 0U);
    }

    void Text(micropixel::Point position, const char* text, micropixel::Color color,
              micropixel::SystemFont font = micropixel::SystemFont::kMedium) {
        SetText(position, text, color, font, false);
    }

    void CenteredText(int32_t center_x, int32_t y, const char* text, micropixel::Color color,
                      micropixel::SystemFont font = micropixel::SystemFont::kMedium) {
        SetText({center_x, y}, text, color, font, true);
    }

    void Sprite(micropixel::Point position, const micropixel::Texture& texture, micropixel::Rect source,
                uint8_t opacity = 255U) {
        micropixel::Assert(update_ != nullptr, "demo: no active scene update");
        if (sprite_count_ == sprites_.size()) {
            sprites_.push_back(sprite_container_.CreateSprite(
                texture, {position.x, position.y, source.width, source.height}, source, opacity));
        }
        micropixel::SpriteNode& sprite = sprites_[sprite_count_++];
        sprite.SetTexture(*update_, texture);
        sprite.SetSource(*update_, source);
        sprite.SetDestination(*update_, {position.x, position.y, source.width, source.height});
        sprite.SetOpacity(*update_, opacity);
        sprite.SetVisible(*update_, opacity != 0U);
    }

    [[nodiscard]] micropixel::SceneUpdate& scene_update() {
        micropixel::Assert(update_ != nullptr, "demo: no active scene update");
        return *update_;
    }

   private:
    template <typename Node>
    static void DestroyUnused(micropixel::SceneUpdate& update, std::vector<Node>& nodes, uint32_t used) {
        for (uint32_t index = used; index < nodes.size(); ++index) {
            nodes[index].Destroy(update);
        }
    }

    void SetText(micropixel::Point position, const char* text, micropixel::Color color, micropixel::SystemFont font,
                 bool centered) {
        micropixel::Assert(update_ != nullptr, "demo: no active scene update");
        if (label_count_ == labels_.size()) {
            labels_.push_back(label_container_.CreateLabel(position, text, color, font, centered));
        }
        micropixel::LabelNode& label = labels_[label_count_++];
        label.SetPosition(*update_, position);
        label.SetText(*update_, text);
        label.SetColor(*update_, color);
        label.SetFont(*update_, font);
        label.SetCentered(*update_, centered);
        label.SetVisible(*update_, true);
    }

    micropixel::Scene& scene_;
    micropixel::ContainerNode shape_container_{};
    micropixel::ContainerNode sprite_container_{};
    micropixel::ContainerNode label_container_{};
    std::vector<micropixel::ShapeNode> shapes_{};
    std::vector<micropixel::LabelNode> labels_{};
    std::vector<micropixel::SpriteNode> sprites_{};
    micropixel::SceneUpdate* update_{};
    uint32_t shape_count_{};
    uint32_t label_count_{};
    uint32_t sprite_count_{};
};

struct DemoContext final {
    micropixel::Application& app;
    micropixel::InputInfo input;
    DemoLayout layout;
    DemoAtlasTextures& atlas_textures;
    micropixel::Scene& scene;
    micropixel::ContainerNode root_container;
    DemoView& view;
    micropixel::AudioClip audio_clip{};
    micropixel::Playback audio_playback{};
};

using Line = micropixel::FixedString<96U>;

[[nodiscard]] inline constexpr int32_t ContentWidth(const DemoContext& context) { return context.layout.screen.width; }

[[nodiscard]] inline constexpr int32_t ContentHeight(const DemoContext& context) {
    return context.layout.screen.height;
}

[[nodiscard]] inline constexpr int32_t PageCenterX(const DemoContext& context) {
    return context.layout.page_content.center_x();
}

[[nodiscard]] inline constexpr int32_t PageY(const DemoContext& context, int32_t compact_offset,
                                             int32_t expanded_offset) {
    return context.layout.page_content.y + (context.layout.compact() ? compact_offset : expanded_offset);
}

inline void LayoutButtonRow(const DemoContext& context, std::span<micropixel::Rect> bounds) {
    micropixel::Assert(!bounds.empty() && bounds.size() <= 6U, "demo: invalid action button count");
    std::array<micropixel::ui::FlexItem, 6U> items{};
    for (uint32_t index = 0U; index < bounds.size(); ++index) {
        items[index] = micropixel::ui::FlexItem::Grow();
    }
    const int32_t horizontal_padding = context.layout.compact() ? 20 : 28;
    const int32_t vertical_padding = context.layout.compact() ? 12 : 16;
    auto result = micropixel::ui::ComputeFlexLayout(
        context.layout.page_actions,
        micropixel::ui::FlexLayout{
            .direction = micropixel::ui::FlexDirection::kHorizontal,
            .padding = {vertical_padding, horizontal_padding, vertical_padding, horizontal_padding},
            .gap_pixels = context.layout.compact() ? 10 : 14},
        std::span<const micropixel::ui::FlexItem>{items.data(), bounds.size()}, bounds);
    micropixel::Assert(result.has_value(), "demo: action button layout failed");
}

enum class PageId : uint8_t {
    kHome,
    kTimer,
    kInput,
    kStorage,
    kResourceAtlas,
    kAudio,
    kDevices,
};

[[nodiscard]] micropixel::Timer CreateDemoTicker(micropixel::Application& app);
[[nodiscard]] micropixel::Timer CreateResourceAtlasTicker(micropixel::Application& app);
[[nodiscard]] micropixel::Timer CreateDeviceTicker(micropixel::Application& app);
[[nodiscard]] DemoAtlasTextures LoadDemoAtlases(micropixel::Application& app);

void TimerDemoEnter(DemoContext& context);
void TimerDemoExit(DemoContext& context);
[[nodiscard]] bool TimerDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool TimerDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void TimerDemoRender(DemoContext& context, DemoView& view);

void InputDemoEnter(DemoContext& context);
[[nodiscard]] bool InputDemoOnKey(DemoContext& context, const micropixel::KeyEvent& event);
[[nodiscard]] bool InputDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void InputDemoRender(DemoContext& context, DemoView& view);

void StorageDemoEnter(DemoContext& context);
void StorageDemoExit(DemoContext& context);
[[nodiscard]] bool StorageDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void StorageDemoRender(DemoContext& context, DemoView& view);

void ResourceAtlasDemoEnter(DemoContext& context);
void ResourceAtlasDemoExit(DemoContext& context);
[[nodiscard]] bool ResourceAtlasDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool ResourceAtlasDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void ResourceAtlasDemoRender(DemoContext& context, DemoView& view);

void AudioDemoEnter(DemoContext& context);
void AudioDemoExit(DemoContext& context);
[[nodiscard]] bool AudioDemoOnEvent(DemoContext& context, const micropixel::Event& event);
[[nodiscard]] bool AudioDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void AudioDemoRender(DemoContext& context, DemoView& view);

void DeviceDemoEnter(DemoContext& context);
void DeviceDemoExit(DemoContext& context);
[[nodiscard]] bool DeviceDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event);
[[nodiscard]] bool DeviceDemoOnEvent(DemoContext& context, const micropixel::Event& event);
[[nodiscard]] bool DeviceDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event);
void DeviceDemoRender(DemoContext& context, DemoView& view);

}  // namespace demo

#endif
