// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <span>

#include "jump-jump_assets.hpp"
#include "jump-jump_sfx_profiles.hpp"
#include "model.hpp"
#include "renderer.hpp"
#include "sdk/micropixel.hpp"

namespace jump_jump {
namespace {
constexpr uint64_t kKeyToken = uint64_t{1U} << 32U;

class Sound final {
   public:
    explicit Sound(micropixel::Application& app) : app_(app) {
        const auto info = app_.audio().info();
        available_ = info.has_value() && info->Supports(micropixel::Waveform::kSine) &&
                     info->Supports(micropixel::Waveform::kTriangle);
        if (info.has_value() && info->supports_ogg_opus) {
            Load(music_clip_, jump_jump_assets::music_box);
            Load(drain_clip_, jump_jump_assets::drain);
        }
    }
    void Clear() {
        count_ = 0U;
        charging_ = false;
        music_.Reset();
        music_at_us_ = UINT64_MAX;
        if (available_) Check(app_.audio().StopAll().has_value());
    }
    void BeginCharge(uint64_t now) {
        count_ = 0U;
        music_.Reset();
        music_at_us_ = UINT64_MAX;
        charging_ = true;
        charge_at_us_ = now;
        charge_step_ = UINT32_MAX;
        Tick(now);
    }
    // Stop scheduling notes; the active voices finish their own sample-level
    // release envelopes. Never hard-cut them or add a takeoff thump.
    void StopCharge() { charging_ = false; }
    void MusicBox(uint64_t now) {
        Clear();
        Start(music_clip_, music_, false);
        if (music_.valid()) music_at_us_ = now;
    }
    uint64_t music_age_us(uint64_t now) const {
        return music_.valid() && music_at_us_ != UINT64_MAX && now >= music_at_us_ ? now - music_at_us_ : UINT64_MAX;
    }
    void Drain() {
        Clear();
        Start(drain_clip_, music_, false);
    }
    void Perfect(uint32_t combo, uint64_t now) {
        using namespace jump_jump_sfx;
        const ToneSpec* profiles[] = {
            jump_jump_sfx::kPerfect, kPerfect2, kPerfect3, kPerfect4, kPerfect5, kPerfect6, kPerfect7, kPerfect8};
        const uint32_t counts[] = {kPerfectCount,  kPerfect2Count, kPerfect3Count, kPerfect4Count,
                                   kPerfect5Count, kPerfect6Count, kPerfect7Count, kPerfect8Count};
        const uint32_t index = combo > 8U ? 7U : (combo > 0U ? combo - 1U : 0U);
        Play(profiles[index], counts[index], now);
    }
    void OnEvent(const micropixel::Event& event) {
        if (event.PlaybackFrom(music_) != nullptr) {
            music_.Reset();
            music_at_us_ = UINT64_MAX;
        }
    }
    void Play(const jump_jump_sfx::ToneSpec* notes, uint32_t count, uint64_t now) {
        if (!available_) return;
        // A gameplay feedback replaces any unfinished phrase; stale notes
        // must not sound after death, restart, cancellation or resume.
        Clear();
        count_ = count <= 8U ? count : 8U;
        for (uint32_t i = 0U; i < count_; ++i) {
            notes_[i] = &notes[i];
            due_[i] = now + uint64_t{notes[i].delay_ms} * 1000U;
        }
        Tick(now);
    }
    void Tick(uint64_t now) {
        if (charging_ && now >= charge_at_us_) {
            using namespace jump_jump_sfx;
            const uint64_t interval = uint64_t{kCharge1[1].delay_ms} * 1000U;
            const uint32_t step = static_cast<uint32_t>((now - charge_at_us_) / interval);
            if (step != charge_step_) {
                charge_step_ = step;
                const ToneSpec* groups[] = {kCharge1, kCharge2, kCharge3, kCharge4};
                const auto& note = step < kCharge1Count + kCharge2Count + kCharge3Count + kCharge4Count
                                       ? groups[step / kCharge1Count][step % kCharge1Count]
                                       : kChargeHold[0];
                Emit(note);
            }
        }
        for (uint32_t i = 0U; i < count_; ++i) {
            if (notes_[i] == nullptr || due_[i] > now) continue;
            const auto& note = *notes_[i];
            Emit(note);
            notes_[i] = nullptr;
        }
    }

