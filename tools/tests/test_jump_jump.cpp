// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "apps/jump-jump/model.hpp"
#include "apps/jump-jump/renderer.hpp"
#include "runtime/graphics/raster_kernels.hpp"

namespace {
using jump_jump::Input;
using jump_jump::Landing;
using jump_jump::Model;
using jump_jump::Phase;
using jump_jump::Platform;
using jump_jump::PlatformKind;
using jump_jump::Vec2;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "jump-jump FAIL: %s\n", message);
        std::exit(1);
    }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.015F; }
uint64_t HoldFor(const Model& model, float offset = 0) {
    const float distance = jump_jump::Length(model.next().position - model.pose().position) + offset;
    return static_cast<uint64_t>(std::lround(distance / 0.00024F));
}
uint64_t Jump(Model& model, uint64_t now, float offset = 0, uint64_t step = 0) {
    const uint64_t hold = HoldFor(model, offset);
    Check(model.Press(now), "ready accepts press");
    if (step)
        for (uint64_t at = now + step; at < now + hold; at += step) model.Advance(at);
    Check(model.Release(now + hold), "owned release launches");
    const uint64_t end = now + hold + Model::kFlightUs + Model::kSettleUs + 1U;
    if (step)
        for (uint64_t at = now + hold + step; at < end; at += step) model.Advance(at);
    model.Advance(end);
    return end;
}

void TestLandingShapes() {
    const Platform box{{0, 0}, 80, PlatformKind::kBox};
    Check(Model::Classify(box, {0, 0}) == Landing::kCenter, "center");
    Check(Model::Classify(box, {9.7F, 0}) == Landing::kSafe, "center boundary is strict");
    Check(Model::Classify(box, {37, 0}) == Landing::kSafe, "full foot support succeeds");
    Check(Model::Classify(box, {38, 0}) == Landing::kEdge, "partial foot support topples");
    Check(Model::Classify(box, {44, 0}) == Landing::kMiss, "outside platform misses");
    Platform round = box;
    round.kind = PlatformKind::kCylinder;
    Check(Model::Classify(box, {30, 30}) == Landing::kSafe, "square corner supported");
    Check(Model::Classify(round, {30, 30}) == Landing::kEdge, "circle does not use square collision");
    Check(Model::Classify(round, {35, 35}) == Landing::kMiss, "circle corner misses");
    round.kind = PlatformKind::kStool;
    round.size = 32;
    Check(Model::Classify(round, {12, 12}) == Landing::kEdge, "small stool has round collision");
    Check(Model::Classify(round, {20, 0}) == Landing::kMiss, "small stool has a smaller landing area");
    Check(Model::Distance(UINT64_MAX) == Model::Distance(Model::kMaximumChargeUs), "charge saturates safely");
}

void TestInputOwnership() {
    Model model;
    Input input;
    model.Reset(5U, 100U);
    Check(input.Down(model, 3U, 100U), "first finger owns charge");
    Check(!input.Down(model, 4U, 200U), "second finger ignored");
    Check(!input.Up(model, 4U, 900U), "second finger cannot release charge");
    input.Cancel(model, 4U, 900U);
    Check(input.active(), "unrelated cancel ignored");
    input.Reset(model, 1000U);
    Check(!input.active() && model.phase() == Phase::kReady, "resume cancels charge");
    Check(!input.Up(model, 3U, 1200U), "stale up after resume ignored");
    Check(input.Down(model, 7U, 1300U), "can charge after resume");
    input.Cancel(model, 7U, 1500U);
    Check(model.phase() == Phase::kReady, "cancel never jumps");
    Check(input.Down(model, uint64_t{1U} << 32U, 2000U), "key token accepted");
    Check(!input.Up(model, 0U, 20000U), "touch id zero cannot release a key");
    Check(input.Up(model, uint64_t{1U} << 32U, 802000U), "matching key releases");
    Check(!input.Down(model, 1U, 810000U), "airborne input is not buffered");
    Check(!input.Up(model, uint64_t{1U} << 32U, 900000U), "duplicate release ignored");
}

