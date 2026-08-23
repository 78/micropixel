// Audio capability discovery and tone playback for the Demo app. See ../README.md.

#include "apps/demo/demo_page.hpp"

using micropixel::literals::operator""_ms;

namespace demo {

namespace {

[[nodiscard]] micropixel::Rect AudioButtonRect(const DemoContext& context, uint32_t column, uint32_t row) {
    constexpr int32_t kMargin = 42;
    constexpr int32_t kGap = 16;
    constexpr int32_t kTop = 260;
    constexpr int32_t kHeight = 78;
    constexpr int32_t kRowGap = 22;
    const int32_t display_width = static_cast<int32_t>(context.display.width());
    const int32_t width = (display_width - kMargin * 2 - kGap) / 2;
    return micropixel::Rect{kMargin + static_cast<int32_t>(column) * (width + kGap),
                            kTop + static_cast<int32_t>(row) * (kHeight + kRowGap), width, kHeight};
}

class AudioPage final {
   public:
    void Enter(DemoContext& context) {
        auto info = context.app.audio().info();
        available_ = info.has_value();
        if (available_) {
            sample_rate_ = info.value().sample_rate;
            max_voices_ = info.value().max_voices;
            status_ = "Audio capability ready";
            context.app.log().Info("demo.audio: Audio capability ready");
        } else {
            sample_rate_ = 0U;
            max_voices_ = 0U;
            status_ = "Audio capability unavailable";
            context.app.log().Info("demo.audio: Audio capability unavailable");
        }
        for (uint32_t index = 0U; index < 4U; ++index) {
            buttons_[index].SetBounds(AudioButtonRect(context, index % 2U, index / 2U));
            buttons_[index].SetEnabled(available_);
            buttons_[index].Reset();
        }
        buttons_[4].SetBounds(BottomButtonRect(context, 0U, 1U));
        buttons_[4].SetEnabled(available_);
        buttons_[4].Reset();
    }

    void Exit(DemoContext& context) {
        if (available_ && !context.app.audio().StopAll().has_value()) {
            context.app.log().Info("demo.audio: StopAll failed while leaving page");
        }
    }

    [[nodiscard]] bool OnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
        bool redraw = false;
        for (uint32_t index = 0U; index < 5U; ++index) {
            const micropixel::ui::ButtonUpdate update = buttons_[index].OnTouch(event);
            redraw = redraw || update.redraw();
            if (!update.clicked) {
                continue;
            }
            if (index == 0U) {
                Play(context, micropixel::Waveform::kSine, 440U, "Last tone: sine / 440 Hz");
            } else if (index == 1U) {
                Play(context, micropixel::Waveform::kSquare, 554U, "Last tone: square / 554 Hz");
            } else if (index == 2U) {
                Play(context, micropixel::Waveform::kTriangle, 659U, "Last tone: triangle / 659 Hz");
            } else if (index == 3U) {
                Play(context, micropixel::Waveform::kNoise, 0U, "Last tone: noise");
            } else {
                auto stopped = context.app.audio().StopAll();
                status_ = stopped.has_value() ? "All voices stopped" : "StopAll failed";
            }
        }
        return redraw;
    }

    void Render(DemoContext& context, micropixel::CommandBuffer& commands) {
        const int32_t center_x = static_cast<int32_t>(context.display.width() / 2U);
        Line capability;
        capability.Append("Sample rate: ");
        capability.AppendUint(sample_rate_);
        capability.Append(" Hz   Max voices: ");
        capability.AppendUint(max_voices_);
        commands.DrawTextCentered(center_x, 126, capability.c_str(), MutedColor(), 18U);
        commands.DrawTextCentered(center_x, 184, status_, available_ ? AccentColor() : DangerColor(), 22U);

        DrawButton(commands, buttons_[0], "SINE", BlueColor());
        DrawButton(commands, buttons_[1], "SQUARE", BlueColor());
        DrawButton(commands, buttons_[2], "TRIANGLE", BlueColor());
        DrawButton(commands, buttons_[3], "NOISE", BlueColor());
        DrawButton(commands, buttons_[4], "STOP ALL", DangerColor());
    }

   private:
    void Play(DemoContext& context, micropixel::Waveform waveform, uint32_t frequency_hz, const char* status) {
        const micropixel::Tone tone{
            .waveform = waveform,
            .frequency_hz = frequency_hz,
            .duration = 500_ms,
            .volume_per_mille = 90U,
            .attack = 8_ms,
            .release = 80_ms,
        };
        auto played = context.app.audio().Play(tone);
        status_ = played.has_value() ? status : "Audio::Play failed";
        context.app.log().Info(played.has_value() ? "demo.audio: tone submitted" : "demo.audio: tone rejected");
    }

    uint32_t sample_rate_{};
    uint16_t max_voices_{};
    const char* status_{"Not initialized"};
    bool available_{};
    micropixel::ui::Button buttons_[5]{};
};

AudioPage audio_page;

}  // namespace

void AudioDemoEnter(DemoContext& context) { audio_page.Enter(context); }

void AudioDemoExit(DemoContext& context) { audio_page.Exit(context); }

bool AudioDemoOnTouch(DemoContext& context, const micropixel::TouchEvent& event) {
    return audio_page.OnTouch(context, event);
}

void AudioDemoRender(DemoContext& context, micropixel::CommandBuffer& commands) {
    audio_page.Render(context, commands);
}

}  // namespace demo
