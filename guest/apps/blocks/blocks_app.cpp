#include <stdint.h>

#include "apps/blocks/blocks.hpp"
#include "apps/blocks/blocks_game.hpp"
#include "blocks_assets.hpp"
#include "sdk/micropixel.hpp"

namespace blocks {

namespace {

uint32_t ReadBest(micropixel::KVStore storage) {
    auto value = storage.GetU32("best");
    if (value.has_value()) {
        return value.value();
    }
    micropixel::AssertThat(value.error().code() == micropixel::ErrorCode::kNotFound, "blocks: private KV read failed");
    return 0U;
}

micropixel::Bitmap LoadPackageBitmap(micropixel::Application& app, micropixel::AssetId asset) {
    micropixel::LoadRequest request = app.resources().Load(micropixel::ResourceRef::Package(asset));
    for (;;) {
        micropixel::Event event = app.WaitEvent();
        if (micropixel::ResourceReadyEvent* ready = event.ResourceFrom(request)) {
            micropixel::AssertThat(ready->succeeded(), "blocks: critical bitmap resource failed");
            return ready->TakeBitmap();
        }
    }
}

}  // namespace

int BlocksAppMain() {
    micropixel::Application app;
    micropixel::Graphics graphics = app.graphics();
    micropixel::GraphicsInfo display = graphics.info();
    micropixel::AssertThat(display.width() == kScreenWidth && display.height() == kScreenHeight,
                           "blocks: requires 720x720 display");

    const uint32_t best_score = ReadBest(app.storage());
    Line restored;
    restored.Append("blocks: restored BEST ");
    restored.AppendUint(best_score);
    app.log().Info(restored.c_str());

    app.log().Info("blocks: launch retained while UI resources decode");
    micropixel::Bitmap board = LoadPackageBitmap(app, blocks_assets::board);
    micropixel::Bitmap start = LoadPackageBitmap(app, blocks_assets::button_start);
    micropixel::Bitmap pause = LoadPackageBitmap(app, blocks_assets::button_pause);
    micropixel::Bitmap restart = LoadPackageBitmap(app, blocks_assets::button_restart);

    micropixel::Audio audio = app.audio();
    const auto audio_info = audio.info();
    const bool audio_available = audio_info.has_value();
    app.log().Info(audio_available ? "blocks: Audio ready; bounded SFX enabled"
                                   : "blocks: Audio unavailable; visual gameplay continues");

    BlocksGame game{app, graphics, display, audio, audio_available, best_score};
    game.set_bitmaps(static_cast<micropixel::Bitmap&&>(board), static_cast<micropixel::Bitmap&&>(start),
                     static_cast<micropixel::Bitmap&&>(pause), static_cast<micropixel::Bitmap&&>(restart));
    const micropixel::Timer ticker = app.timers().Every(micropixel::Duration::Microseconds(kRenderTargetPeriodUs));
    game.Render();
    app.log().Info("blocks: ready; 4 offscreen playfield buffers with atomic dirty-cell commits");

    for (;;) {
        micropixel::Event event = app.WaitEvent();
        if (const micropixel::TimerEvent* tick = event.TimerFrom(ticker)) {
            game.OnTimer(*tick);
        } else if (const micropixel::TouchEvent* touch = event.touch()) {
            game.OnTouch(*touch);
        }
    }
}

}  // namespace blocks
