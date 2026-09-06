#ifndef MICROPIXEL_GUEST_RUNTIME_DIRECT_SURFACE_STATE_HPP
#define MICROPIXEL_GUEST_RUNTIME_DIRECT_SURFACE_STATE_HPP

#include <stdint.h>

namespace micropixel::runtime {

// Ignore queued releases for a destroyed surface; update only the live owner.
void ReleaseSurfaceBuffer(uint32_t handle, uint32_t buffer_index);

}  // namespace micropixel::runtime

#endif  // MICROPIXEL_GUEST_RUNTIME_DIRECT_SURFACE_STATE_HPP
