#ifndef MICROPIXEL_GUEST_RUNTIME_GRAPHICS_LIMITS_HPP
#define MICROPIXEL_GUEST_RUNTIME_GRAPHICS_LIMITS_HPP

#include <cstdint>

namespace micropixel::runtime {

// Static capacities of the Guest-side Scene and Raster encoders. They size the
// fixed buffers in this runtime; the Host reports its own policy through
// micropixel_graphics_info_t and the effective limit for any request is the
// smaller of the two (LoadGraphicsLimits()). Neither side is a wire constant.
// Scene nodes, containers and batch instances have no count cap on either
// side: their storage grows on demand and creation fails with
// kResourceExhausted when memory or the uint16 wire ids run out.
namespace limits {
inline constexpr uint32_t kMaxTextBytes = 1024U;
inline constexpr uint32_t kMaxSceneBytes = 128U * 1024U;
inline constexpr uint32_t kMaxRasterBytes = 32768U;
inline constexpr uint32_t kMaxSurfaceBuffers = 3U;
// Wire ids are uint16; the Host also indexes every node and batch instance as
// one draw operation, so their sum shares the same space.
inline constexpr uint32_t kMaxSceneItems = 65535U;
}  // namespace limits

// Effective per-session limits: min(Host policy, Guest static capacity).
struct GraphicsLimits final {
    uint32_t max_text_bytes{};
    uint32_t max_scene_bytes{};
    uint32_t max_raster_bytes{};  // 0 when the Host has no raster kernels.
    uint32_t max_surface_buffers{};
};

const GraphicsLimits& LoadGraphicsLimits();

}  // namespace micropixel::runtime

#endif  // MICROPIXEL_GUEST_RUNTIME_GRAPHICS_LIMITS_HPP
