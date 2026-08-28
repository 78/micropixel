#include <stdint.h>

#include "apps/snake/snake.hpp"
#include "apps/snake/snake_game.hpp"
#include "sdk/micropixel.hpp"
#include "snake_assets.hpp"

namespace snake {

namespace {

uint32_t ReadU32OrDefault(micropixel::KVStore storage, const char* key, uint32_t fallback) {
    auto value = storage.GetU32(key);
    if (value.has_value()) {
        return value.value();
    }
    micropixel::Assert(value.error().code() == micropixel::ErrorCode::kNotFound, "snake: private KV read failed");
    return fallback;
}

micropixel::Texture LoadPackageTexture(micropixel::Application& app, micropixel::AssetId asset) {
    auto result = app.resources().LoadTexture(asset);
    micropixel::Assert(result.has_value(), "snake: critical texture resource failed");
    return static_cast<micropixel::Texture&&>(result.value());
}

constexpr micropixel::AssetId kFoodAssets[] = {
    snake_assets::food_normal,
    snake_assets::food_golden,
    snake_assets::food_poison,
    snake_assets::food_speed,
};

static_assert(snake_assets::burst_atlas_count == 4U && sizeof(kFoodAssets) / sizeof(kFoodAssets[0]) == 4U,
              "snake: generated sprite sheet bindings disagree with the app model");

}  // namespace

int SnakeAppMain() {
    micropixel::Application app;
    micropixel::Renderer renderer = app.renderer();
    micropixel::RendererInfo display = renderer.info();
    micropixel::Assert(display.width() == display.height() && display.width() >= 480U && display.width() <= 720U,
                       "snake: requires a 480-720 square display");

    micropixel::KVStore storage = app.storage();
    uint32_t best_score = ReadU32OrDefault(storage, "best", 0U);
    Line restored;
    restored.Append("snake: restored BEST ");
    restored.AppendUint(best_score);
    restored.Append(" 10 LEVELS 200-128 MS COMBO 4S EFFECT HOLD");
    app.log().Info(restored.c_str());

    app.log().Info("snake: launch page retained while ARGB sprite set predecodes");
    micropixel::Texture board = LoadPackageTexture(app, snake_assets::board);
    micropixel::Texture start_button = LoadPackageTexture(app, snake_assets::button_start);
    micropixel::Texture restart_button = LoadPackageTexture(app, snake_assets::button_restart);
    micropixel::Texture burst_sheets[4U]{};
    for (uint32_t type = 0U; type < 4U; ++type) {
        burst_sheets[type] = LoadPackageTexture(app, snake_assets::burst_atlases[type].asset);
    }
    micropixel::Texture food_sheets[4U]{};
    for (uint32_t type = 0U; type < 4U; ++type) {
        food_sheets[type] = LoadPackageTexture(app, kFoodAssets[type]);
    }
    app.log().Info("snake: board/buttons and eight sprite sheets decoded to persistent PSRAM");

    micropixel::Audio audio = app.audio();
    auto audio_info = audio.info();
    bool audio_available = audio_info.has_value();
    app.log().Info(audio_available ? "snake: Audio 1.1 ready; BGM and bounded SFX enabled"
                                   : "snake: Audio unavailable; visual gameplay continues");
    if (audio_available) {
        Line audio_format;
        audio_format.Append("snake: Host game-audio output ");
        audio_format.AppendUint(audio_info.value().sample_rate);
        audio_format.Append(" Hz");
        app.log().Info(audio_format.c_str());
    }

    SnakeGame game{app, renderer, display, audio, audio_available, best_score};
    game.SetBoard(static_cast<micropixel::Texture&&>(board));
    game.SetButtonTextures(static_cast<micropixel::Texture&&>(start_button),
                           static_cast<micropixel::Texture&&>(restart_button));
    for (uint32_t type = 0U; type < 4U; ++type) {
        game.SetBurstSheet(static_cast<FoodType>(type), static_cast<micropixel::Texture&&>(burst_sheets[type]));
    }
    for (uint32_t type = 0U; type < 4U; ++type) {
        game.SetFoodSheet(static_cast<FoodType>(type), static_cast<micropixel::Texture&&>(food_sheets[type]));
    }
    micropixel::Timer ticker = app.timers().Every(micropixel::Duration::Microseconds(kRenderTargetPeriodUs));
    game.Render();
    app.log().Info("snake: M18 ready; themes/interpolation/fixed effects/overlays");

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

}  // namespace snake