void TestImmediateStartAndSafeRestart() {
    Model model;
    Input input;
    model.Reset(42U, 0U);
    Check(model.phase() == Phase::kReady, "launch is immediately ready without a start click");
    Check(input.Down(model, 9U, 2000000U), "first press starts charging");
    model.Advance(2600000U);
    Check(model.phase() == Phase::kCharging && model.charge() > 0.0F, "first hold charges immediately");
    Check(!input.Up(model, 8U, 2700000U), "other finger cannot release first charge");
    Check(input.Up(model, 9U, 3200000U), "first release jumps");
    const uint64_t over = 3200000U + Model::kFlightUs + Model::kFallUs + 300000U;
    model.Advance(over);
    Check(model.phase() == Phase::kGameOver, "restart scenario game over");
    Check(!input.Down(model, 4U, over), "restart press cannot charge");
    input.Cancel(model, 4U, over + 100U);
    Check(!input.Up(model, 4U, over + 200U) && model.phase() == Phase::kGameOver, "cancelled restart stays on results");
    Check(!input.Down(model, 4U, over + 300U), "new restart press");
    Check(!input.Up(model, 4U, over + 1000000U), "even held restart cannot jump");
    model.Advance(over + 2000000U);
    Check(model.phase() == Phase::kReady && model.score() == 0U && model.TakeFeedback() == 0U,
          "restart leaves new run at rest");
}

void TestScoringAndReplay() {
    Model fast, slow;
    fast.Reset(0x12345678U, 0U);
    slow.Reset(0x12345678U, 0U);
    uint64_t a = 0U, b = 0U;
    uint32_t expected = 0U;
    for (uint32_t i = 1U; i <= 1000U; ++i) {
        a = Jump(fast, a, 0, 16000U);
        b = Jump(slow, b, 0, 170000U);
        expected += (i <= 16U ? i : 16U) * 2U;
        Check(fast.phase() == Phase::kReady && slow.phase() == Phase::kReady, "generated route reachable");
        Check(a == b && fast.score() == slow.score() && fast.score() == expected, "frame rate independent score");
        Check(Near(fast.pose().position.x, slow.pose().position.x) &&
                  Near(fast.pose().position.z, slow.pose().position.z),
              "frame rate independent landing");
        Check(Near(fast.next().position.x, slow.next().position.x) && fast.next().kind == slow.next().kind &&
                  Near(fast.next().size, slow.next().size),
              "seeded route replays");
        Check(jump_jump::Length(fast.current().position) == 0.0F, "rebased world bounded after long play");
        Check(HoldFor(fast) < Model::kMaximumChargeUs, "next center inside charge range");
        const uint32_t score = fast.score();
        fast.Advance(a);
        Check(fast.score() == score, "same timestamp cannot double award");
    }
    const uint32_t score = fast.score();
    a = Jump(fast, a, fast.next().size * 0.25F);
    Check(fast.score() == score + 1U && fast.combo() == 0U, "off-center breaks combo");
    a = Jump(fast, a);
    Check(fast.score() == score + 3U && fast.combo() == 1U, "center restarts at two");
    fast.Reset(0U, a);
    Check(fast.score() == 0U && fast.combo() == 0U && !fast.has_previous(), "restart clears run");
}

void TestMissesAndShortHops() {
    Model model;
    model.Reset(2U, 0U);
    Check(model.Press(0U) && model.Release(0U), "zero hold supported");
    model.Advance(Model::kFlightUs + Model::kSettleUs);
    Check(model.phase() == Phase::kReady && model.score() == 0U && model.jumps() == 0U, "same platform has no points");
    uint64_t now = Model::kFlightUs + Model::kSettleUs;
    Check(model.Press(now) && model.Release(now + Model::kMaximumChargeUs), "long press");
    now += Model::kMaximumChargeUs + Model::kFlightUs;
    model.Advance(now);
    Check(model.phase() == Phase::kFalling && model.last_landing() == Landing::kMiss, "overshoot falls");
    Check(!model.Press(now + 1U), "cannot rescue falling character by press");
    model.Advance(now + Model::kFallUs);
    Check(model.phase() == Phase::kGameOver, "fall completes to results");
    Check((model.TakeFeedback() & jump_jump::kFail) != 0U && model.TakeFeedback() == 0U,
          "failure feedback consumed once");
    model.Reset(2U, 0U);
    const float edge = model.next().size * 0.5F;
    now = Jump(model, 0U, edge);
    Check(model.phase() == Phase::kFalling && model.last_landing() == Landing::kEdge,
          "edge topples rather than scores");
    model.Advance(now + 20000000U);
    Check(model.phase() == Phase::kGameOver && model.score() == 0U, "large timer gap crosses fall correctly");
    model.Reset(2U, 0U);
    Check(model.Press(0U) && model.Release(333333U), "short gap jump");
    model.Advance(333333U + Model::kFlightUs + Model::kFallUs);
    Check(model.phase() == Phase::kGameOver && model.score() == 0U, "undershoot into gap fails");
}

