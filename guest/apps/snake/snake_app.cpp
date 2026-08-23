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
    micropixel::AssertThat(value.error().code() == micropixel::ErrorCode::kNotFound, "snake: private KV read failed");
    return fallback;
}

micropixel::Bitmap LoadPackageBitmap(micropixel::Application& app, micropixel::AssetId asset) {
    micropixel::LoadRequest request = app.resources().Load(micropixel::ResourceRef::Package(asset));
    for (;;) {
        micropixel::Event event = app.WaitEvent();
        if (micropixel::ResourceReadyEvent* ready = event.ResourceFrom(request)) {
            micropixel::AssertThat(ready->succeeded(), "snake: critical sprite resource failed");
            return ready->TakeBitmap();
        }
    }
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
    micropixel::Graphics graphics = app.graphics();
    micropixel::GraphicsInfo display = graphics.info();
    micropixel::AssertThat(display.width() == 720U && display.height() == 720U, "snake: requires 720x720 display");

    micropixel::KVStore storage = app.storage();
    uint32_t best_score = ReadU32OrDefault(storage, "best", 0U);
    Line restored;
    restored.Append("snake: restored BEST ");
    restored.AppendUint(best_score);
    restored.Append(" 10 LEVELS 200-128 MS COMBO 4S EFFECT HOLD");
    app.log().Info(restored.c_str());

    app.log().Info("snake: launch page retained while ARGB sprite set predecodes");
    micropixel::Bitmap board = LoadPackageBitmap(app, snake_assets::board);
    micropixel::Bitmap start_button = LoadPackageBitmap(app, snake_assets::button_start);
    micropixel::Bitmap restart_button = LoadPackageBitmap(app, snake_assets::button_restart);
    micropixel::Bitmap burst_sheets[4U]{};
    for (uint32_t type = 0U; type < 4U; ++type) {
        burst_sheets[type] = LoadPackageBitmap(app, snake_assets::burst_atlases[type].asset);
    }
    micropixel::Bitmap food_sheets[4U]{};
    for (uint32_t type = 0U; type < 4U; ++type) {
        food_sheets[type] = LoadPackageBitmap(app, kFoodAssets[type]);
    }
    app.log().Info("snake: board/buttons and eight sprite sheets decoded to persistent PSRAM");

    micropixel::Audio audio = app.audio();
    auto audio_info = audio.info();
    bool audio_available = audio_info.has_value();
    app.log().Info(audio_available ? "snake: Audio 1.0 ready; BGM and bounded SFX enabled"
                                   : "snake: Audio unavailable; visual gameplay continues");
    if (audio_available) {
        Line audio_format;
        audio_format.Append("snake: Host game-audio output ");
        audio_format.AppendUint(audio_info.value().sample_rate);
        audio_format.Append(" Hz");
        app.log().Info(audio_format.c_str());
        Line audio_master;
        audio_master.Append("snake: app audio master set to ");
        audio_master.AppendUint(snake_sfx::kMasterPercent);
        audio_master.Append(" percent");
        app.log().Info(audio_master.c_str());
    }

    SnakeGame game{app, graphics, display, audio, audio_available, best_score};
    game.set_board(static_cast<micropixel::Bitmap&&>(board));
    game.set_button_bitmaps(static_cast<micropixel::Bitmap&&>(start_button),
                            static_cast<micropixel::Bitmap&&>(restart_button));
    for (uint32_t type = 0U; type < 4U; ++type) {
        game.set_burst_sheet(static_cast<FoodType>(type), static_cast<micropixel::Bitmap&&>(burst_sheets[type]));
    }
    for (uint32_t type = 0U; type < 4U; ++type) {
        game.set_food_sheet(static_cast<FoodType>(type), static_cast<micropixel::Bitmap&&>(food_sheets[type]));
    }
    micropixel::Timer ticker = app.timers().Every(micropixel::Duration::Microseconds(kRenderTargetPeriodUs));
    game.Render();
    app.log().Info("snake: M18 ready; themes/interpolation/fixed effects/overlays");

    for (;;) {
        micropixel::Event event = app.WaitEvent();
        if (const micropixel::TimerEvent* tick = event.TimerFrom(ticker)) {
            game.OnTimer(*tick);
        } else if (const micropixel::TouchEvent* touch = event.touch()) {
            game.OnTouch(*touch);
        } else if (event.type() == micropixel::EventType::kResume) {
            game.Render();
        } else if (event.type() == micropixel::EventType::kStop) {
            return 0;
        }
    }
}

}  // namespace snake
