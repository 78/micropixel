#pragma once

#include <cstdint>
#include <limits>

namespace micropixel::platform::audio {

[[nodiscard]] constexpr bool ShouldRunAudioOutput(bool app_foreground, bool playable_voice) {
    return app_foreground || playable_voice;
}

[[nodiscard]] constexpr uint32_t NextAudioIdleChunkCount(uint32_t current, bool app_foreground, bool rendered_audio,
                                                         bool playable_voice) {
    if (app_foreground || rendered_audio || playable_voice) {
        return 0U;
    }
    return current == std::numeric_limits<uint32_t>::max() ? current : current + 1U;
}

[[nodiscard]] constexpr bool ShouldStopAudioOutput(bool app_foreground, uint32_t idle_chunks,
                                                   uint32_t idle_grace_chunks, bool playable_voice) {
    return !app_foreground && idle_chunks >= idle_grace_chunks && !playable_voice;
}

}  // namespace micropixel::platform::audio
