// Package Resource loading and generated Atlas metadata usage. See ../README.md.

#include "apps/demo/demo_page.hpp"
#include "demo_assets.hpp"

namespace demo {

namespace {

static_assert(demo_assets::sprite_atlas_count == kDemoAtlasSheetCount, "demo: unexpected sprite atlas count");
static_assert(demo_assets::sprite_atlas_frame_count == 10U, "demo: unexpected frames per sprite atlas");
constexpr uint64_t kAtlasFramePeriodUs = 16667U;
constexpr uint32_t kAtlasTotalFrameCount = demo_assets::sprite_atlas_count * demo_assets::sprite_atlas_frame_count;

[[nodiscard]] bool AtlasTexturesValid(const DemoContext& context) {
    for (const micropixel::Texture& texture : context.atlas_textures) {
        if (!texture.valid()) {
            return false;
        }
    }
    return true;
}

class ResourceAtlasPage final {
   public:
    void Enter(DemoContext& context) {
        animation_running_ = AtlasTexturesValid(context);
        page_container_ = context.root_container.CreateContainer();
        CreateButtons(context);
        context.app.log().Info(animation_running_ ? "demo.resource: atlas textures ready"
                                                  : "demo.resource: atlas textures unavailable");
    }

    void Exit() { micropixel::Assert(page_container_.Destroy().has_value(), "demo.resource: destroy page failed"); }

    [[nodiscard]] bool OnTimer(DemoContext&, const micropixel::TimerEvent& event) {
        if (!animation_running_) {
            return false;
        }
        accumulated_us_ += event.delta().count_microseconds();
        const uint64_t elapsed_frames = accumulated_us_ / kAtlasFramePeriodUs;
        if (elapsed_frames == 0U) {
            return false;
        }
        accumulated_us_ %= kAtlasFramePeriodUs;
        frame_ = static_cast<uint32_t>((frame_ + elapsed_frames) % kAtlasTotalFrameCount);
        return true;
    }

    [[nodiscard]] bool OnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
        (void)context;
        bool redraw = false;
        for (uint32_t index = 0U; index < 2U; ++index) {
            const micropixel::ui::ButtonUpdate update = buttons_[index].OnTouch(event);
            redraw = redraw || update.redraw();
            if (!update.clicked) {
                continue;
            }
            if (index == 0U) {
                animation_running_ = !animation_running_;
            } else {
                animation_running_ = false;
                frame_ = (frame_ + 1U) % kAtlasTotalFrameCount;
            }
        }
        return redraw;
    }

    void Render(DemoContext& context, DemoView& commands) {
        const int32_t center_x = PageCenterX(context);
        commands.CenteredText(center_x, PageY(context, 8, 16), "30-frame RGBA atlas, native pixels and alpha blend.",
                              MutedColor(), micropixel::SystemFont::kMedium);
        const bool textures_valid = AtlasTexturesValid(context);
        for (micropixel::ui::TextButton& button : buttons_) {
            button.SetEnabled(commands.scene_update(), textures_valid);
            button.SetVisible(commands.scene_update(), textures_valid);
        }
        if (!textures_valid) {
            commands.CenteredText(center_x, PageY(context, 130, 220), "Atlas texture failed to load", DangerColor(),
                                  micropixel::SystemFont::kLarge);
            return;
        }

        const uint32_t atlas_index = frame_ / demo_assets::sprite_atlas_frame_count;
        const uint32_t atlas_frame = frame_ % demo_assets::sprite_atlas_frame_count;
        const demo_assets::Atlas& atlas = demo_assets::sprite_atlases[atlas_index];
        const demo_assets::AtlasFrame& frame = atlas.frames[atlas_frame];
        constexpr int32_t kCanvasWidth = static_cast<int32_t>(demo_assets::sprite_canvas_width);
        constexpr int32_t kCanvasHeight = static_cast<int32_t>(demo_assets::sprite_canvas_height);
        const int32_t animation_center_y = PageY(context, 128, 180);
        const micropixel::Point canvas_origin{center_x - kCanvasWidth / 2, animation_center_y - kCanvasHeight / 2};
        const micropixel::Point sprite_position{canvas_origin.x + frame.canvas_x, canvas_origin.y + frame.canvas_y};
        commands.Sprite(sprite_position, context.atlas_textures[atlas_index],
                        micropixel::Rect{static_cast<int32_t>(frame.x), static_cast<int32_t>(frame.y),
                                         static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)},
                        224U);

