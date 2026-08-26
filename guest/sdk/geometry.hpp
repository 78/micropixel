#ifndef MICROPIXEL_SDK_GEOMETRY_HPP
#define MICROPIXEL_SDK_GEOMETRY_HPP

#include <stdint.h>

namespace micropixel {

// Geometry in the shared logical coordinate space used by Renderer and Input.
struct Point final {
    int32_t x{};
    int32_t y{};

    friend constexpr bool operator==(Point, Point) = default;
};

struct Size final {
    uint32_t width{};
    uint32_t height{};
};

struct Rect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};

    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr int32_t center_x() const { return x + width / 2; }
    [[nodiscard]] constexpr int32_t center_y() const { return y + height / 2; }

    [[nodiscard]] constexpr bool contains(int32_t point_x, int32_t point_y) const {
        return !empty() && point_x >= x && point_y >= y &&
               static_cast<int64_t>(point_x) < static_cast<int64_t>(x) + width &&
               static_cast<int64_t>(point_y) < static_cast<int64_t>(y) + height;
    }

    [[nodiscard]] constexpr bool contains(Point point) const { return contains(point.x, point.y); }

    [[nodiscard]] constexpr Rect translated(int32_t delta_x, int32_t delta_y) const {
        const int64_t translated_x = static_cast<int64_t>(x) + delta_x;
        const int64_t translated_y = static_cast<int64_t>(y) + delta_y;
        return translated_x >= INT32_MIN && translated_x <= INT32_MAX && translated_y >= INT32_MIN &&
                       translated_y <= INT32_MAX
                   ? Rect{static_cast<int32_t>(translated_x), static_cast<int32_t>(translated_y), width, height}
                   : Rect{};
    }

    [[nodiscard]] constexpr Rect inset(int32_t amount) const {
        return Rect{x + amount, y + amount, width - amount * 2, height - amount * 2};
    }

    [[nodiscard]] constexpr Rect intersection(Rect other) const {
        const int32_t left = x > other.x ? x : other.x;
        const int32_t top = y > other.y ? y : other.y;
        const int64_t this_right = static_cast<int64_t>(x) + width;
        const int64_t other_right = static_cast<int64_t>(other.x) + other.width;
        const int64_t this_bottom = static_cast<int64_t>(y) + height;
        const int64_t other_bottom = static_cast<int64_t>(other.y) + other.height;
        const int64_t right = this_right < other_right ? this_right : other_right;
        const int64_t bottom = this_bottom < other_bottom ? this_bottom : other_bottom;
        return right > left && bottom > top
                   ? Rect{left, top, static_cast<int32_t>(right - left), static_cast<int32_t>(bottom - top)}
                   : Rect{};
    }

    friend constexpr bool operator==(Rect, Rect) = default;
};

}  // namespace micropixel

#endif
