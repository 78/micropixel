#ifndef MICROPIXEL_GUEST_RUNTIME_DIRECT_SURFACE_STATE_HPP
#define MICROPIXEL_GUEST_RUNTIME_DIRECT_SURFACE_STATE_HPP

#include <stdint.h>

namespace micropixel::runtime {

// Ignore queued releases for a destroyed surface; update only the live owner.
void ReleaseSurfaceBuffer(uint32_t handle, uint32_t buffer_index);

// Integer upscale of the App's live Direct Surface (buffer = panel / upscale),
// 0 when no surface exists. Resources::LoadTexture(kSurface) divides the
// display scale by it so textures match the buffer resolution.
[[nodiscard]] uint32_t ActiveSurfaceUpscale();

}  // namespace micropixel::runtime

#endif  // MICROPIXEL_GUEST_RUNTIME_DIRECT_SURFACE_STATE_HPP
