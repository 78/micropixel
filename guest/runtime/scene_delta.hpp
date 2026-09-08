#ifndef MICROPIXEL_RUNTIME_SCENE_DELTA_HPP
#define MICROPIXEL_RUNTIME_SCENE_DELTA_HPP

#include <stdint.h>

namespace micropixel::detail {

// Scene frames serialize net changes relative to the last accepted frame,
// not the sequence of intermediate setter calls. This lets callers freely rebuild a
// retained batch without emitting properties that were restored before
// Present(). original_dirty preserves an unsent pending change.
constexpr void UpdateScenePropertyDirty(uint32_t& dirty, uint32_t original_dirty, uint32_t property, bool changed) {
    if (changed) {
        dirty |= property;
    } else {
        dirty = (dirty & ~property) | (original_dirty & property);
    }
}

}  // namespace micropixel::detail

#endif
