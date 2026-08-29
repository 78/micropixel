#ifndef MICROPIXEL_APPS_SHOWCASE_SHOWCASE_APP_HPP
#define MICROPIXEL_APPS_SHOWCASE_SHOWCASE_APP_HPP

#include <stdint.h>

#include "sdk/micropixel.hpp"

namespace showcase {

enum class Mode : uint8_t {
    kTapCounter,
    kColorLab,
    kPixelSketch,
    kOrbitPad,
};

struct Config final {
    const char* title{};
    const char* subtitle{};
    const char* log_name{};
    micropixel::Color accent{micropixel::Color::Green()};
    Mode mode{Mode::kTapCounter};
};

class ShowcaseApp final {
   public:
    explicit ShowcaseApp(const Config& config)
        : config_(config),
          app_(),
          renderer_(app_.renderer()),
          display_(renderer_.info()),
          scene_(renderer_.CreateScene({.logical_width = display_.width(),
                                        .logical_height = display_.height(),
                                        .background = micropixel::Color::Rgb(8U, 17U, 31U)})) {
        micropixel::Assert(display_.width() == 720U && display_.height() == 720U,
                           "showcase: requires a square display");
        RandomizePalette();
        layer_ = scene_.CreateLayer(
            {.clip = {0, 0, static_cast<int32_t>(display_.width()), static_cast<int32_t>(display_.height())}});
        accent_bar_ = scene_.CreateShape({40, 42, 10, 62}, config_.accent, layer_);
        shapes_ = scene_.CreateSpriteBatch(100U, layer_);
        title_ = scene_.CreateLabel({72, 46}, config_.title, micropixel::Color::White(), micropixel::SystemFont::kTitle,
                                    layer_);
        subtitle_ = scene_.CreateLabel({72, 92}, config_.subtitle, micropixel::Color::Rgb(145U, 164U, 189U),
                                       micropixel::SystemFont::kMedium, layer_);
        for (micropixel::LabelNode& label : mode_labels_) {
            label = scene_.CreateLabel({360, 620}, " ", micropixel::Color::White(), micropixel::SystemFont::kMedium,
                                       layer_, true);
        }
    }

    int Run() {
        Render();
        app_.log().Info(config_.log_name);
        app_.Run([this](const micropixel::Event& event) {
            if (event.type() == micropixel::EventType::kStop) {
                return;
            }
            bool redraw = event.type() == micropixel::EventType::kResume;
            if (const micropixel::TouchEvent* touch = event.touch()) {
                redraw = HandleTouch(*touch) || redraw;
            }
            if (redraw) {
                Render();
            }
        });
        return 0;
    }

   private:
    static constexpr int32_t kCanvasLeft = 60;
    static constexpr int32_t kCanvasTop = 190;
    static constexpr int32_t kCanvasWidth = 600;
    static constexpr int32_t kCanvasHeight = 390;
    static constexpr uint32_t kGridColumns = 12U;
    static constexpr uint32_t kGridRows = 8U;

    [[nodiscard]] bool HandleTouch(const micropixel::TouchEvent& touch) {
        switch (config_.mode) {
            case Mode::kTapCounter:
                if (touch.phase() == micropixel::TouchPhase::kUp) {
                    ++tap_count_;
                    return true;
                }
                return false;
            case Mode::kColorLab:
                if (touch.phase() == micropixel::TouchPhase::kDown) {
                    RandomizePalette();
                    ++tap_count_;
                    return true;
                }
                return false;
            case Mode::kPixelSketch:
                if (touch.phase() != micropixel::TouchPhase::kDown && touch.phase() != micropixel::TouchPhase::kMove) {
                    return false;
                }
                return PaintCell(touch.x(), touch.y());
            case Mode::kOrbitPad:
                if (touch.phase() == micropixel::TouchPhase::kDown || touch.phase() == micropixel::TouchPhase::kMove) {
                    orbit_x_ = Clamp(touch.x(), kCanvasLeft + 26, kCanvasLeft + kCanvasWidth - 26);
                    orbit_y_ = Clamp(touch.y(), kCanvasTop + 26, kCanvasTop + kCanvasHeight - 26);
                    return true;
                }
                return false;
        }
        return false;
    }

