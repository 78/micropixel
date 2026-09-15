// SPDX-License-Identifier: Apache-2.0
#include "chinese-demo_strings.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/scene.hpp"

using namespace micropixel;
using namespace micropixel::literals;

int main() {
    Application app;
    const auto locale = app.localization().CurrentLocale();
    const auto strings = chinese_demo_strings::ForLocale(locale);
    using Id = chinese_demo_strings::Id;
    auto created = app.renderer().CreateScene(Color::Rgb(13, 20, 29));
    Assert(created.has_value(), "CreateScene failed");
    auto scene = std::move(*created);
    const auto label = [&](int y, Id id, SystemFont font) {
        Assert(scene.CreateLabel({30, y}, strings.Get(id), Color::Rgb(218, 229, 239), font).has_value(),
               "CreateLabel failed");
    };
    label(48, Id::kGreeting, SystemFont::kTitle);
    label(122, Id::kAppTitle, SystemFont::kLarge);
    label(196, Id::kSampleSmall, SystemFont::kSmall);
    label(254, Id::kSampleMedium, SystemFont::kMedium);
    label(320, Id::kSampleLarge, SystemFont::kLarge);
    label(406, Id::kSharedFont, SystemFont::kSmall);
    label(462, Id::kSwitchHint, SystemFont::kSmall);
    auto value = scene.CreateLabel({30, 538}, "", Color::Rgb(95, 226, 177), SystemFont::kLarge);
    Assert(value.has_value(), "Create counter failed");
    FixedString<96> language;
    language.Append(strings.Get(Id::kLocalePrefix));
    language.Append(locale.tag());
    Assert(scene.CreateLabel({30, 634}, language.c_str(), Color::Rgb(218, 229, 239), SystemFont::kSmall).has_value(),
           "Create locale label failed");
    const auto update_counter = [&](uint32_t seconds) {
        FixedString<96> text;
        text.Append(strings.Get(Id::kCounterPrefix));
        text.AppendUint(seconds);
        text.Append(strings.Get(Id::kCounterSuffix));
        value->SetText(text.c_str());
        Assert(app.renderer().Present(scene).has_value(), "Present failed");
    };
    update_counter(0);
    app.log().Info("MULTILINGUAL_DEMO started; shared system fonts");
    app.log().Info(locale.tag());
    const auto started = app.clock().Now();
    auto timer = app.timers().Every(1_s);
    Assert(timer.has_value(), "Create timer failed");
    app.Run([&](const Event& event) {
        if (event.TimerFrom(*timer) != nullptr) {
            const auto elapsed = app.clock().Now() - started;
            update_counter(static_cast<uint32_t>(elapsed.count_microseconds() / 1000000U));
        }
    });
    return 0;
}
