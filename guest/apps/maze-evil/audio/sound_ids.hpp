#ifndef MICROPIXEL_APPS_MAZE_BREAK_AUDIO_SOUND_IDS_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_AUDIO_SOUND_IDS_HPP

#include <stdint.h>

namespace maze_break::audio {

// Sound effects the game can trigger. Waveform recipes live in audio/sfx.json
// and reach the Host tone synth through the generated profile header.
enum class SoundId : uint8_t {
    kShotgun,
    kEmptyClick,
    kImpAlert,
    kImpFireball,
    kImpMelee,
    kImpPain,
    kImpDeath,
    kFireballExplode,
    kPlayerPain,
    kPickupHealth,
    kPickupAmmo,
    kDoorOpen,
    kDoorClose,
    kExitSealed,
    kWin,
    kDie,
    kCount,
};

struct SoundEvent {
    SoundId id{};
    uint8_t gain{255};  // 0..255 linear, already distance-attenuated
};

}  // namespace maze_break::audio

#endif
