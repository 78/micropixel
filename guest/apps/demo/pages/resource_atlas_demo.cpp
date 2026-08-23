// Package Resource loading and generated Atlas metadata usage. See ../README.md.

#include "apps/demo/demo_page.hpp"
#include "demo_assets.hpp"

namespace demo {

namespace {

static_assert(demo_assets::sprite_atlas_count == 1U, "demo: expected one sprite atlas");

class ResourceAtlasPage final {
   public:
    void Enter(DemoContext& context) {
        animation_running_ = context.atlas_bitmap.valid();
        for (uint32_t index = 0U; index < 2U; ++index) {
            buttons_[index].SetBounds(BottomButtonRect(context, index, 2U));
            buttons_[index].SetEnabled(context.atlas_bitmap.valid());
            buttons_[index].Reset();
        }
        context.app.log().Info(context.atlas_bitmap.valid() ? "demo.resource: atlas bitmap ready"
                                                            : "demo.resource: atlas bitmap unavailable");
    }

    [[nodiscard]] bool OnTimer(DemoContext&, const micropixel::TimerEvent& event) {
        if (!animation_running_) {
            return false;
        }
        accumulated_us_ += event.delta().count_microseconds();
        if (accumulated_us_ < 120000U) {
            return false;
        }
        accumulated_us_ %= 120000U;
        frame_ = (frame_ + 1U) % demo_assets::sprite_atlas_frame_count;
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
                frame_ = (frame_ + 1U) % demo_assets::sprite_atlas_frame_count;
            }
        }
        return redraw;
    }

    void Render(DemoContext& context, micropixel::CommandBuffer& commands) {
        const int32_t center_x = static_cast<int32_t>(context.display.width() / 2U);
        commands.DrawTextCentered(center_x, 118, "AssetId and frame rectangles come from the asset manifest.",
                                  MutedColor(), 17U);
        if (!context.atlas_bitmap.valid()) {
            commands.DrawTextCentered(center_x, 300, "Atlas bitmap failed to load", DangerColor(), 24U);
            return;
        }

        const demo_assets::Atlas& atlas = demo_assets::sprite_atlases[0];
        const demo_assets::AtlasFrame& frame = atlas.frames[frame_];
        const int32_t canvas_x = center_x - static_cast<int32_t>(demo_assets::sprite_canvas_width / 2U);
        const int32_t canvas_y = 190;
        commands.FillRect(micropixel::Rect{canvas_x, canvas_y, static_cast<int32_t>(demo_assets::sprite_canvas_width),
                                           static_cast<int32_t>(demo_assets::sprite_canvas_height)},
                          PanelColor());
        commands.DrawBitmapRegion(
            canvas_x + frame.canvas_x, canvas_y + frame.canvas_y, context.atlas_bitmap,
            micropixel::Rect{static_cast<int32_t>(frame.x), static_cast<int32_t>(frame.y),
                             static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height)});

        Line status;
        status.Append("Frame ");
        status.AppendUint(frame_ + 1U);
        status.Append(" / ");
        status.AppendUint(demo_assets::sprite_atlas_frame_count);
        status.Append("   PNG ");
        status.AppendUint(context.atlas_bitmap.width());
        status.Append("x");
        status.AppendUint(context.atlas_bitmap.height());
        commands.DrawTextCentered(center_x, 424, status.c_str(), micropixel::Color::White(), 18U);
        commands.DrawTextCentered(center_x, 468, animation_running_ ? "ANIMATING" : "PAUSED",
                                  animation_running_ ? AccentColor() : DangerColor(), 20U);

        DrawButton(commands, buttons_[0], animation_running_ ? "PAUSE" : "PLAY", AccentColor());
        DrawButton(commands, buttons_[1], "NEXT FRAME", BlueColor());
    }

   private:
    uint64_t accumulated_us_{};
    uint32_t frame_{};
    bool animation_running_{};
    micropixel::ui::Button buttons_[2]{};
};

ResourceAtlasPage resource_atlas_page;

}  // namespace

micropixel::Bitmap LoadDemoAtlas(micropixel::Application& app) {
    const demo_assets::Atlas& atlas = demo_assets::sprite_atlases[0];
    micropixel::LoadRequest request = app.resources().Load(micropixel::ResourceRef::Package(atlas.asset));
    for (;;) {
        micropixel::Event event = app.WaitEvent();
        if (micropixel::ResourceReadyEvent* ready = event.ResourceFrom(request)) {
            if (!ready->succeeded()) {
                return micropixel::Bitmap{};
            }
            micropixel::Bitmap bitmap = ready->TakeBitmap();
            micropixel::AssertThat(bitmap.width() == atlas.width && bitmap.height() == atlas.height,
                                   "demo.resource: atlas dimensions disagree with manifest");
            return bitmap;
        }
    }
}

void ResourceAtlasDemoEnter(DemoContext& context) { resource_atlas_page.Enter(context); }

bool ResourceAtlasDemoOnTimer(DemoContext& context, const micropixel::TimerEvent& event) {
    return resource_atlas_page.OnTimer(context, event);
}

bool ResourceAtlasDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return resource_atlas_page.OnTouch(context, event);
}

void ResourceAtlasDemoRender(DemoContext& context, micropixel::CommandBuffer& commands) {
    resource_atlas_page.Render(context, commands);
}

}  // namespace demo