void TestDelayedInput() {
    Model timely, delayed;
    timely.Reset(29U, 0U);
    delayed.Reset(29U, 0U);
    const uint64_t hold = HoldFor(timely);
    Check(timely.Press(1000U) && delayed.Press(1000U), "delayed input scenario starts");
    timely.Release(1000U + hold);
    // Simulate a render/timer event observed before a queued release. The
    // release timestamp, not dispatch wall-time, must determine distance.
    delayed.Advance(1000U + hold + 180000U);
    delayed.Release(1000U + hold);
    timely.Advance(1000U + hold + Model::kFlightUs + Model::kSettleUs);
    delayed.Advance(1000U + hold + Model::kFlightUs + Model::kSettleUs);
    Check(timely.score() == 2U && delayed.score() == timely.score(), "queued release preserves center hit");
    Check(Near(timely.pose().position.x, delayed.pose().position.x), "queued release preserves landing");
    delayed.Advance(0U);
    Check(delayed.phase() == Phase::kReady && delayed.score() == 2U, "old events cannot rewind simulation");
}

void TestSpecialRewards() {
    Model model;
    model.Reset(123U, 0U);
    uint64_t now = 0U;
    unsigned seen = 0U;
    for (unsigned i = 0; i < 1600U && seen != 15U; ++i) {
        now = Jump(model, now);
        const auto kind = model.current().kind;
        const uint32_t bonus = Model::Bonus(kind);
        if (bonus == 0U) continue;
        seen |= 1U << (static_cast<unsigned>(kind) - 2U);
        const uint32_t score = model.score();
        const uint64_t landed = now - Model::kSettleUs - 1U;
        model.Advance(landed + Model::kBonusUs - 1U);
        Check(model.score() == score, "no bonus before two seconds from landing");
        now = landed + Model::kBonusUs;
        model.Advance(now);
        Check(model.score() == score + bonus && model.current().rewarded, "special dwell reward");
        model.Advance(now + Model::kBonusUs * 3U);
        now += Model::kBonusUs * 3U;
        Check(model.score() == score + bonus, "reward cannot repeat");
        Check(model.Press(now) && model.Release(now), "zero hop on rewarded platform");
        now += Model::kFlightUs + Model::kSettleUs + Model::kBonusUs;
        model.Advance(now);
        Check(model.score() == score + bonus, "return landing cannot farm special reward");
    }
    Check(seen == 15U, "all four bonus kinds reached");
    model.Reset(123U, now);
    do {
        now = Jump(model, now);
    } while (Model::Bonus(model.current().kind) == 0U);
    const uint32_t score = model.score();
    Check(model.Press(now), "charge on special");
    model.Advance(now + Model::kBonusUs * 2U);
    Check(model.score() == score, "charging is not idle dwell");
    model.Cancel(now + Model::kBonusUs * 2U);
    now += Model::kBonusUs * 2U;
    model.Advance(now + Model::kBonusUs - 1U);
    Check(model.score() == score, "cancel restarts dwell timer");
    model.Advance(now + Model::kBonusUs);
    Check(model.score() > score, "bonus available after a fresh idle interval");
}

