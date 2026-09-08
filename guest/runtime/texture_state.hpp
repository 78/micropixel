#ifndef MICROPIXEL_RUNTIME_TEXTURE_STATE_HPP
#define MICROPIXEL_RUNTIME_TEXTURE_STATE_HPP
#include <stdint.h>

namespace micropixel::runtime {
// SDK references keep the original identity. Each successful dynamic update
// points it at an immutable Host snapshot; accepted frames retain old snapshots.
struct DynamicTextureState final {
    uint32_t identity{}, snapshot{}, format{};
};
inline DynamicTextureState dynamic_textures[255]{};
inline uint32_t dynamic_texture_count{};
inline uint64_t texture_revision{};
inline DynamicTextureState* FindDynamicTexture(uint32_t identity) {
    if (dynamic_texture_count == 0 || identity == 0) return nullptr;
    for (auto& state : dynamic_textures)
        if (state.identity == identity) return &state;
    return nullptr;
}
inline bool RegisterDynamicTexture(uint32_t handle, uint32_t format) {
    for (auto& state : dynamic_textures)
        if (state.identity == 0) {
            state = {handle, handle, format};
            ++dynamic_texture_count;
            return true;
        }
    return false;
}
inline uint32_t TextureSnapshot(uint32_t identity) {
    const auto* state = FindDynamicTexture(identity);
    return state ? state->snapshot : identity;
}
inline void ForgetDynamicTexture(uint32_t identity) {
    if (auto* state = FindDynamicTexture(identity)) {
        *state = {};
        --dynamic_texture_count;
        ++texture_revision;
    }
}
}  // namespace micropixel::runtime
#endif
