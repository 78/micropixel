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
    micropixel::Assert(value.error().code() == micropixel::ErrorCode::kNotFound, "blocks: private KV read failed");
    return 0U;
}

micropixel::Texture LoadPackageTexture(micropixel::Application& app, micropixel::AssetId asset) {
    auto result = app.resources().LoadTexture(asset);
    micropixel::Assert(result.has_value(), "blocks: critical texture resource failed");
    return static_cast<micropixel::Texture&&>(result.value());
}

}  // namespace

int BlocksAppMain() {
    micropixel::Application app;
    micropixel::Renderer renderer = app.renderer();
    micropixel::RendererInfo display = renderer.info();
    micropixel::Assert(display.width() == kScreenWidth && display.height() == kScreenHeight,
                       "blocks: requires 720x720 display");

    const uint32_t best_score = ReadBest(app.storage());
    Line restored;
    restored.Append("blocks: restored BEST ");
    restored.AppendUint(best_score);
    app.log().Info(restored.c_str());

    app.log().Info("blocks: launch retained while UI resources decode");
    micropixel::Texture board = LoadPackageTexture(app, blocks_assets::board);
    micropixel::Texture start = LoadPackageTexture(app, blocks_assets::button_start);
    micropixel::Texture restart = LoadPackageTexture(app, blocks_assets::button_restart);

    micropixel::Audio audio = app.audio();
    const auto audio_info = audio.info();
    const bool audio_available = audio_info.has_value();
    app.log().Info(audio_available ? "blocks: Audio ready; bounded SFX enabled"
                                   : "blocks: Audio unavailable; visual gameplay continues");

    BlocksGame game{app, renderer, display, audio, audio_available, best_score};
    game.set_textures(static_cast<micropixel::Texture&&>(board), static_cast<micropixel::Texture&&>(start),
                      static_cast<micropixel::Texture&&>(restart));
    const micropixel::Timer ticker = app.timers().Every(micropixel::Duration::Microseconds(kRenderTargetPeriodUs));
    game.Render();
    app.log().Info("blocks: ready; 4 offscreen playfield buffers with atomic dirty-cell commits");

    app.Run([&](const micropixel::Event& event) {
        if (const micropixel::TimerEvent* tick = event.TimerFrom(ticker)) {
            game.OnTimer(*tick);
        } else if (const micropixel::TouchEvent* touch = event.touch()) {
            game.OnTouch(*touch);
        } else if (event.type() == micropixel::EventType::kResume) {
            game.Render();
        }
    });
    return 0;
}

}  // namespace blocks
