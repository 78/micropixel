#ifndef MICROPIXEL_SDK_INPUT_HPP
#define MICROPIXEL_SDK_INPUT_HPP

#include <stdint.h>

namespace micropixel {

class Application;

class InputInfo final {
   public:
    [[nodiscard]] constexpr uint32_t logical_width() const { return logical_width_; }
    [[nodiscard]] constexpr uint32_t logical_height() const { return logical_height_; }
    [[nodiscard]] constexpr uint16_t max_touch_points() const { return max_touch_points_; }

   private:
    constexpr InputInfo(uint32_t width, uint32_t height, uint16_t max_touch_points)
        : logical_width_(width), logical_height_(height), max_touch_points_(max_touch_points) {}

    uint32_t logical_width_{};
    uint32_t logical_height_{};
    uint16_t max_touch_points_{};

    friend class Input;
};

class Input final {
   public:
    constexpr Input(const Input&) noexcept = default;
    constexpr Input& operator=(const Input&) noexcept = default;

    [[nodiscard]] InputInfo info() const;

   private:
    struct CapabilityToken {};
    explicit constexpr Input(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