void TestRewardCadence() {
    unsigned rewarded = 0U, stools = 0U, early_stools = 0U, small_stools = 0U;
    for (uint32_t seed = 1U; seed <= 16U; ++seed) {
        Model model;
        model.Reset(seed, 0U);
        uint64_t now = 0U;
        unsigned last = 0U;
        for (unsigned jump = 1U; jump <= 1000U; ++jump) {
            now = Jump(model, now);
            if (Model::Bonus(model.current().kind)) {
                Check(jump - last >= 5U, "bonus platforms cannot appear back to back");
                last = jump;
                ++rewarded;
            }
            if (model.current().kind == PlatformKind::kStool) {
                Check(model.current().size <= 86, "table follows the ordinary platform scale");
                if (model.score() < 100U) ++early_stools;
                if (model.current().size < 40) ++small_stools;
                ++stools;
            }
        }
    }
    Check(rewarded > 240U && rewarded < 1280U && stools > 0U && early_stools > 0U && small_stools > 0U,
          "sparse rewards, early tables and later small landing surfaces across seeds");
    std::printf("jump-jump: %u / 16000 reward platforms, %u stools\n", rewarded, stools);
}

void Export(const jump_jump::Frame& frame, const char* name, jump_jump::Viewport viewport = {}) {
    const char* directory = std::getenv("JUMP_JUMP_PREVIEW_DIR");
    if (directory == nullptr) return;
    std::filesystem::create_directories(directory);
    std::ofstream svg(std::filesystem::path(directory) / (std::string(name) + ".svg"));
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << viewport.width << "\" height=\"" << viewport.height
        << "\" viewBox=\"0 0 " << viewport.width << ' ' << viewport.height << "\">\n";
    for (size_t i = 0U; i < frame.polygon_count; ++i) {
        const auto& poly = frame.polygons[i];
        const auto color = jump_jump::Palette(poly.color);
        svg << "<polygon points=\"";
        for (unsigned j = 0U; j < poly.count; ++j) svg << poly.corners[j].x << ',' << poly.corners[j].y << ' ';
        svg << "\" fill=\"rgb(" << unsigned(color.r) << ',' << unsigned(color.g) << ',' << unsigned(color.b)
            << ")\"/>\n";
    }
    for (size_t i = 0U; i < frame.text_count; ++i) {
        const auto& text = frame.texts[i];
        const auto color = jump_jump::Palette(text.color);
        const auto extent = viewport.width < viewport.height ? viewport.width : viewport.height;
        const unsigned size = extent < 480U    ? (text.size == 2U ? 18U : (text.size == 1U ? 12U : 10U))
                              : extent >= 720U ? (text.size == 2U ? 32U : (text.size == 1U ? 18U : 14U))
                                               : (text.size == 2U ? 26U : (text.size == 1U ? 16U : 14U));
        svg << "<text x=\"" << text.position.x << "\" y=\"" << text.position.y + size * 0.85F
            << "\" text-anchor=\"middle\" font-family=\"Arial,sans-serif\" font-size=\"" << size << "\" fill=\"rgb("
            << unsigned(color.r) << ',' << unsigned(color.g) << ',' << unsigned(color.b) << ")\">" << text.value
            << "</text>\n";
    }
    svg << "</svg>\n";
}

