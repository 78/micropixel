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
        : config_(config), app_(), renderer_(app_.renderer()), display_(renderer_.info()) {
        RandomizePalette();
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

    void Render() {
        micropixel::Frame frame = renderer_.BeginFrame();
        frame.Clear(micropixel::Color::Rgb(8U, 17U, 31U));
        frame.FillRect(micropixel::Rect{40, 42, 10, 62}, config_.accent);
        frame.DrawText(micropixel::Point{72, 46}, config_.title, micropixel::Color::White(),
                       micropixel::SystemFont::kTitle);
        frame.DrawText(micropixel::Point{72, 92}, config_.subtitle, micropixel::Color::Rgb(145U, 164U, 189U),
                       micropixel::SystemFont::kMedium);
        switch (config_.mode) {
            case Mode::kTapCounter:
                RenderTapCounter(frame);
                break;
            case Mode::kColorLab:
                RenderColorLab(frame);
                break;
            case Mode::kPixelSketch:
                RenderPixelSketch(frame);
                break;
            case Mode::kOrbitPad:
                RenderOrbitPad(frame);
                break;
        }
        micropixel::Assert(frame.Present().has_value(), "showcase: frame present failed");
    }

    void RenderTapCounter(micropixel::Frame& frame) const {
        frame.FillRect(micropixel::Rect{kCanvasLeft, kCanvasTop, kCanvasWidth, kCanvasHeight},
                       micropixel::Color::Rgb(17U, 31U, 50U));
        micropixel::FixedString<32U> count;
        count.AppendUint(tap_count_);
        frame.DrawTextCentered(static_cast<int32_t>(display_.width() / 2U), 294, count.c_str(), config_.accent,
                               micropixel::SystemFont::kTitle);
        frame.DrawTextCentered(static_cast<int32_t>(display_.width() / 2U), 380, "TAPS", micropixel::Color::White(),
                               micropixel::SystemFont::kLarge);
        const int32_t filled = static_cast<int32_t>((tap_count_ % 20U) * 26U);
        frame.FillRect(micropixel::Rect{100, 474, 520, 22}, micropixel::Color::Rgb(33U, 54U, 78U));
        if (filled > 0) {
            frame.FillRect(micropixel::Rect{100, 474, filled, 22}, config_.accent);
        }
        frame.DrawTextCentered(static_cast<int32_t>(display_.width() / 2U), 620, "Tap anywhere to count",
                               micropixel::Color::Rgb(145U, 164U, 189U));
    }

    void RenderColorLab(micropixel::Frame& frame) const {
        for (uint32_t index = 0U; index < 4U; ++index) {
            const int32_t x = 60 + static_cast<int32_t>(index % 2U) * 310;
            const int32_t y = 190 + static_cast<int32_t>(index / 2U) * 200;
            frame.FillRect(micropixel::Rect{x, y, 290, 180}, palette_[index]);
        }
        frame.DrawTextCentered(static_cast<int32_t>(display_.width() / 2U), 620, "Tap to generate a new palette",
                               micropixel::Color::Rgb(145U, 164U, 189U));
    }

    void RenderPixelSketch(micropixel::Frame& frame) const {
        constexpr int32_t kCellWidth = kCanvasWidth / static_cast<int32_t>(kGridColumns);
        constexpr int32_t kCellHeight = kCanvasHeight / static_cast<int32_t>(kGridRows);
        frame.FillRect(micropixel::Rect{kCanvasLeft, kCanvasTop, kCanvasWidth, kCanvasHeight},
                       micropixel::Color::Rgb(17U, 31U, 50U));
        for (uint32_t row = 0U; row < kGridRows; ++row) {
            for (uint32_t column = 0U; column < kGridColumns; ++column) {
                const uint32_t index = row * kGridColumns + column;
                const micropixel::Color color =
                    pixels_[index] ? palette_[index % 4U] : micropixel::Color::Rgb(28U, 46U, 69U);
                frame.FillRect(micropixel::Rect{kCanvasLeft + static_cast<int32_t>(column) * kCellWidth + 2,
                                                kCanvasTop + static_cast<int32_t>(row) * kCellHeight + 2,
                                                kCellWidth - 4, kCellHeight - 4},
                               color);
            }
        }
        frame.DrawTextCentered(static_cast<int32_t>(display_.width() / 2U), 620, "Touch and drag to paint",
                               micropixel::Color::Rgb(145U, 164U, 189U));
    }

    void RenderOrbitPad(micropixel::Frame& frame) const {
        frame.FillRect(micropixel::Rect{kCanvasLeft, kCanvasTop, kCanvasWidth, kCanvasHeight},
                       micropixel::Color::Rgb(17U, 31U, 50U));
        for (int32_t radius = 240; radius >= 80; radius -= 80) {
            frame.FillRect(micropixel::Rect{360 - radius / 2, 385 - 2, radius, 4},
                           micropixel::Color::Rgb(46U, 69U, 98U));
            frame.FillRect(micropixel::Rect{360 - 2, 385 - radius / 2, 4, radius},
                           micropixel::Color::Rgb(46U, 69U, 98U));
        }
        frame.FillRect(micropixel::Rect{orbit_x_ - 26, orbit_y_ - 26, 52, 52}, config_.accent);
        frame.DrawTextCentered(static_cast<int32_t>(display_.width() / 2U), 620, "Drag the marker around the pad",
                               micropixel::Color::Rgb(145U, 164U, 189U));
    }

    [[nodiscard]] static constexpr int32_t Clamp(int32_t value, int32_t minimum, int32_t maximum) {
        return value < minimum ? minimum : value > maximum ? maximum : value;
    }

    Config config_;
    micropixel::Application app_;
    micropixel::Renderer renderer_;
    micropixel::RendererInfo display_;
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
