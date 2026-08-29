#ifndef MICROPIXEL_SNAKE_GAMEKIT_CANVAS_GEOMETRY_HPP
#define MICROPIXEL_SNAKE_GAMEKIT_CANVAS_GEOMETRY_HPP

#include <stdint.h>

namespace snake::gamekit {

// Keeps an effect canvas inside the Scene viewport before an atlas frame is
// selected from it. Callers guarantee positive extents and canvas <= viewport.
[[nodiscard]] constexpr int32_t ClampCanvasOrigin(int32_t origin, int32_t canvas_extent,
                                                  int32_t viewport_extent) {
    const int32_t maximum = viewport_extent - canvas_extent;
    return origin < 0 ? 0 : (origin > maximum ? maximum : origin);
}

}  // namespace snake::gamekit

#endif