void TestAdaptiveViewports() {
    auto frame = std::make_unique<jump_jump::Frame>();
    const jump_jump::Viewport viewports[] = {{320U, 240U}, {480U, 480U}, {720U, 720U}, {240U, 320U}};
    for (const auto viewport : viewports) {
        Check(viewport.Contains(0, 0) && viewport.Contains(viewport.width - 1, viewport.height - 1),
              "native screen corners accept touch, including outside the square playfield");
        Check(!viewport.Contains(-1, 0) && !viewport.Contains(viewport.width, 0) &&
                  !viewport.Contains(0, viewport.height),
              "out-of-screen touches rejected");
        const auto origin = viewport.Map({0, 0});
        const auto end = viewport.Map({480, 480});
        Check(Near(end.x - origin.x, end.y - origin.y), "scene preserves aspect ratio on rectangular displays");
        Check(origin.x >= 0 && origin.y >= 0 && end.x <= viewport.width && end.y <= viewport.height,
              "whole design canvas fits without cropping");
        Model model;
        Input input;
        model.Reset(6U, 0U);
        const auto draw = [&](const char* phase) {
            jump_jump::Render(model, 257U, *frame, UINT64_MAX, viewport);
            Check(!frame->overflow, "adaptive frame remains bounded");
            Check(Near(frame->polygons[0].corners[0].x, 0) && Near(frame->polygons[0].corners[0].y, 0) &&
                      Near(frame->polygons[0].corners[2].x, viewport.width) &&
                      Near(frame->polygons[0].corners[2].y, viewport.height),
                  "background covers native surface");
            for (size_t i = 0; i < frame->text_count; ++i)
                Check(viewport.Contains(frame->texts[i].position.x, frame->texts[i].position.y),
                      "all UI anchors remain on screen");
            const std::string name =
                std::to_string(viewport.width) + "x" + std::to_string(viewport.height) + "-" + phase;
            Export(*frame, name.c_str(), viewport);
        };
        draw("ready");
        const auto hold = HoldFor(model);
        Check(input.Down(model, 1U, 200000U), "adapted viewport starts charge");
        model.Advance(200000U + hold / 2U);
        draw("charge");
        Check(input.Up(model, 1U, 200000U + hold), "adapted viewport releases jump");
        model.Advance(200000U + hold + Model::kFlightUs / 2U);
        draw("flight");
        uint64_t now = 200001U + hold + Model::kFlightUs + Model::kSettleUs;
        model.Advance(now);
        Check(model.phase() == Phase::kReady && model.score() == 2U,
              "identical hold lands at center on every resolution");
        draw("landed");
        for (unsigned i = 0; i < 100U; ++i) {
            now = Jump(model, now);
            model.Advance(now += 400000U);
            for (const Platform* p : {&model.current(), &model.next()}) {
                const auto center =
                    viewport.Map(jump_jump::Project(p->position, Model::kPlatformHeight, model.camera()));
                const float margin = p->size * 0.87F * viewport.scale();
                Check(center.x > margin && center.x < viewport.width - margin && center.y > 0 &&
                          center.y < viewport.height,
                      "both playable platforms remain visible across route and camera directions");
            }
        }
        Check(model.Press(now) && model.Release(now + Model::kMaximumChargeUs), "adaptive death scenario");
        model.Advance(now + Model::kMaximumChargeUs + Model::kFlightUs + Model::kFallUs);
        Check(model.phase() == Phase::kGameOver, "adaptive results scenario reached");
        draw("results");
        Check(!input.Down(model, 2U, now + 5000000U) && !input.Up(model, 2U, now + 5100000U) &&
                  model.phase() == Phase::kReady,
              "adapted restart still cannot jump");
    }
}

