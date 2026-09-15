// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "sdk/micropixel.hpp"
#include "sdk/scene.hpp"

namespace {
using namespace micropixel;
using namespace micropixel::literals;
constexpr uint32_t kWarmupFrames = 60;
constexpr uint32_t kMeasuredFrames = 600;
constexpr Color kBackground = Color::Rgb(13, 20, 29);
constexpr Color kStatic = Color::Rgb(208, 217, 230);
constexpr Color kValue = Color::Rgb(95, 226, 177);

struct Row {
    const char* text;
    int32_t y;
    SystemFont font;
};
constexpr std::array<Row, 15> kRows{{
    {"Battery voltage / stable", 65, SystemFont::kSmall},
    {"Network packets received", 83, SystemFont::kSmall},
    {"Storage capacity / free", 101, SystemFont::kSmall},
    {"System uptime / seconds", 119, SystemFont::kSmall},
    {"Temperature sensor", 148, SystemFont::kMedium},
    {"Average frame time", 169, SystemFont::kMedium},
    {"Motor speed / RPM", 190, SystemFont::kMedium},
    {"Distance travelled", 211, SystemFont::kMedium},
    {"Total energy used", 243, SystemFont::kLarge},
    {"Target position", 269, SystemFont::kLarge},
    {"Current velocity", 295, SystemFont::kLarge},
    {"Signal strength", 321, SystemFont::kLarge},
    {"SCORE TOTAL", 357, SystemFont::kTitle},
    {"LAP RECORD", 388, SystemFont::kTitle},
    {"TIME LEFT", 419, SystemFont::kTitle},
}};
constexpr std::array<const char*, kRows.size()> kChineseLabels{{
    "电池电压 / 稳定",
    "接收网络数据包",
    "可用存储空间",
    "系统运行秒数",
    "温度传感器",
    "平均帧耗时",
    "电机转速",
    "累计行驶距离",
    "总能量消耗",
    "目标位置",
    "当前速度",
    "信号强度",
    "累计得分",
    "圈速纪录",
    "剩余时间",
}};

struct Samples {
    std::array<uint32_t, kMeasuredFrames> frame_us{};
    uint64_t prepare_us{};
    uint64_t draw_us{};
    uint64_t present_us{};
    uint64_t wait_us{};
    uint64_t started_us{};
    uint64_t ended_us{};
};

class Benchmark {
   public:
    int Run() {
        const auto info = app_.renderer().info();
        Assert(info.physical_width() == 480 && info.physical_height() == 480,
               "Font benchmark currently uses a 480 x 480 physical layout");
        const auto locale = app_.localization().CurrentLocale();
        chinese_ = std::string_view(locale.tag()) == "zh-CN";
        uint32_t static_chars = 0;
        for (size_t i = 0; i < kRows.size(); ++i) static_chars += std::strlen(Label(i));
        FixedString<192> message;
        message.Append("FONTBENCH config rows=15 dynamic_chars=90 static_utf8_bytes=");
        message.AppendUint(static_chars);
        message.Append(" warmup=");
        message.AppendUint(kWarmupFrames);
        message.Append(" samples=");
        message.AppendUint(kMeasuredFrames);
        message.Append(chinese_ ? " labels=zh-CN" : " labels=en");
        app_.log().Info(message.c_str());
        if (!RunScene()) return 0;
        if (!Pause(2_s)) return 0;
        if (!RunRaster()) return 0;
        app_.log().Info("FONTBENCH complete; final frame retained, restart App to repeat");
        app_.Run([](const Event&) {});
        return 0;
    }

   private:
    const char* Label(size_t index) const { return chinese_ ? kChineseLabels[index] : kRows[index].text; }
    uint64_t Now() const { return app_.clock().Now().microseconds(); }

    bool Handle(const Event& event) {
        if (event.type() == EventType::kStop) return false;
        if (event.type() == EventType::kResume) {
            interrupted_ = true;
        }
        return true;
    }

    bool Drain() {
        Event event;
        while (app_.PollEvent(event)) {
            if (!Handle(event)) return false;
        }
        return true;
    }

    bool Pause(Duration duration) {
        const auto until = app_.clock().Now() + duration;
        Event event;
        while (app_.clock().Now() < until) {
            if (app_.WaitEventFor(event, 20_ms) && !Handle(event)) return false;
        }
        return true;
    }

    void Values(uint32_t frame) {
        for (size_t i = 0; i < kRows.size(); ++i) {
            const unsigned value = (frame * 37U + static_cast<unsigned>(i) * 691U) % 100000U;
            auto& text = values_[i];
            text[0] = static_cast<char>('0' + value / 10000U);
            text[1] = static_cast<char>('0' + value / 1000U % 10U);
            text[2] = static_cast<char>('0' + value / 100U % 10U);
            text[3] = '.';
            text[4] = static_cast<char>('0' + value / 10U % 10U);
            text[5] = static_cast<char>('0' + value % 10U);
            text[6] = '\0';
        }
    }

    void Record(uint32_t frame, uint64_t before_wait, uint64_t before_prepare, uint64_t before_draw,
                uint64_t before_present, uint64_t end) {
        if (frame < kWarmupFrames) return;
        const uint32_t index = frame - kWarmupFrames;
        if (index == 0) samples_.started_us = before_wait;
        samples_.prepare_us += before_draw - before_prepare;
        samples_.draw_us += before_present - before_draw;
        samples_.present_us += end - before_present;
        samples_.wait_us += before_prepare - before_wait;
        samples_.frame_us[index] = static_cast<uint32_t>(end - before_wait);
        samples_.ended_us = end;
    }

