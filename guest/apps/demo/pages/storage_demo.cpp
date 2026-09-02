// App-private KV Storage usage for the navigable Demo app. See ../README.md.

#include "apps/demo/demo_page.hpp"

namespace demo {

namespace {

constexpr const char* kCounterKey = "demo.counter";

class StoragePage final {
   public:
    void Enter(DemoContext& context) {
        std::array<micropixel::Rect, 2U> bounds{};
        LayoutButtonRow(context, bounds);
        page_container_ = context.root_container.CreateContainer();
        CreateButtons(bounds);
        auto stored = context.app.storage().GetU32(kCounterKey);
        if (stored.has_value()) {
            counter_ = stored.value();
            status_ = "Loaded from the app-private KV namespace";
        } else if (stored.error().code() == micropixel::ErrorCode::kNotFound) {
            counter_ = 0U;
            status_ = "No saved value yet";
        } else {
            counter_ = 0U;
            status_ = "Storage read failed";
        }
        context.app.log().Info("demo.storage: counter loaded");
    }

    [[nodiscard]] bool OnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
        bool redraw = false;
        for (uint32_t index = 0U; index < 2U; ++index) {
            const micropixel::ui::ButtonUpdate update = buttons_[index].OnTouch(event);
            redraw = redraw || update.redraw();
            if (!update.clicked) {
                continue;
            }
            if (index == 0U) {
                ++counter_;
                auto saved = context.app.storage().SetU32(kCounterKey, counter_);
                status_ = saved.has_value() ? "Saved synchronously" : "Storage write failed";
                context.app.log().Info(saved.has_value() ? "demo.storage: counter saved"
                                                         : "demo.storage: counter save failed");
            } else {
                counter_ = 0U;
                auto saved = context.app.storage().SetU32(kCounterKey, counter_);
                status_ = saved.has_value() ? "Reset value saved" : "Storage reset failed";
                context.app.log().Info(saved.has_value() ? "demo.storage: counter reset"
                                                         : "demo.storage: counter reset failed");
            }
        }
        return redraw;
    }

    void Render(DemoContext& context, DemoView& commands) {
        const int32_t center_x = PageCenterX(context);
        commands.CenteredText(center_x, PageY(context, 16, 30), "The value survives app and device restarts.",
                              MutedColor(), micropixel::SystemFont::kMedium);
        const micropixel::Rect panel{context.layout.page_content.x + (context.layout.compact() ? 24 : 56),
                                     PageY(context, 66, 98),
                                     context.layout.page_content.width - (context.layout.compact() ? 48 : 112),
                                     context.layout.compact() ? 190 : 260};
        commands.Panel(panel, PanelColor());

        Line value;
        value.Append("COUNTER  ");
        value.AppendUint(counter_);
        commands.CenteredText(
            center_x, panel.y + (context.layout.compact() ? 48 : 80), value.c_str(), AccentColor(),
            context.layout.compact() ? micropixel::SystemFont::kLarge : micropixel::SystemFont::kTitle);
        commands.CenteredText(center_x, panel.y + (context.layout.compact() ? 112 : 172), status_,
                              micropixel::Color::White(), micropixel::SystemFont::kMedium);
        commands.CenteredText(center_x, panel.y + (context.layout.compact() ? 152 : 216), "KVStore::GetU32 / SetU32",
                              MutedColor(), micropixel::SystemFont::kSmall);

        for (auto& button : buttons_) {
            button.Sync(commands.scene_update());
        }
    }

    void Exit() { micropixel::Assert(page_container_.Destroy().has_value(), "demo.storage: destroy page failed"); }

   private:
    void CreateButtons(const std::array<micropixel::Rect, 2U>& bounds) {
        constexpr const char* labels[2]{"ADD + SAVE", "RESET"};
        const micropixel::Color backgrounds[2]{AccentColor(), DangerColor()};
        for (uint32_t index = 0U; index < 2U; ++index) {
            buttons_[index] = page_container_.CreateTextButton(
                {.bounds = bounds[index],
                 .text = labels[index],
                 .style = {.background = backgrounds[index], .font = micropixel::SystemFont::kLarge}});
        }
    }

    uint32_t counter_{};
    const char* status_{"Not loaded"};
    micropixel::ContainerNode page_container_{};
    micropixel::ui::TextButton buttons_[2]{};
};

StoragePage storage_page;

}  // namespace

void StorageDemoEnter(DemoContext& context) { storage_page.Enter(context); }

void StorageDemoExit(DemoContext&) { storage_page.Exit(); }

bool StorageDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return storage_page.OnTouch(context, event);
}

void StorageDemoRender(DemoContext& context, DemoView& commands) { storage_page.Render(context, commands); }

}  // namespace demo