        Line status;
        status.Append("Atlas frame ");
        status.AppendUint(frame_ + 1U);
        status.Append(" / ");
        status.AppendUint(kAtlasTotalFrameCount);
        status.Append("   Sheet ");
        status.AppendUint(atlas_index + 1U);
        status.Append(" / ");
        status.AppendUint(demo_assets::sprite_atlas_count);
        const int32_t status_y = animation_center_y + (context.layout.compact() ? 108 : 144);
        commands.CenteredText(center_x, status_y, status.c_str(), micropixel::Color::White(),
                              micropixel::SystemFont::kMedium);
        commands.CenteredText(center_x, status_y + (context.layout.compact() ? 32 : 44),
                              animation_running_ ? "ANIMATING / ALPHA 224" : "PAUSED / ALPHA 224",
                              animation_running_ ? AccentColor() : DangerColor(), micropixel::SystemFont::kLarge);

        auto text = buttons_[0].SetText(commands.scene_update(), animation_running_ ? "PAUSE" : "PLAY");
        micropixel::Assert(text.has_value(), "demo.resource: update text button failed");
    }

   private:
    void CreateButtons(DemoContext& context) {
        std::array<micropixel::Rect, 2U> bounds{};
        LayoutButtonRow(context, bounds);
        constexpr const char* labels[2]{"PLAY", "NEXT FRAME"};
        const micropixel::Color backgrounds[2]{AccentColor(), BlueColor()};
        for (uint32_t index = 0U; index < 2U; ++index) {
            buttons_[index] = page_container_.CreateTextButton(
                {.bounds = bounds[index],
                 .text = labels[index],
                 .style = {.background = backgrounds[index], .font = micropixel::SystemFont::kLarge}});
        }
    }

    uint64_t accumulated_us_{};
    uint32_t frame_{};
    bool animation_running_{};
    micropixel::ContainerNode page_container_{};
    micropixel::ui::TextButton buttons_[2]{};
};

ResourceAtlasPage resource_atlas_page;

}  // namespace

micropixel::Timer CreateResourceAtlasTicker(micropixel::Application& app) {
    return app.timers().Every(micropixel::Duration::Microseconds(kAtlasFramePeriodUs));
}

DemoAtlasTextures LoadDemoAtlases(micropixel::Application& app) {
    DemoAtlasTextures textures{};
    for (uint32_t index = 0U; index < kDemoAtlasSheetCount; ++index) {
        const demo_assets::Atlas& atlas = demo_assets::sprite_atlases[index];
        auto result = app.resources().LoadTexture(atlas.asset);
        if (!result.has_value()) {
            return DemoAtlasTextures{};
        }
        textures[index] = static_cast<micropixel::Texture&&>(result.value());
        micropixel::Assert(textures[index].width() == atlas.width && textures[index].height() == atlas.height,
                           "demo.resource: atlas dimensions disagree with manifest");
    }
    return textures;
}

void ResourceAtlasDemoEnter(DemoContext& context) { resource_atlas_page.Enter(context); }

void ResourceAtlasDemoExit(DemoContext&) { resource_atlas_page.Exit(); }

bool ResourceAtlasDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event) {
    return resource_atlas_page.OnTimer(context, event);
}

bool ResourceAtlasDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return resource_atlas_page.OnTouch(context, event);
}

void ResourceAtlasDemoRender(DemoContext& context, DemoView& commands) {
    resource_atlas_page.Render(context, commands);
}

}  // namespace demo