void TestDamageRendering() {
    namespace raster = micropixel::runtime::raster;
    auto frame = std::make_unique<jump_jump::Frame>();
    std::array<uint16_t, 256> colors{};
    for (unsigned i = 0; i < colors.size(); ++i) colors[i] = i + 1U;
    const raster::Palette palette{colors.data(), 1U, false};
    for (const jump_jump::Viewport viewport : {jump_jump::Viewport{320, 240}, {480, 480}, {720, 720}, {240, 320}}) {
        const auto pixels = viewport.width * viewport.height;
        std::vector<uint16_t> reference(pixels, colors[0]);
        std::array<std::vector<uint16_t>, 3> buffers{reference, reference, reference};
        jump_jump::Damage damage[3]{};
        Model model;
        model.Reset(6U, 0U);
        uint32_t serial = 0U;
        const auto paint = [&](std::vector<uint16_t>& buffer, jump_jump::Damage* coverage) {
            const raster::Target target{reinterpret_cast<uint8_t*>(buffer.data()), viewport.width, viewport.height,
                                        viewport.width * 2U, false};
            for (size_t i = 1; i < frame->polygon_count; ++i) {
                const auto& polygon = frame->polygons[i];
                micropixel_raster_vertex_t corners[4]{};
                for (unsigned c = 0; c < polygon.count; ++c) {
                    corners[c].x = static_cast<int16_t>(std::floor(polygon.corners[c].x * 16 + 0.5F));
                    corners[c].y = static_cast<int16_t>(std::floor(polygon.corners[c].y * 16 + 0.5F));
                }
                corners[0].u = uint16_t{polygon.color} << 8U;
                raster::DrawPolygon(target, nullptr, palette, MICROPIXEL_RASTER_POLYGON_FLAT_COLOR, corners,
                                    polygon.count);
                if (coverage) coverage->Include(polygon, viewport);
            }
            // Use solid glyph bounds including overhang to exercise UI damage
            // independently of the device font provider.
            for (size_t i = 0; i < frame->text_count; ++i) {
                const auto& text = frame->texts[i];
                const int width = static_cast<int>(std::string(text.value).size()) * 6;
                const int x =
                    static_cast<int>(jump_jump::Clamp(text.position.x - width * 0.5F, 4, viewport.width - width - 4));
                const int y = static_cast<int>(text.position.y);
                micropixel_raster_rect_t rect{};
                rect.x = x - 2;
                rect.y = y - 2;
                rect.width = width + 4;
                rect.height = 20;
                rect.opacity = 255;
                rect.color = colors[text.color];
                raster::DrawRect(target, rect);
                if (coverage)
                    coverage->Include({static_cast<float>(x), static_cast<float>(y)},
                                      {static_cast<float>(x + width), static_cast<float>(y + 16)}, viewport);
            }
        };
        const auto verify = [&] {
            jump_jump::Render(model, 123456U, *frame, 1350000U, viewport);
            // Occasionally reuse the same buffer; buffers need not alternate.
            const auto index = (serial++ / 2U) % 3U;
            auto& buffer = buffers[index];
            uint32_t cleared = 0U;
            for (unsigned i = 0; i < jump_jump::Damage::kBands; ++i) {
                const auto r = damage[index].Rectangle(i, viewport);
                Check(r.x >= 0 && r.y >= 0 && r.x + r.width <= static_cast<int>(viewport.width) &&
                          r.y + r.height <= static_cast<int>(viewport.height),
                      "damage rectangles stay inside native buffer");
                for (int y = r.y; y < r.y + r.height; ++y)
                    std::fill_n(buffer.begin() + y * viewport.width + r.x, r.width, colors[0]);
                cleared += r.width * r.height;
            }
            Check(cleared <= pixels, "damage bands never overlap");
            damage[index].Clear();
            std::fill(reference.begin(), reference.end(), colors[0]);
            paint(reference, nullptr);
            paint(buffer, &damage[index]);
            Check(buffer == reference, "per-buffer damage is pixel-identical to full redraw, without trails");
        };
        verify();
        verify();
        model.Restart(1U);
        uint64_t now = 1U;
        for (unsigned jump = 0; jump < 12; ++jump) {
            verify();
            const auto hold = HoldFor(model);
            model.Press(now);
            model.Advance(now + hold / 2U);
            verify();
            model.Release(now + hold);
            model.Advance(now + hold + Model::kFlightUs / 2U);
            verify();
            model.Advance(now + hold + Model::kFlightUs);
            verify();
            now += hold + Model::kFlightUs + Model::kSettleUs + Model::kBonusUs;
            model.Advance(now);
            verify();
        }
        model.Press(now);
        model.Release(now + Model::kMaximumChargeUs);
        model.Advance(now + Model::kMaximumChargeUs + Model::kFlightUs + Model::kFallUs);
        verify();
        model.Reset(6U, now + 5000000U);
        verify();
        verify();
    }
}