   private:
    void Emit(const jump_jump_sfx::ToneSpec& note) {
        if (!available_) return;
        Check(app_.audio()
                  .Play({note.waveform, note.frequency_hz, micropixel::Duration::Milliseconds(note.duration_ms),
                         note.volume_per_mille, micropixel::Duration::Milliseconds(note.attack_ms),
                         micropixel::Duration::Milliseconds(note.release_ms)})
                  .has_value());
    }
    void Load(micropixel::AudioClip& clip, micropixel::AssetId id) {
        auto loaded = app_.audio().Load(id);
        Check(loaded.has_value());
        if (loaded.has_value()) clip = static_cast<micropixel::AudioClip&&>(loaded.value());
    }
    void Start(const micropixel::AudioClip& clip, micropixel::Playback& playback, bool loop) {
        if (!clip.valid()) return;
        auto started = app_.audio().Play(clip, {.loop = loop});
        Check(started.has_value());
        if (started.has_value()) playback = static_cast<micropixel::Playback&&>(started.value());
    }
    void Check(bool ok) {
        if (!ok && !warned_) {
            warned_ = true;
            app_.log().Info("jump-jump: audio command unavailable; visuals continue");
        }
    }
    micropixel::Application& app_;
    micropixel::AudioClip music_clip_{};
    micropixel::AudioClip drain_clip_{};
    micropixel::Playback music_{};
    const jump_jump_sfx::ToneSpec* notes_[8]{};
    uint64_t due_[8]{};
    uint32_t count_{};
    uint32_t charge_step_{UINT32_MAX};
    uint64_t charge_at_us_{};
    uint64_t music_at_us_{UINT64_MAX};
    bool charging_{};
    bool available_{};
    bool warned_{};
};

class Game final {
   public:
    explicit Game(micropixel::Application& app)
        : app_(app),
          sound_(app),
          timer_(app.timers().Every(micropixel::Duration::Microseconds(33333U)).value()),
          sound_timer_(app.timers().Every(micropixel::Duration::Milliseconds(10U)).value()) {}

    int Run() {
        app_.renderer().ConfigureDisplay({}).value();  // Native raster, text metrics and touch coordinates.
        const auto info = app_.renderer().info();
        if (!info.polygon_supported()) {
            app_.log().Error("jump-jump: polygon raster support is required");
            return 1;
        }
        viewport_ = {info.physical_width(), info.physical_height()};
        if (viewport_.width < 240U || viewport_.height < 240U) {
            app_.log().Error("jump-jump: display dimensions must be at least 240 pixels");
            return 1;
        }
        perf_ = app_.launch_arguments().HasFlag("--perf");
        uint32_t buffers = viewport_.width > 480U || viewport_.height > 480U ? 3U : 2U;
        if (app_.launch_arguments().HasFlag("--buffers=2")) buffers = 2U;
        if (app_.launch_arguments().HasFlag("--buffers=3")) buffers = 3U;
        auto created = [&] {
            auto result = app_.renderer().CreateHostSurface(buffers, 1U);
            if (!result.has_value() && buffers == 3U) {
                app_.log().Info("jump-jump: third buffer unavailable; using two buffers");
                return app_.renderer().CreateHostSurface(2U, 1U);
            }
            return result;
        }();
        if (!created.has_value()) {
            app_.log().Error("jump-jump: cannot allocate display buffers");
            return 1;
        }
        surface_ = static_cast<micropixel::HostSurface&&>(created.value());
        raster_ = app_.renderer().CreateRasterResources().value();
        for (uint32_t i = 0U; i < 256U; ++i) {
            const Rgb color = Palette(static_cast<uint8_t>(i));
            palette_[i] = micropixel::Color::Rgb(color.r, color.g, color.b).rgb565();
        }
        raster_.UploadLitPalette(0U, 1U, std::span<const uint16_t>(palette_, 256U)).value();
        saved_best_ = best_ = app_.storage().GetU32Or("best", 0U);
        const uint64_t now = app_.clock().Now().microseconds();
        model_.Reset(app_.launch_arguments().GetUnsigned("--seed", app_.random().U32()), now);
        stats_at_ = now;
        Draw();
        if (failed_) return 2;
        app_.Run([&](const micropixel::Event& event) { return Handle(event); });
        return failed_ ? 2 : 0;
    }

   private:
    void SaveBest() {
        if (best_ <= saved_best_) return;
        if (app_.storage().SetU32("best", best_).has_value())
            saved_best_ = best_;
        else
            app_.log().Info("jump-jump: could not save best score");
    }

    void Down(uint64_t token, uint64_t now) {
        if (input_.Down(model_, token, now)) sound_.BeginCharge(now);
    }