    [[nodiscard]] bool PaintCell(int32_t x, int32_t y) {
        if (x < kCanvasLeft || x >= kCanvasLeft + kCanvasWidth || y < kCanvasTop || y >= kCanvasTop + kCanvasHeight) {
            return false;
        }
        const uint32_t column = static_cast<uint32_t>(x - kCanvasLeft) * kGridColumns / kCanvasWidth;
        const uint32_t row = static_cast<uint32_t>(y - kCanvasTop) * kGridRows / kCanvasHeight;
        const uint32_t index = row * kGridColumns + column;
        if (pixels_[index]) {
            return false;
        }
        pixels_[index] = true;
        ++tap_count_;
        return true;
    }

    void RandomizePalette() {
        for (uint32_t index = 0U; index < 4U; ++index) {
            const uint32_t value = app_.random().U32();
            palette_[index] = micropixel::Color::Rgb(static_cast<uint8_t>(72U + (value & 0x9fU)),
                                                     static_cast<uint8_t>(72U + ((value >> 8U) & 0x9fU)),
                                                     static_cast<uint8_t>(72U + ((value >> 16U) & 0x9fU)));
        }
    }

    void SetShape(micropixel::SceneUpdate& update, uint16_t index, micropixel::Rect rect, micropixel::Color color) {
        shapes_.SetInstance(update, index, {.destination = rect, .color = color, .opacity = 255U, .visible = true});
    }

    void SetModeLabel(micropixel::SceneUpdate& update, uint16_t index, micropixel::Point position, const char* text,
                      micropixel::Color color, micropixel::SystemFont font) {
        mode_labels_[index].SetPosition(update, position);
        mode_labels_[index].SetText(update, text);
        mode_labels_[index].SetColor(update, color);
        mode_labels_[index].SetFont(update, font);
        mode_labels_[index].SetVisible(update, true);
    }

    void Render() {
        auto update = scene_.BeginUpdate();
        for (uint16_t index = 0U; index < shapes_.capacity(); ++index) {
            shapes_.SetInstanceVisible(update, index, false);
        }
        for (micropixel::LabelNode& label : mode_labels_) {
            label.SetVisible(update, false);
        }
        switch (config_.mode) {
            case Mode::kTapCounter:
                RenderTapCounter(update);
                break;
            case Mode::kColorLab:
                RenderColorLab(update);
                break;
            case Mode::kPixelSketch:
                RenderPixelSketch(update);
                break;
            case Mode::kOrbitPad:
                RenderOrbitPad(update);
                break;
        }
        micropixel::Assert(update.Present().has_value(), "showcase: scene update failed");
    }

    void RenderTapCounter(micropixel::SceneUpdate& update) {
        SetShape(update, 0U, {kCanvasLeft, kCanvasTop, kCanvasWidth, kCanvasHeight},
                 micropixel::Color::Rgb(17U, 31U, 50U));
        micropixel::FixedString<32U> count;
        count.AppendUint(tap_count_);
        SetModeLabel(update, 0U, {static_cast<int32_t>(display_.width() / 2U), 294}, count.c_str(), config_.accent,
                     micropixel::SystemFont::kTitle);
        SetModeLabel(update, 1U, {static_cast<int32_t>(display_.width() / 2U), 380}, "TAPS", micropixel::Color::White(),
                     micropixel::SystemFont::kLarge);
        const int32_t filled = static_cast<int32_t>((tap_count_ % 20U) * 26U);
        SetShape(update, 1U, {100, 474, 520, 22}, micropixel::Color::Rgb(33U, 54U, 78U));
        if (filled > 0) {
            SetShape(update, 2U, {100, 474, filled, 22}, config_.accent);
        }
        SetModeLabel(update, 2U, {static_cast<int32_t>(display_.width() / 2U), 620}, "Tap anywhere to count",
                     micropixel::Color::Rgb(145U, 164U, 189U), micropixel::SystemFont::kMedium);
    }