void TestMusicRendering() {
    Model model;
    model.Reset(123U, 0U);
    uint64_t now = 0U;
    for (unsigned i = 0; i < 1600U && model.current().kind != PlatformKind::kRecord; ++i) now = Jump(model, now);
    Check(model.current().kind == PlatformKind::kRecord, "music render scenario reachable");
    now += Model::kBonusUs;
    model.Advance(now);
    auto frame = std::make_unique<jump_jump::Frame>();
    jump_jump::Render(model, model.score(), *frame);
    const auto quiet_count = frame->polygon_count;
    jump_jump::Render(model, model.score(), *frame, 250000U);
    Check(frame->polygon_count > quiet_count && !frame->overflow, "playing music adds rotation and floating note");
    Export(*frame, "music-early");
    jump_jump::Render(model, model.score(), *frame, 1350000U);
    Check(frame->polygon_count > quiet_count && !frame->overflow, "second note appears later");
    Export(*frame, "music-late");
    jump_jump::Render(model, model.score(), *frame);
    Check(frame->polygon_count == quiet_count, "finished playback removes all musical decorations");
    model.Press(now);
    jump_jump::Render(model, model.score(), *frame, 1350000U);
    const auto charging_count = frame->polygon_count;
    jump_jump::Render(model, model.score(), *frame);
    Check(frame->polygon_count == charging_count, "charging stops music animation even with stale play time");
    model.Cancel(now);
    for (unsigned i = 0; i < 1600U && model.next().kind != PlatformKind::kStool; ++i) now = Jump(model, now);
    Check(model.next().kind == PlatformKind::kStool, "stool render scenario reachable");
    jump_jump::Render(model, model.score(), *frame);
    Check(!frame->overflow, "stool render fits the bounded frame");
    Export(*frame, "small-stool");
}

void TestRendering() {
    auto frame = std::make_unique<jump_jump::Frame>();
    Model model;
    model.Reset(123U, 0U);
    jump_jump::Render(model, 42U, *frame);
    Export(*frame, "ready");
    model.Reset(123U, 0U);
    uint64_t now = 0U;
    for (unsigned jump = 0U; jump < 100U; ++jump) {
        const uint64_t hold = HoldFor(model);
        Check(model.Press(now), "render scenario press");
        for (unsigned i = 0U; i < 3U; ++i) {
            model.Advance(now + hold * i / 3U);
            jump_jump::Render(model, 42U, *frame);
            Check(!frame->overflow, "charging draw buffer bounded");
            if (jump == 0U && i == 2U) Export(*frame, "charge");
        }
        model.Release(now + hold);
        for (uint64_t t = 0U; t < Model::kFlightUs + Model::kSettleUs; t += 16000U) {
            model.Advance(now + hold + t);
            jump_jump::Render(model, 42U, *frame);
            Check(!frame->overflow && frame->polygon_count < 900U, "all platforms fit draw budget");
            for (size_t p = 0U; p < frame->polygon_count; ++p) {
                for (unsigned c = 0U; c < frame->polygons[p].count; ++c) {
                    Check(std::isfinite(frame->polygons[p].corners[c].x) &&
                              std::isfinite(frame->polygons[p].corners[c].y),
                          "finite geometry");
                }
            }
            if (jump == 0U && t == 240000U) Export(*frame, "jump");
        }
        now += hold + Model::kFlightUs + Model::kSettleUs + 1U;
        model.Advance(now);
        jump_jump::Render(model, 42U, *frame);
        if (jump < 12U) {
            const std::string name = "platform-" + std::to_string(jump);
            Export(*frame, name.c_str());
        }
        // The camera finishes a little later than the landing squash.
        model.Advance(now + 400000U);
        now += 400000U;
        const auto goal = model.camera();
        for (const Platform* p : {&model.current(), &model.next()}) {
            const auto center = jump_jump::Project(p->position, Model::kPlatformHeight, goal);
            Check(center.x > p->size * 0.87F && center.x < 480 - p->size * 0.87F,
                  "playable platforms fit horizontally");
            Check(center.y > 150 && center.y < 390, "playable platforms fit vertically");
        }
    }
    Check(model.Press(now) && model.Release(now + Model::kMaximumChargeUs), "render death");
    model.Advance(now + Model::kMaximumChargeUs + Model::kFlightUs + Model::kFallUs);
    jump_jump::Render(model, model.score(), *frame);
    Export(*frame, "results");
}
}  // namespace

int main() {
    TestLandingShapes();
    TestInputOwnership();
    TestImmediateStartAndSafeRestart();
    TestScoringAndReplay();
    TestMissesAndShortHops();
    TestDelayedInput();
    TestSpecialRewards();
    TestRewardCadence();
    TestMusicRendering();
    TestRendering();
    TestAdaptiveViewports();
    TestDamageRendering();
    std::puts("jump-jump: gameplay, input, replay, rewards and rendering tests passed");
}
