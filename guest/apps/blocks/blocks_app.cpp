#include <stdint.h>

#include "apps/blocks/blocks.hpp"
#include "apps/blocks/blocks_game.hpp"
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

}  // namespace

int BlocksAppMain() {
    micropixel::Application app;
    micropixel::Renderer renderer = app.renderer();
    micropixel::RendererInfo display = renderer.info();
    micropixel::Assert(display.width() >= kScreenWidth && display.height() >= kScreenHeight,
                       "blocks: requires a logical 720x720 viewport");

    const uint32_t best_score = ReadBest(app.storage());
    Line restored;
    restored.Append("blocks: restored BEST ");
    restored.AppendUint(best_score);
    app.log().Info(restored.c_str());

    micropixel::Audio audio = app.audio();
    const auto audio_info = audio.info();
    const bool audio_available = audio_info.has_value();
    app.log().Info(audio_available ? "blocks: Audio ready; bounded SFX enabled"
                                   : "blocks: Audio unavailable; visual gameplay continues");

    BlocksGame game{app, renderer, display, audio, audio_available, best_score};
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