    void RenderColorLab(micropixel::SceneUpdate& update) {
        for (uint32_t index = 0U; index < 4U; ++index) {
            const int32_t x = 60 + static_cast<int32_t>(index % 2U) * 310;
            const int32_t y = 190 + static_cast<int32_t>(index / 2U) * 200;
            SetShape(update, static_cast<uint16_t>(index), {x, y, 290, 180}, palette_[index]);
        }
        SetModeLabel(update, 0U, {static_cast<int32_t>(display_.width() / 2U), 620}, "Tap to generate a new palette",
                     micropixel::Color::Rgb(145U, 164U, 189U), micropixel::SystemFont::kMedium);
    }

    void RenderPixelSketch(micropixel::SceneUpdate& update) {
        constexpr int32_t kCellWidth = kCanvasWidth / static_cast<int32_t>(kGridColumns);
        constexpr int32_t kCellHeight = kCanvasHeight / static_cast<int32_t>(kGridRows);
        SetShape(update, 0U, {kCanvasLeft, kCanvasTop, kCanvasWidth, kCanvasHeight},
                 micropixel::Color::Rgb(17U, 31U, 50U));
        for (uint32_t row = 0U; row < kGridRows; ++row) {
            for (uint32_t column = 0U; column < kGridColumns; ++column) {
                const uint32_t index = row * kGridColumns + column;
                const micropixel::Color color =
                    pixels_[index] ? palette_[index % 4U] : micropixel::Color::Rgb(28U, 46U, 69U);
                SetShape(update, static_cast<uint16_t>(index + 1U),
                         {kCanvasLeft + static_cast<int32_t>(column) * kCellWidth + 2,
                          kCanvasTop + static_cast<int32_t>(row) * kCellHeight + 2, kCellWidth - 4, kCellHeight - 4},
                         color);
            }
        }
        SetModeLabel(update, 0U, {static_cast<int32_t>(display_.width() / 2U), 620}, "Touch and drag to paint",
                     micropixel::Color::Rgb(145U, 164U, 189U), micropixel::SystemFont::kMedium);
    }

    void RenderOrbitPad(micropixel::SceneUpdate& update) {
        SetShape(update, 0U, {kCanvasLeft, kCanvasTop, kCanvasWidth, kCanvasHeight},
                 micropixel::Color::Rgb(17U, 31U, 50U));
        uint16_t slot = 1U;
        for (int32_t radius = 240; radius >= 80; radius -= 80) {
            SetShape(update, slot++, {360 - radius / 2, 385 - 2, radius, 4}, micropixel::Color::Rgb(46U, 69U, 98U));
            SetShape(update, slot++, {360 - 2, 385 - radius / 2, 4, radius}, micropixel::Color::Rgb(46U, 69U, 98U));
        }
        SetShape(update, slot, {orbit_x_ - 26, orbit_y_ - 26, 52, 52}, config_.accent);
        SetModeLabel(update, 0U, {static_cast<int32_t>(display_.width() / 2U), 620}, "Drag the marker around the pad",
                     micropixel::Color::Rgb(145U, 164U, 189U), micropixel::SystemFont::kMedium);
    }

    [[nodiscard]] static constexpr int32_t Clamp(int32_t value, int32_t minimum, int32_t maximum) {
        return value < minimum ? minimum : value > maximum ? maximum : value;
    }

    Config config_;
    micropixel::Application app_;
    micropixel::Renderer renderer_;
    micropixel::RendererInfo display_;
    micropixel::Scene scene_;
    micropixel::Layer layer_{};
    micropixel::ShapeNode accent_bar_{};
    micropixel::SpriteBatch shapes_{};
    micropixel::LabelNode title_{};
    micropixel::LabelNode subtitle_{};
    micropixel::LabelNode mode_labels_[3U]{};
    micropixel::Color palette_[4]{micropixel::Color::White(), micropixel::Color::White(), micropixel::Color::White(),
                                  micropixel::Color::White()};
    bool pixels_[kGridColumns * kGridRows]{};
    uint32_t tap_count_{};
    int32_t orbit_x_{360};
    int32_t orbit_y_{385};
};

inline int Run(const Config& config) {
    ShowcaseApp app(config);
    return app.Run();
}

}  // namespace showcase

#endif
