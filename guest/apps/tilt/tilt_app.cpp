#include "apps/tilt/tilt.hpp"
#include "apps/tilt/tilt_game.hpp"
#include "apps/tilt/tilt_progress_keys.hpp"
#include "sdk/micropixel.hpp"
#include "tilt_assets.hpp"

namespace tilt {
namespace {

struct LoadedArt final {
    micropixel::Texture board_base{};
    micropixel::Texture board_frame{};
    micropixel::Texture board_tiles{};
    micropixel::Texture objects{};
    micropixel::Texture fans{};
    micropixel::Texture mechanics{};
    micropixel::Texture title{};
    micropixel::Texture hud_icons{};
    uint32_t object_frame_pixels{};
    uint32_t board_tile_frame_pixels{};
    uint32_t fan_frame_pixels{};
    uint32_t mechanic_frame_pixels{};
    uint32_t hud_frame_pixels{};
};

static_assert(kLegacyLevelCount <= kLevelCount);

micropixel::Texture RequireTexture(micropixel::Result<micropixel::Texture> result, const char* reason) {
    micropixel::Assert(result.has_value(), reason);
    return static_cast<micropixel::Texture&&>(result.value());
}

LoadedArt LoadArt(micropixel::Application& app, const micropixel::RendererInfo& display) {
    micropixel::Resources resources = app.resources();
    LoadedArt art{};
    const bool native_480 = display.physical_width() == 480U && display.physical_height() == 480U;
    const bool native_720 = display.physical_width() == 720U && display.physical_height() == 720U;
    if (native_480) {
        art.board_base = RequireTexture(resources.LoadNativeTexture(tilt_assets::board_base_p480),
                                        "tilt: native 480 board base failed");
        art.board_frame = RequireTexture(resources.LoadNativeTexture(tilt_assets::board_frame_p480),
                                         "tilt: native 480 board frame failed");
        art.board_tiles = RequireTexture(resources.LoadNativeTexture(tilt_assets::board_tiles_p480),
                                         "tilt: native 480 board tile atlas failed");
        art.objects = RequireTexture(resources.LoadNativeTexture(tilt_assets::objects_p480),
                                     "tilt: native 480 object texture failed");
        art.fans =
            RequireTexture(resources.LoadNativeTexture(tilt_assets::fans_p480), "tilt: native 480 fan texture failed");
        art.mechanics = RequireTexture(resources.LoadNativeTexture(tilt_assets::mechanics_p480),
                                       "tilt: native 480 mechanic texture failed");
        art.title = RequireTexture(resources.LoadNativeTexture(tilt_assets::title_p480),
                                   "tilt: native 480 title texture failed");
        art.hud_icons = RequireTexture(resources.LoadNativeTexture(tilt_assets::hud_icons_p480),
                                       "tilt: native 480 HUD texture failed");
        art.object_frame_pixels = 64U;
        art.board_tile_frame_pixels = 64U;
        art.fan_frame_pixels = 64U;
        art.mechanic_frame_pixels = 64U;
        art.hud_frame_pixels = 40U;
        return art;
    }
    if (native_720) {
        art.board_base = RequireTexture(resources.LoadNativeTexture(tilt_assets::board_base_p720),
                                        "tilt: native 720 board base failed");
        art.board_frame = RequireTexture(resources.LoadNativeTexture(tilt_assets::board_frame_p720),
                                         "tilt: native 720 board frame failed");
        art.board_tiles = RequireTexture(resources.LoadNativeTexture(tilt_assets::board_tiles_p720),
                                         "tilt: native 720 board tile atlas failed");
        art.objects = RequireTexture(resources.LoadNativeTexture(tilt_assets::objects_p720),
                                     "tilt: native 720 object texture failed");
        art.fans =
            RequireTexture(resources.LoadNativeTexture(tilt_assets::fans_p720), "tilt: native 720 fan texture failed");
        art.mechanics = RequireTexture(resources.LoadNativeTexture(tilt_assets::mechanics_p720),
                                       "tilt: native 720 mechanic texture failed");
        art.title = RequireTexture(resources.LoadNativeTexture(tilt_assets::title_p720),
                                   "tilt: native 720 title texture failed");
        art.hud_icons = RequireTexture(resources.LoadNativeTexture(tilt_assets::hud_icons_p720),
                                       "tilt: native 720 HUD texture failed");
        art.object_frame_pixels = 96U;
        art.board_tile_frame_pixels = 96U;
        art.fan_frame_pixels = 96U;
        art.mechanic_frame_pixels = 96U;
        art.hud_frame_pixels = 60U;
        return art;
    }
    app.log().Info("tilt: no native art profile; adapting 720 assets");
    art.board_base =
        RequireTexture(resources.LoadTexture(tilt_assets::board_base_p720), "tilt: adaptive board base failed");
    art.board_frame =
        RequireTexture(resources.LoadTexture(tilt_assets::board_frame_p720), "tilt: adaptive board frame failed");
    art.board_tiles =
        RequireTexture(resources.LoadTexture(tilt_assets::board_tiles_p720), "tilt: adaptive board tile atlas failed");
    art.objects =
        RequireTexture(resources.LoadTexture(tilt_assets::objects_p720), "tilt: adaptive object texture failed");
    art.fans = RequireTexture(resources.LoadTexture(tilt_assets::fans_p720), "tilt: adaptive fan texture failed");
    art.mechanics =
        RequireTexture(resources.LoadTexture(tilt_assets::mechanics_p720), "tilt: adaptive mechanic texture failed");
    art.title = RequireTexture(resources.LoadTexture(tilt_assets::title_p720), "tilt: adaptive title texture failed");
    art.hud_icons =
        RequireTexture(resources.LoadTexture(tilt_assets::hud_icons_p720), "tilt: adaptive HUD texture failed");
    art.object_frame_pixels = 96U;
    art.board_tile_frame_pixels = 96U;
    art.fan_frame_pixels = 96U;
    art.mechanic_frame_pixels = 96U;
    art.hud_frame_pixels = 60U;
    return art;
}

uint32_t ReadStoredU32(micropixel::KVStore storage, const char* key) {
    auto value = storage.GetU32(key);
    if (value.has_value()) {
        return value.value();
    }
    micropixel::Assert(value.error().code() == micropixel::ErrorCode::kNotFound, "tilt: best time read failed");
    return 0U;
}

ProgressData ReadProgress(micropixel::KVStore storage) {
    ProgressData progress{};
    auto stored_size = storage.GetBytesSize("progress_v2");
    if (stored_size.has_value()) {
        if (stored_size.value() == sizeof(progress)) {
            auto loaded = storage.GetBytes("progress_v2", reinterpret_cast<uint8_t*>(&progress), sizeof(progress));
            micropixel::Assert(loaded.has_value() && loaded.value() == sizeof(progress),
                               "tilt: progress blob read failed");
            if (progress.schema_version == kProgressSchemaVersion && progress.level_count == kLevelCount &&
                progress.unlocked_level_index < kLevelCount) {
                for (uint32_t index = 0U; index < kLevelCount; ++index) {
                    if (progress.best_ratings[index] > 3U) {
                        progress.best_ratings[index] = 3U;
                    }
                }
                return progress;
            }
        }
    } else {
        micropixel::Assert(stored_size.error().code() == micropixel::ErrorCode::kNotFound,
                           "tilt: progress blob size failed");
    }

    progress = {};
    for (uint32_t index = 0U; index < kLegacyLevelCount; ++index) {
        progress.best_times_ms[index] = ReadStoredU32(storage, kLegacyBestTimeKeys[index]);
        const uint32_t rating = ReadStoredU32(storage, kLegacyBestRatingKeys[index]);
        progress.best_ratings[index] = static_cast<uint8_t>(rating > 3U ? 3U : rating);
    }
    if (progress.best_times_ms[0] == 0U) {
        progress.best_times_ms[0] = ReadStoredU32(storage, "best_ms");
    }
    progress.unlocked_level_index = ReadStoredU32(storage, "unlocked_level");
    if (progress.unlocked_level_index >= kLevelCount) {
        progress.unlocked_level_index = kLevelCount - 1U;
    }
    // The five-level release stored level 05 as the final selection. Infer the
    // next unlock from completed best times so existing players enter level 06.
    for (uint32_t index = 0U; index + 1U < kLevelCount; ++index) {
        if (progress.best_times_ms[index] != 0U && progress.unlocked_level_index <= index) {
            progress.unlocked_level_index = index + 1U;
        }
    }
    return progress;
}

}  // namespace

int TiltAppMain() {
    micropixel::Application app;
    micropixel::Renderer renderer = app.renderer();
    micropixel::RendererInfo display = renderer.info();
    micropixel::Assert(display.width() >= kScreenWidth && display.height() >= kScreenHeight,
                       "tilt: requires a logical 720x720 viewport");
    LoadedArt art = LoadArt(app, display);
    micropixel::Assert(
        art.board_base.width() != 0U && art.board_frame.width() != 0U &&
            art.board_tiles.width() == art.board_tile_frame_pixels * kBoardTileSheetColumns &&
            art.board_tiles.height() ==
                art.board_tile_frame_pixels *
                    ((kBoardTileFrameCount + kBoardTileSheetColumns - 1U) / kBoardTileSheetColumns) &&
            art.objects.width() == art.object_frame_pixels * kObjectSheetColumns &&
            art.objects.height() == art.object_frame_pixels * kObjectSheetColumns &&
            art.fans.width() == art.fan_frame_pixels * kFanFrameCount && art.fans.height() == art.fan_frame_pixels &&
            art.mechanics.width() == art.mechanic_frame_pixels * kMechanicSheetColumns &&
            art.mechanics.height() == art.mechanic_frame_pixels * 2U && art.title.width() != 0U &&
            art.hud_icons.width() == art.hud_frame_pixels * 3U && art.hud_icons.height() == art.hud_frame_pixels,
        "tilt: selected art dimensions invalid");

    micropixel::Audio audio = app.audio();
    const bool audio_available = audio.info().has_value();
    const ProgressData progress = ReadProgress(app.storage());
    TiltGame game{app,
                  renderer,
                  display,
                  static_cast<micropixel::Texture&&>(art.board_base),
                  static_cast<micropixel::Texture&&>(art.board_frame),
                  static_cast<micropixel::Texture&&>(art.board_tiles),
                  art.board_tile_frame_pixels,
                  static_cast<micropixel::Texture&&>(art.objects),
                  art.object_frame_pixels,
                  static_cast<micropixel::Texture&&>(art.fans),
                  art.fan_frame_pixels,
                  static_cast<micropixel::Texture&&>(art.mechanics),
                  art.mechanic_frame_pixels,
                  static_cast<micropixel::Texture&&>(art.title),
                  static_cast<micropixel::Texture&&>(art.hud_icons),
                  art.hud_frame_pixels,
                  audio,
                  audio_available,
                  progress};
    micropixel::Timer ticker = app.timers().Every(micropixel::Duration::Microseconds(kRenderTargetPeriodUs));
    game.Render();
    app.log().Info("tilt: ready; native 480/720 art selection and accelerometer control");

    app.Run([&](const micropixel::Event& event) {
        if (const micropixel::TimerEvent* tick = event.TimerFrom(ticker)) {
            game.OnTimer(*tick);
        } else if (const micropixel::TouchEvent* touch = event.touch()) {
            game.OnTouch(*touch);
        } else if (event.type() == micropixel::EventType::kResume) {
            game.OnResume();
        }
    });
    return 0;
}

}  // namespace tilt
