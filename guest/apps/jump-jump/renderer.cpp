// SPDX-License-Identifier: Apache-2.0
#include "renderer.hpp"

namespace jump_jump {
namespace {
constexpr float kPi = 3.14159265F;
enum Ink : uint8_t {
    kBackground,
    kGroundShadow,
    kShadow,
    kInk,
    kMuted,
    kWhite,
    kAccent,
    kPale,
    kTop,
    kLeft,
    kRight,
    kOrangeTop,
    kOrangeLeft,
    kOrangeRight,
    kTealTop,
    kTealLeft,
    kTealRight,
    kLilacTop,
    kLilacLeft,
    kLilacRight,
    kYellowTop,
    kYellowLeft,
    kYellowRight,
    kBody,
    kBodyLight,
    kBodyHighlight,
    kRed,
    kBlue,
    kGreen,
    kGold,
    kDrain,
    kDrainDark,
    kRecord,
    kPaper,
    kSoftShadow,
};
constexpr Rgb kPalette[] = {
    {235, 232, 222}, {222, 219, 208}, {195, 193, 182}, {40, 44, 50},    {117, 121, 117}, {255, 255, 249},
    {36, 130, 104},  {211, 226, 212}, {249, 248, 237}, {211, 210, 198}, {228, 227, 214}, {241, 176, 83},
    {192, 121, 54},  {222, 147, 64},  {144, 188, 178}, {83, 139, 130},  {114, 166, 155}, {189, 175, 202},
    {139, 122, 156}, {163, 148, 177}, {229, 211, 130}, {180, 158, 80},  {206, 187, 104}, {40, 42, 54},
    {58, 60, 74},    {89, 89, 105},   {220, 97, 77},   {76, 144, 188},  {113, 170, 124}, {242, 188, 65},
    {157, 167, 166}, {99, 113, 115},  {66, 70, 73},    {248, 245, 233}, {230, 227, 216},
};

// Bounded polynomial on [-pi/2, pi/2]; no Guest libm dependency.
float Sin(float x) {
    while (x > kPi) x -= 2.0F * kPi;
    while (x < -kPi) x += 2.0F * kPi;
    if (x > kPi * 0.5F) x = kPi - x;
    if (x < -kPi * 0.5F) x = -kPi - x;
    const float x2 = x * x;
    return x * (1.0F + x2 * (-1.0F / 6.0F + x2 * (1.0F / 120.0F + x2 * (-1.0F / 5040.0F + x2 / 362880.0F))));
}
float Cos(float x) { return Sin(x + kPi * 0.5F); }
Point Add(Point p, float x, float y) { return {p.x + x, p.y + y}; }

void TopQuad(Frame& f, Vec2 center, Vec2 camera, float height, float x, float z, float w, float d, uint8_t color) {
    f.Quad(Project(center + Vec2{x, z}, height, camera), Project(center + Vec2{x + w, z}, height, camera),
           Project(center + Vec2{x + w, z + d}, height, camera), Project(center + Vec2{x, z + d}, height, camera),
           color);
}

void PlatformShadow(Frame& f, const Platform& p, Vec2 camera) {
    const Point center = Project(p.position, 0.0F, camera);
    f.Ellipse(Add(center, 13.0F, 5.0F), p.size * 0.91F, p.size * 0.49F, kSoftShadow);
    f.Ellipse(Add(center, 10.0F, 3.0F), p.size * 0.82F, p.size * 0.43F, kGroundShadow);
}

void DrawPlatform(Frame& f, const Platform& p, Vec2 camera, float squash, uint64_t music_age_us = UINT64_MAX) {
    const float h = p.size * 0.5F;
    const float height = Model::kPlatformHeight * (1.0F - squash * 0.20F);
    const Point center = Project(p.position, height, camera);
    const bool round =
        p.kind == PlatformKind::kCylinder || p.kind == PlatformKind::kDrain || p.kind == PlatformKind::kRecord;
    const uint8_t top = static_cast<uint8_t>(kTop + p.color * 3U);
    if (p.kind == PlatformKind::kStool) {
        const Point base = Project(p.position, 0, camera);
        for (unsigned i = 0; i < 24U; ++i) {
            const float a = i * kPi / 24.0F, b = (i + 1U) * kPi / 24.0F;
            f.Quad(Add(center, Cos(a) * h * .24F, 4 + Sin(a) * h * .14F),
                   Add(center, Cos(b) * h * .24F, 4 + Sin(b) * h * .14F),
                   Add(base, Cos(b) * h * .73F, Sin(b) * h * .42F), Add(base, Cos(a) * h * .73F, Sin(a) * h * .42F),
                   i < 12U ? kWhite : kPaper);
        }
        f.Ellipse(Add(center, 0, 5), h * 1.224745F, h * 0.707107F, top + 1U);
        f.Ellipse(center, h * 1.224745F, h * 0.707107F, top);
    } else if (round) {
        const float rx = h * 1.224745F;
        const float ry = h * 0.707107F;
        for (unsigned i = 0U; i < 32U; ++i) {
            const float a = static_cast<float>(i) * kPi / 32.0F;
            const float b = static_cast<float>(i + 1U) * kPi / 32.0F;
            const Point edge_a = Add(center, Cos(a) * rx, Sin(a) * ry);
            const Point edge_b = Add(center, Cos(b) * rx, Sin(b) * ry);
            const float light = Clamp(0.52F + 0.48F * Cos((a + b) * 0.5F - 0.55F), 0, 1);
            const auto shade = static_cast<uint8_t>(light * 23.0F);
            f.Quad(edge_a, edge_b, Add(edge_b, 0, height), Add(edge_a, 0, height),
                   static_cast<uint8_t>(64U + p.color * 24U + shade));
        }
        f.Ellipse(center, rx, ry, top);
    } else {
        const Point front = Project(p.position + Vec2{-h, -h}, height, camera);
        const Point right = Project(p.position + Vec2{h, -h}, height, camera);
        const Point back = Project(p.position + Vec2{h, h}, height, camera);
        const Point left = Project(p.position + Vec2{-h, h}, height, camera);
        f.Quad(left, front, Add(front, 0, height), Add(left, 0, height), top + 1U);
        f.Quad(front, right, Add(right, 0, height), Add(front, 0, height), top + 2U);
        f.Quad(front, right, back, left, top);
    }
    switch (p.kind) {
        case PlatformKind::kBox:
            TopQuad(f, p.position, camera, height + 0.2F, -h * 0.58F, -h * 0.58F, h * 1.16F, h * 1.16F, kPaper);
            TopQuad(f, p.position, camera, height + 0.3F, -2, -h * 0.58F, 4, h * 1.16F, top + 2U);
            break;
        case PlatformKind::kCylinder:
            f.Ellipse(center, h * 0.52F, h * 0.30F, kPaper);
            f.Ellipse(center, h * 0.14F, h * 0.08F, top);
            break;
        case PlatformKind::kStool:
            break;
        case PlatformKind::kDrain:
            f.Ellipse(center, h * 1.13F, h * 0.65F, kDrainDark);
            f.Ellipse(center, h * 1.01F, h * 0.58F, kDrain);
            for (int i = -2; i <= 2; ++i) {
                TopQuad(f, p.position, camera, height + 0.3F, -h * 0.48F, static_cast<float>(i) * 8 - 1.5F, h * 0.96F,
                        3.0F, kDrainDark);
            }
            break;
        case PlatformKind::kCube:
            for (int x = 0; x < 3; ++x) {
                for (int z = 0; z < 3; ++z) {
                    const uint8_t colors[] = {kWhite, kGold, kBlue, kRed, kGreen};
                    const unsigned tile = p.rewarded ? 0U : static_cast<unsigned>((x * 3 + z) % 5);
                    TopQuad(f, p.position, camera, height + 0.3F, -h + x * p.size / 3 + 2, -h + z * p.size / 3 + 2,
                            p.size / 3 - 4, p.size / 3 - 4, colors[tile]);
                }
            }
            break;
        case PlatformKind::kShop: {
            TopQuad(f, p.position, camera, height + 0.3F, -h + 5, -h + 5, p.size - 10, p.size - 10, kPaper);
            for (int i = 0; i < 5; ++i) {
                TopQuad(f, p.position, camera, height + 0.4F, -h + i * p.size / 5, -h, p.size / 10, 13, kRed);
            }
            const Point door = Project(p.position + Vec2{-h * 0.20F, -h}, height - 17, camera);
            const Point door_right = Project(p.position + Vec2{h * 0.42F, -h}, height - 17, camera);
            f.Quad(door, door_right, Add(door_right, 0, 29), Add(door, 0, 29), p.rewarded ? kInk : kTealLeft);
            break;
        }
        case PlatformKind::kRecord:
            f.Ellipse(center, h * 1.10F, h * 0.64F, kRecord);
            f.Ellipse(center, h * 0.80F, h * 0.46F, kDrainDark);
            f.Ellipse(center, h * 0.72F, h * 0.41F, kRecord);
            f.Ellipse(center, h * 0.36F, h * 0.21F, kRed);
            f.Ellipse(center, h * 0.07F, h * 0.045F, kPaper);
            if (music_age_us != UINT64_MAX) {
                const float angle = static_cast<float>(music_age_us % 2400000U) * (2.0F * kPi / 2400000.0F);
                for (unsigned i = 0; i < 2U; ++i) {
                    const float a = angle + i * kPi;
                    f.Ellipse(Add(center, Cos(a) * h * 0.86F, Sin(a) * h * 0.50F), 3.0F, 1.7F, kPaper, 8U);
                }
            }
            break;
    }
    if (p.rewarded && Model::Bonus(p.kind) != 0U) {
        f.Ellipse(Add(center, 0, -3), 3.5F, 2.0F, kGold, 12U);
    }
}

void MusicNotes(Frame& f, const Model& model, uint64_t age_us) {
    const Point origin = Project(model.current().position, Model::kPlatformHeight + 5, model.camera());
    for (unsigned i = 0; i < 2U; ++i) {
        const uint64_t delay = i * 1000000U;
        if (age_us < delay) continue;
        const uint64_t age = (age_us - delay) % 2500000U;
        if (age >= 2000000U) continue;
        const float t = static_cast<float>(age) / 2000000.0F;
        const float side = i == 0U ? -1.0F : 1.0F;
        const Point p = Add(origin, side * (29 + t * 22) + Sin(t * kPi * 2) * 5, -t * 88);
        const uint8_t color = static_cast<uint8_t>(184U + static_cast<unsigned>(t * 15));
        f.Ellipse(p, 5, 3, color, 12U);
        f.Rect(p.x + 3, p.y - 16, 2.2F, 16, color);
        if (i == 0U) {
            f.Quad(Add(p, 3, -16), Add(p, 11, -12), Add(p, 11, -8), Add(p, 3, -12), color);
        } else {
            f.Ellipse(Add(p, 12, -3), 5, 3, color, 12U);
            f.Rect(p.x + 15, p.y - 19, 2.2F, 16, color);
            f.Quad(Add(p, 3, -16), Add(p, 17, -19), Add(p, 17, -15), Add(p, 3, -12), color);
        }
    }
}

Point Rotate(Point local, Point origin, float sine, float cosine) {
    return {origin.x + local.x * cosine - local.y * sine, origin.y + local.x * sine + local.y * cosine};
}
void Character(Frame& f, const Pose& pose, Vec2 camera) {
    const Point foot = Project(pose.position, pose.height, camera);
    const float sy = 1.0F - pose.squash;
    const float sx = 1.0F + pose.squash * 0.55F;
    const float sine = Sin(pose.rotation);
    const float cosine = Cos(pose.rotation);
    const Point pivot = Add(foot, 0, -26 * sy);
    auto point = [&](float x, float y) { return Rotate({x * sx, (y + 26) * sy}, pivot, sine, cosine); };
    auto oval = [&](float x, float y, float rx, float ry, uint8_t color) {
        const Point center = point(x, y);
        for (unsigned i = 0U; i < 24U; ++i) {
            const float a = static_cast<float>(i) * (2 * kPi / 24);
            const float b = static_cast<float>(i + 1U) * (2 * kPi / 24);
            f.Triangle(center, point(x + Cos(a) * rx, y + Sin(a) * ry), point(x + Cos(b) * rx, y + Sin(b) * ry), color);
        }
    };
    f.Quad(point(-13, -5), point(13, -5), point(7, -35), point(-7, -35), kBody);
    oval(0, -5, 13, 5, kBody);
    f.Quad(point(-10, -6), point(-4, -6), point(-2, -35), point(-6, -35), kBodyLight);
    oval(0, -37, 8, 4, kBody);
    oval(0, -48, 11.5F, 11.5F, kBody);
    oval(-2.5F, -50.5F, 7.7F, 7.7F, kBodyLight);
    oval(-4.0F, -53.0F, 3.0F, 2.0F, kBodyHighlight);
}

void Reward(Frame& f, const Model& model, Vec2 camera) {
    if (model.last_award() == 0U || model.reward_age_us() > 750000U) return;
    const float t = static_cast<float>(model.reward_age_us()) / 750000.0F;
    const Point center = Project(model.reward_position(), Model::kPlatformHeight + 70.0F + t * 35.0F, camera);
    f.Number(center, model.last_award(), kAccent, 1U, "+");
    if (model.last_award() < 2U) return;
    const Point burst = Project(model.reward_position(), Model::kPlatformHeight + 8.0F, camera);
    for (unsigned i = 0U; i < 10U; ++i) {
        const float angle = static_cast<float>(i) * 2 * kPi / 10;
        const float radius = 14 + 38 * t;
        const Point dot = Add(burst, Cos(angle) * radius, Sin(angle) * radius * 0.55F - 18 * t * (1 - t));
        const float size = 2.6F * (1.0F - t) + 0.5F;
        f.Quad(Add(dot, 0, -size), Add(dot, size, 0), Add(dot, 0, size), Add(dot, -size, 0), i % 2U ? kGold : kWhite);
    }
}
}  // namespace

Rgb Palette(uint8_t index) {
    if (index >= 184U && index < 200U) {
        const unsigned fade = index - 184U;
        const Rgb a = kPalette[kAccent], b = kPalette[kBackground];
        return {static_cast<uint8_t>((a.r * (15U - fade) + b.r * fade) / 15U),
                static_cast<uint8_t>((a.g * (15U - fade) + b.g * fade) / 15U),
                static_cast<uint8_t>((a.b * (15U - fade) + b.b * fade) / 15U)};
    }
    if (index >= 64U && index < 184U) {
        const unsigned family = (index - 64U) / 24U;
        const unsigned shade = (index - 64U) % 24U;
        const Rgb dark = kPalette[kTop + family * 3U + 1U];
        const Rgb light = kPalette[kTop + family * 3U];
        return {static_cast<uint8_t>((dark.r * (23U - shade) + light.r * shade) / 23U),
                static_cast<uint8_t>((dark.g * (23U - shade) + light.g * shade) / 23U),
                static_cast<uint8_t>((dark.b * (23U - shade) + light.b * shade) / 23U)};
    }
    return index < sizeof(kPalette) / sizeof(kPalette[0]) ? kPalette[index] : kPalette[0];
}
Point Project(Vec2 position, float height, Vec2 camera) {
    const Vec2 p = position - camera;
    return {240.0F + (p.x - p.z) * 0.8660254F, 350.0F - (p.x + p.z) * 0.5F - height};
}
void Frame::Clear() {
    polygon_count = text_count = 0U;
    overflow = false;
}
void Frame::Triangle(Point a, Point b, Point c, uint8_t color) {
    if (polygon_count == kMaxPolygons) {
        overflow = true;
        return;
    }
    polygons[polygon_count++] = {{a, b, c, {}}, 3U, color};
}
void Frame::Quad(Point a, Point b, Point c, Point d, uint8_t color) {
    if (polygon_count == kMaxPolygons) {
        overflow = true;
        return;
    }
    polygons[polygon_count++] = {{a, b, c, d}, 4U, color};
}
void Frame::Rect(float x, float y, float w, float h, uint8_t color) {
    Quad({x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}, color);
}
void Frame::Ellipse(Point center, float rx, float ry, uint8_t color, unsigned segments) {
    for (unsigned i = 0; i < segments; ++i) {
        const float a = static_cast<float>(i) * (2 * kPi / static_cast<float>(segments));
        const float b = static_cast<float>(i + 1U) * (2 * kPi / static_cast<float>(segments));
        Triangle(center, Add(center, Cos(a) * rx, Sin(a) * ry), Add(center, Cos(b) * rx, Sin(b) * ry), color);
    }
}
void Frame::Label(Point position, const char* text, uint8_t color, uint8_t size) {
    if (text_count == kMaxTexts) {
        overflow = true;
        return;
    }
    Text& out = texts[text_count++];
    out.position = position;
    out.color = color;
    out.size = size;
    size_t i = 0U;
    for (; text[i] != '\0' && i + 1U < sizeof(out.value); ++i) out.value[i] = text[i];
    out.value[i] = '\0';
}
void Frame::Number(Point position, uint32_t number, uint8_t color, uint8_t size, const char* prefix) {
    char text[32]{};
    size_t length = 0U;
    while (*prefix && length < 16U) text[length++] = *prefix++;
    char digits[10]{};
    size_t count = 0U;
    do {
        digits[count++] = static_cast<char>('0' + number % 10U);
        number /= 10U;
    } while (number != 0U);
    while (count != 0U) text[length++] = digits[--count];
    Label(position, text, color, size);
}

float Viewport::scale() const { return static_cast<float>(width < height ? width : height) / 480.0F; }
Point Viewport::Map(Point point) const {
    const float factor = scale();
    return {static_cast<float>(width) * 0.5F + (point.x - 240.0F) * factor,
            static_cast<float>(height) * 0.5F + (point.y - 240.0F) * factor};
}
bool Viewport::Contains(float x, float y) const { return x >= 0 && y >= 0 && x < width && y < height; }

void Damage::Clear() {
    for (auto& band : bands) band = {};
}
void Damage::Include(Point minimum, Point maximum, Viewport viewport) {
    // Include subpixel rounding and glyph overhang at either edge.
    const auto left = static_cast<uint32_t>(Clamp(minimum.x - 3, 0, viewport.width));
    const auto right = static_cast<uint32_t>(Clamp(maximum.x + 4, 0, viewport.width));
    const auto top = static_cast<uint32_t>(Clamp(minimum.y - 3, 0, viewport.height));
    const auto bottom = static_cast<uint32_t>(Clamp(maximum.y + 4, 0, viewport.height));
    if (right <= left || bottom <= top) return;
    const uint32_t band_height = (viewport.height + kBands - 1U) / kBands;
    for (uint32_t i = top / band_height; i <= (bottom - 1U) / band_height; ++i) {
        auto& band = bands[i];
        if (band.right == 0U || left < band.left) band.left = left;
        if (right > band.right) band.right = right;
    }
}
void Damage::Include(const Polygon& polygon, Viewport viewport) {
    if (polygon.count == 0U) return;
    Point minimum = polygon.corners[0], maximum = minimum;
    for (unsigned i = 1U; i < polygon.count; ++i) {
        const auto point = polygon.corners[i];
        if (point.x < minimum.x) minimum.x = point.x;
        if (point.y < minimum.y) minimum.y = point.y;
        if (point.x > maximum.x) maximum.x = point.x;
        if (point.y > maximum.y) maximum.y = point.y;
    }
    Include(minimum, maximum, viewport);
}
DamageRect Damage::Rectangle(uint32_t index, Viewport viewport) const {
    if (index >= kBands || bands[index].right <= bands[index].left) return {};
    const uint32_t band_height = (viewport.height + kBands - 1U) / kBands;
    const uint32_t top = index * band_height;
    if (top >= viewport.height) return {};
    const uint32_t height = top + band_height > viewport.height ? viewport.height - top : band_height;
    return {static_cast<int32_t>(bands[index].left), static_cast<int32_t>(top),
            static_cast<int32_t>(bands[index].right - bands[index].left), static_cast<int32_t>(height)};
}

void Render(const Model& model, uint32_t best, Frame& f, uint64_t music_age_us, Viewport viewport) {
    f.Clear();
    f.Rect(0, 0, 480, 480, kBackground);
    const Vec2 camera = model.camera();
    const Pose pose = model.pose();
    if (model.current().kind != PlatformKind::kRecord || !model.current().rewarded || model.phase() != Phase::kReady)
        music_age_us = UINT64_MAX;
    if (model.has_previous()) PlatformShadow(f, model.previous(), camera);
    PlatformShadow(f, model.current(), camera);
    PlatformShadow(f, model.next(), camera);

    // Monotone +x/+z route: next is always behind current, previous in front.
    // Insert the character by its ground-depth; once below a platform top,
    // that platform must cover it (edge/miss animation).
    const bool falling = model.phase() == Phase::kFalling || model.phase() == Phase::kGameOver;
    const bool behind_next =
        falling && (pose.position.x + pose.position.z >= model.next().position.x + model.next().position.z);
    if (behind_next && model.phase() != Phase::kGameOver) Character(f, pose, camera);
    DrawPlatform(f, model.next(), camera, 0);
    const bool behind_current = falling && pose.position.x + pose.position.z >= 0.0F;
    if (behind_current && !behind_next && model.phase() != Phase::kGameOver) Character(f, pose, camera);
    DrawPlatform(f, model.current(), camera, model.charge(), music_age_us);
    if (model.has_previous()) DrawPlatform(f, model.previous(), camera, 0);

    if (!falling) {
        const Point shadow = Project(pose.position, Model::kPlatformHeight, camera);
        if (Model::Classify(model.current(), pose.position) != Landing::kMiss ||
            Model::Classify(model.next(), pose.position) != Landing::kMiss) {
            const float scale = 1.0F - Clamp((pose.height - Model::kPlatformHeight) / 200.0F, 0, 0.5F);
            f.Ellipse(shadow, 15 * scale, 7 * scale, kShadow);
        }
        Character(f, pose, camera);
    } else if (!behind_current && model.phase() != Phase::kGameOver) {
        Character(f, pose, camera);
    }
    if (model.phase() == Phase::kCharging) {
        const Point foot = Project(pose.position, pose.height, camera);
        const float charge = model.charge();
        for (unsigned i = 0U; i < 8U; ++i) {
            const float a = static_cast<float>(i) * kPi / 4 + charge * 4;
            const float r = 35 - charge * 20;
            f.Ellipse(Add(foot, Cos(a) * r, Sin(a) * r * 0.45F - 8), 1.8F, 1.8F, kWhite, 8U);
        }
    }
    Reward(f, model, camera);
    if (music_age_us != UINT64_MAX) MusicNotes(f, model, music_age_us);
    if (model.phase() == Phase::kGameOver) {
        f.Rect(67, 101, 352, 297, kGroundShadow);
        f.Rect(60, 94, 352, 297, kPaper);
        f.Label({236, 122}, "NICE TRY", kMuted, 1U);
        f.Number({236, 161}, model.score(), kInk, 2U);
        f.Number({236, 217}, best, kMuted, 0U, "BEST  ");
        f.Number({236, 248}, model.jumps(), kMuted, 0U, "PLATFORMS  ");
        f.Rect(100, 307, 272, 49, kAccent);
        f.Label({236, 319}, "TAP TO TRY AGAIN", kWhite, 1U);
    } else {
        f.Number({240, 28}, model.score(), kInk, 2U);
        f.Number({400, 33}, best, kMuted, 0U, "BEST ");
        if (model.combo() >= 2U) f.Number({240, 76}, model.combo(), kAccent, 0U, "PERFECT x");
        if (model.jumps() == 0U)
            f.Label({240, 452},
                    viewport.scale() < 0.75F ? "HOLD / RELEASE TO JUMP" : "HOLD TO CHARGE / RELEASE TO JUMP", kMuted);
        else if (!model.current().rewarded && Model::Bonus(model.current().kind) != 0U &&
                 model.phase() == Phase::kReady)
            f.Label({240, 452}, viewport.scale() < 0.75F ? "WAIT FOR A BONUS..." : "WAIT A MOMENT. SOMETHING GOOD...",
                    kMuted);
    }
    // HostSurface does not apply ConfigureDisplay transforms. Map every effect,
    // platform and UI shape once here, without changing world-space physics.
    for (size_t i = 1U; i < f.polygon_count; ++i)
        for (unsigned j = 0U; j < f.polygons[i].count; ++j)
            f.polygons[i].corners[j] = viewport.Map(f.polygons[i].corners[j]);
    for (size_t i = 0U; i < f.text_count; ++i) f.texts[i].position = viewport.Map(f.texts[i].position);
    // Fill the entire native surface, including space outside the square playfield.
    f.polygons[0].corners[1] = {static_cast<float>(viewport.width), 0};
    f.polygons[0].corners[2] = {static_cast<float>(viewport.width), static_cast<float>(viewport.height)};
    f.polygons[0].corners[3] = {0, static_cast<float>(viewport.height)};
}
}  // namespace jump_jump
