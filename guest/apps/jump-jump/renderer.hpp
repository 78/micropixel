// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_JUMP_JUMP_RENDERER_HPP
#define MICROPIXEL_JUMP_JUMP_RENDERER_HPP

#include <stddef.h>
#include <stdint.h>

#include "model.hpp"

namespace jump_jump {
struct Point final {
    float x{};
    float y{};
};
// Geometry stays in the 480-unit design space; raster and touch use native pixels.
struct Viewport final {
    uint32_t width{480U};
    uint32_t height{480U};
    [[nodiscard]] float scale() const;
    [[nodiscard]] Point Map(Point point) const;
    [[nodiscard]] bool Contains(float x, float y) const;
};
struct Rgb final {
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
};
struct Polygon final {
    Point corners[4]{};
    uint8_t count{};
    uint8_t color{};
};
struct DamageRect final {
    int32_t x{}, y{}, width{}, height{};
};
// Native-pixel coverage retained separately for each surface buffer. Fixed
// horizontal bands bound clearing work without allocating a per-pixel mask.
struct Damage final {
    static constexpr uint32_t kBands = 32U;
    struct Band final {
        uint32_t left{}, right{};
    };
    Band bands[kBands]{};
    void Clear();
    void Include(Point minimum, Point maximum, Viewport viewport);
    void Include(const Polygon& polygon, Viewport viewport);
    [[nodiscard]] DamageRect Rectangle(uint32_t index, Viewport viewport) const;
};
struct Text final {
    Point position{};
    char value[64]{};
    uint8_t color{};
    uint8_t size{};  // 0 small, 1 medium, 2 title. Centred horizontally.
};

// One reusable, bounded draw buffer. The same geometry is used by the device
// raster adapter and the native visual-regression exporter.
struct Frame final {
    static constexpr size_t kMaxPolygons = 1024U;
    static constexpr size_t kMaxTexts = 24U;
    Polygon polygons[kMaxPolygons]{};
    Text texts[kMaxTexts]{};
    size_t polygon_count{};
    size_t text_count{};
    bool overflow{};
    void Clear();
    void Triangle(Point a, Point b, Point c, uint8_t color);
    void Quad(Point a, Point b, Point c, Point d, uint8_t color);
    void Rect(float x, float y, float w, float h, uint8_t color);
    void Ellipse(Point center, float rx, float ry, uint8_t color, unsigned segments = 24U);
    void Label(Point position, const char* text, uint8_t color, uint8_t size = 0U);
    void Number(Point position, uint32_t number, uint8_t color, uint8_t size = 2U, const char* prefix = "");
};

[[nodiscard]] Rgb Palette(uint8_t index);
[[nodiscard]] Point Project(Vec2 position, float height, Vec2 camera);
void Render(const Model& model, uint32_t best, Frame& frame, uint64_t music_age_us = UINT64_MAX,
            Viewport viewport = {});
}  // namespace jump_jump
#endif