    void Up(uint64_t token, uint64_t now) {
        const Phase before = model_.phase();
        if (input_.Up(model_, token, now)) sound_.StopCharge();
        if (before == Phase::kGameOver && model_.phase() == Phase::kReady) sound_.Clear();
    }

    micropixel::EventResult Handle(const micropixel::Event& event) {
        const uint64_t now = event.timestamp().microseconds();
        if (event.type() == micropixel::EventType::kStop) {
            if (model_.score() > best_) best_ = model_.score();
            SaveBest();
            sound_.Clear();
            return micropixel::EventResult::kExit;
        }
        if (event.type() == micropixel::EventType::kResume) {
            input_.Reset(model_, now);
            sound_.StopCharge();
        }
        if (const auto* touch = event.touch()) {
            const uint64_t token = touch->id();
            if (touch->phase() == micropixel::TouchPhase::kDown &&
                viewport_.Contains(touch->position().x, touch->position().y))
                Down(token, now);
            else if (touch->phase() == micropixel::TouchPhase::kUp)
                Up(token, now);
            else if (touch->phase() == micropixel::TouchPhase::kCancel) {
                input_.Cancel(model_, token, now);
                if (!input_.active()) sound_.StopCharge();
            }
        }
        if (const auto* key = event.key()) {
            if (key->code() == micropixel::KeyCode::kConfirm || key->code() == micropixel::KeyCode::kSouth) {
                const uint64_t token = kKeyToken | static_cast<uint16_t>(key->code());
                if (key->phase() == micropixel::KeyPhase::kDown)
                    Down(token, now);
                else if (key->phase() == micropixel::KeyPhase::kUp)
                    Up(token, now);
                else if (key->phase() == micropixel::KeyPhase::kCancel) {
                    input_.Cancel(model_, token, now);
                    if (!input_.active()) sound_.StopCharge();
                }
            }
        }
        const Phase before = model_.phase();
        model_.Advance(now);
        if (model_.score() > best_) best_ = model_.score();
        if (before != Phase::kGameOver && model_.phase() == Phase::kGameOver) SaveBest();
        Feedback(now);
        sound_.OnEvent(event);
        sound_.Tick(now);
        if (event.TimerFrom(timer_) != nullptr || event.type() == micropixel::EventType::kResume) Draw();
        return failed_ ? micropixel::EventResult::kExit : micropixel::EventResult::kContinue;
    }

    void Feedback(uint64_t now) {
        const uint32_t flags = model_.TakeFeedback();
        if (flags & kFail)
            sound_.Play(jump_jump_sfx::kGameOver, jump_jump_sfx::kGameOverCount, now);
        else if (flags & kBonus) {
            if (model_.current().kind == PlatformKind::kRecord)
                sound_.MusicBox(now);
            else if (model_.current().kind == PlatformKind::kDrain)
                sound_.Drain();
            else if (model_.current().kind == PlatformKind::kShop)
                sound_.Play(jump_jump_sfx::kShop, jump_jump_sfx::kShopCount, now);
            else
                sound_.Play(jump_jump_sfx::kCube, jump_jump_sfx::kCubeCount, now);
        } else if (flags & kPerfect) {
            sound_.Perfect(model_.combo(), now);
        } else if (flags & kLand)
            sound_.Play(jump_jump_sfx::kLand, jump_jump_sfx::kLandCount, now);
        else if (flags & kJump)
            sound_.StopCharge();
    }