    void Report(const char* mode) {
        std::sort(samples_.frame_us.begin(), samples_.frame_us.end());
        FixedString<512> message;
        message.Append("FONTBENCH result mode=");
        message.Append(mode);
        const auto field = [&](const char* key, uint64_t value) {
            message.Append(key);
            message.AppendUint(value);
        };
        const uint64_t elapsed = samples_.ended_us - samples_.started_us;
        field(" frames=", kMeasuredFrames);
        field(" elapsed_us=", elapsed);
        field(" submit_fps_x100=", uint64_t{kMeasuredFrames} * 100000000U / elapsed);
        field(" prepare_us=", samples_.prepare_us / kMeasuredFrames);
        field(" draw_us=", samples_.draw_us / kMeasuredFrames);
        field(" present_us=", samples_.present_us / kMeasuredFrames);
        field(" wait_us=", samples_.wait_us / kMeasuredFrames);
        field(" p50_us=", samples_.frame_us[kMeasuredFrames / 2]);
        field(" p95_us=", samples_.frame_us[kMeasuredFrames * 95 / 100]);
        field(" max_us=", samples_.frame_us.back());
        field(" interrupted=", interrupted_ ? 1U : 0U);
        Assert(!message.truncated(), "Benchmark report truncated");
        app_.log().Info(message.c_str());
    }

    // Scene coordinates are logical (720); Raster coordinates are physical (480).
    static Point Logical(int32_t x, int32_t y) { return {x * 3 / 2, y * 3 / 2}; }

    bool RunScene() {
        auto created = app_.renderer().CreateScene(kBackground);
        Assert(created.has_value(), "CreateScene failed");
        auto scene = std::move(*created);
        Assert(scene.CreateLabel(Logical(18, 34), "FONT BENCH / SCENE", kValue, SystemFont::kLarge).has_value(),
               "Create scene heading failed");
        Assert(scene.CreateLabel(Logical(18, 461), "15 static labels + 15 changing values", kStatic, SystemFont::kSmall)
                   .has_value(),
               "Create scene footer failed");
        std::array<LabelNode, kRows.size()> labels;
        Values(0);
        for (size_t i = 0; i < kRows.size(); ++i) {
            const auto& row = kRows[i];
            Assert(scene.CreateLabel(Logical(18, row.y), Label(i), kStatic, row.font).has_value(),
                   "Create static label failed");
            auto label = scene.CreateLabel(Logical(350, row.y), values_[i].data(), kValue, row.font);
            Assert(label.has_value(), "Create dynamic label failed");
            labels[i] = *label;
        }
        samples_ = {};
        interrupted_ = false;
        app_.log().Info("FONTBENCH begin mode=scene");
        for (uint32_t frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
            const auto before_wait = Now();
            if (!Drain()) return false;
            if (frame == kWarmupFrames) interrupted_ = false;
            const auto before_prepare = Now();
            Values(frame);
            for (size_t i = 0; i < kRows.size(); ++i) labels[i].SetText(values_[i].data());
            const auto before_draw = Now();
            Assert(app_.renderer().Present(scene).has_value(), "Scene Present failed");
            const auto end = Now();
            Record(frame, before_wait, before_prepare, before_draw, end, end);
        }
        Report("scene");
        return Pause(2_s);
    }

    bool RunRaster() {
        auto created = app_.renderer().CreateHostSurface(2);
        Assert(created.has_value(), "CreateHostSurface failed");
        surface_ = std::move(*created);
        samples_ = {};
        interrupted_ = false;
        app_.log().Info("FONTBENCH begin mode=raster");
        for (uint32_t frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
            const auto before_wait = Now();
            if (!Drain()) return false;
            uint32_t index = 0;
            while (!surface_.AcquireFree(index)) {
                if (!Handle(app_.WaitEvent())) return false;
            }
            if (frame == kWarmupFrames) interrupted_ = false;
            const auto before_prepare = Now();
            Values(frame);
            const auto before_draw = Now();
            bool drawn = true;
            const auto updated = surface_.Update(index, [&](RasterDrawList& list) {
                drawn &= list.FillRect({0, 0, 480, 480}, kBackground);
                drawn &= list.Text({18, 34}, "FONT BENCH / RASTER", kValue, SystemFont::kLarge);
                for (size_t i = 0; i < kRows.size(); ++i) {
                    const auto& row = kRows[i];
                    drawn &= list.Text({18, row.y}, Label(i), kStatic, row.font);
                    drawn &= list.Text({350, row.y}, values_[i].data(), kValue, row.font);
                }
                drawn &= list.Text({18, 461}, "Full redraw / same text and values", kStatic, SystemFont::kSmall);
            });
            Assert(drawn && updated.has_value(), "Raster Update failed");
            const auto before_present = Now();
            Assert(surface_.Present(index).has_value(), "Raster Present failed");
            Record(frame, before_wait, before_prepare, before_draw, before_present, Now());
        }
        Report("raster");
        return true;
    }

    Application app_;
    HostSurface surface_;
    Samples samples_;
    std::array<std::array<char, 8>, kRows.size()> values_{};
    bool interrupted_{};
    bool chinese_{};
};
}  // namespace

int main() {
    Benchmark benchmark;
    return benchmark.Run();
}