    bool Raster(micropixel::RasterDrawList& list, uint32_t index) {
        // Each buffer keeps its own coverage: it may still contain a frame
        // older than the last presented one. Clear only that buffer's ink.
        const Rgb background = Palette(frame_.polygons[0].color);
        const auto color = micropixel::Color::Rgb(background.r, background.g, background.b);
        if (!initialized_[index]) {
            if (!list.FillRect({0, 0, static_cast<int32_t>(viewport_.width), static_cast<int32_t>(viewport_.height)},
                               color))
                return false;
            initialized_[index] = true;
        } else {
            for (uint32_t band = 0U; band < Damage::kBands; ++band) {
                const auto rect = damage_[index].Rectangle(band, viewport_);
                if (rect.width > 0 && rect.height > 0 &&
                    !list.FillRect({rect.x, rect.y, rect.width, rect.height}, color))
                    return false;
            }
        }
        damage_[index].Clear();
        for (size_t i = 1U; i < frame_.polygon_count; ++i) {
            const auto& poly = frame_.polygons[i];
            damage_[index].Include(poly, viewport_);
            micropixel::RasterVertex corners[4]{};
            for (unsigned j = 0U; j < poly.count; ++j) {
                corners[j] = micropixel::RasterVertex::At(poly.corners[j].x, poly.corners[j].y, 0, 0, 0U);
            }
            if (poly.count == 3U) {
                const micropixel::RasterVertex triangle[3] = {corners[0], corners[1], corners[2]};
                if (!list.FlatTriangle(triangle, poly.color)) return false;
            } else if (!list.FlatQuad(corners, poly.color))
                return false;
        }
        for (size_t i = 0U; i < frame_.text_count; ++i) {
            const auto& text = frame_.texts[i];
            auto font = text.size == 2U
                            ? micropixel::SystemFont::kTitle
                            : (text.size == 1U ? micropixel::SystemFont::kMedium : micropixel::SystemFont::kSmall);
            const auto measured = app_.renderer().MeasureText(text.value, font);
            if (!measured.has_value()) return false;
            const Rgb color = Palette(text.color);
            // System fonts have device-specific pixel sizes, independent of the
            // geometry scale. Keep labels inside the physical screen margins.
            const float x = Clamp(text.position.x - measured->width * 0.5F, 4,
                                  static_cast<float>(viewport_.width) - measured->width - 4);
            const float y = Clamp(text.position.y, 0, static_cast<float>(viewport_.height) - measured->height);
            damage_[index].Include({x, y}, {x + measured->width, y + measured->height}, viewport_);
            if (!list.Text({static_cast<int32_t>(x), static_cast<int32_t>(y)}, text.value,
                           micropixel::Color::Rgb(color.r, color.g, color.b), font))
                return false;
        }
        return !frame_.overflow;
    }

    void Draw() {
        uint32_t index{};
        if (!surface_.AcquireFree(index)) {
            ++skipped_;
            return;
        }
        const uint64_t start = app_.clock().Now().microseconds();
        Render(model_, best_, frame_, sound_.music_age_us(app_.clock().Now().microseconds()), viewport_);
        const uint64_t generated = app_.clock().Now().microseconds();
        bool drawn = false;
        const auto updated =
            surface_.Update(index, [&](micropixel::RasterDrawList& list) { drawn = Raster(list, index); });
        const uint64_t rasterized = app_.clock().Now().microseconds();
        if (!updated.has_value() || !drawn || !surface_.Present(index).has_value()) {
            app_.log().Error("jump-jump: frame submission failed");
            failed_ = true;
            return;
        }
        const uint64_t end = app_.clock().Now().microseconds();
        geometry_us_ += generated - start;
        raster_us_ += rasterized - generated;
        present_us_ += end - rasterized;
        ++frames_;
        if (perf_ && frames_ == 120U) {
            micropixel::FixedString<256U> line;
            line.Append("jump-jump: fps_x100=");
            line.AppendUint(12000000000ULL / (end > stats_at_ ? end - stats_at_ : 1U));
            line.Append(" geometry_us=");
            line.AppendUint(geometry_us_ / frames_);
            line.Append(" host_raster_us=");
            line.AppendUint(raster_us_ / frames_);
            line.Append(" present_us=");
            line.AppendUint(present_us_ / frames_);
            line.Append(" skipped=");
            line.AppendUint(skipped_);
            line.Append(" polygons=");
            line.AppendUint(frame_.polygon_count);
            line.Append(" buffers=");
            line.AppendUint(surface_.buffer_count());
            app_.log().Info(line.c_str());
            frames_ = skipped_ = 0U;
            geometry_us_ = raster_us_ = present_us_ = 0U;
            stats_at_ = end;
        }
    }

    micropixel::Application& app_;
    Sound sound_;
    Model model_{};
    Input input_{};
    Frame frame_{};
    Viewport viewport_{};
    Damage damage_[3]{};
    bool initialized_[3]{};
    micropixel::HostSurface surface_{};
    micropixel::RasterResources raster_{};
    micropixel::Timer timer_;
    micropixel::Timer sound_timer_;
    uint16_t palette_[256]{};
    uint32_t best_{};
    uint32_t saved_best_{};
    uint32_t frames_{};
    uint32_t skipped_{};
    uint64_t stats_at_{};
    uint64_t geometry_us_{};
    uint64_t raster_us_{};
    uint64_t present_us_{};
    bool perf_{};
    bool failed_{};
};
}  // namespace
}  // namespace jump_jump

int main() {
    micropixel::Application app;
    // Keep the fixed render workspace off the Guest call stack.
    auto game = std::make_unique<jump_jump::Game>(app);
    return game->Run();
}
