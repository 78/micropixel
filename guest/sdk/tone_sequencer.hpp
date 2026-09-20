#ifndef MICROPIXEL_SDK_TONE_SEQUENCER_HPP
#define MICROPIXEL_SDK_TONE_SEQUENCER_HPP

#include <stdint.h>

#include <span>

#include "sdk/audio.hpp"
#include "sdk/types.hpp"

namespace micropixel {

// Plays multi-note sound effects (`ToneSpec` profiles generated from
// `audio/sfx.json`) through the Host synth. Notes with `delay_ms == 0` start
// immediately; the rest wait in a fixed pool of `Capacity` slots that the App
// drains once per frame with Advance(). Nothing is allocated and a full pool
// drops the late notes rather than blocking. Effects are fire-and-forget; the
// Host mixer owns voices and the device master volume.
//
//   micropixel::ToneSequencer<8> tones{app.audio(), audio_available};
//   tones.Play(my_sfx::kJump);           // in a game event
//   tones.Advance(tick.delta());         // in the frame timer handler
//   tones.StopAll();                     // on game over / pause
template <uint32_t Capacity>
class ToneSequencer final {
   public:
    static_assert(Capacity > 0U, "ToneSequencer needs at least one delay slot");

    // `enabled` false turns every call into a no-op so Apps can keep one code
    // path whether or not the Host offers audio.
    constexpr explicit ToneSequencer(Audio audio, bool enabled = true) : audio_(audio), enabled_(enabled) {}

    [[nodiscard]] constexpr bool enabled() const { return enabled_; }
    constexpr void set_enabled(bool enabled) { enabled_ = enabled; }

    // Starts every note of `profile`. Returns false when a note was dropped
    // (Host rejected it or the delay pool is full); playback of the other notes
    // continues.
    bool Play(std::span<const ToneSpec> profile, uint8_t gain = 255U) {
        if (!enabled_) {
            return true;
        }
        bool complete = true;
        for (const ToneSpec& spec : profile) {
            complete = Schedule(spec.ToTone(gain), Duration::Milliseconds(spec.delay_ms)) && complete;
        }
        return complete;
    }

    // Plays `tone` now. False when the Host rejected the command.
    bool PlayNow(const Tone& tone) {
        if (!enabled_) {
            return true;
        }
        if (audio_.Play(tone).has_value()) {
            return true;
        }
        ++dropped_;
        return false;
    }

    // Plays `tone` after `delay`; a zero delay plays immediately.
    bool Schedule(const Tone& tone, Duration delay) {
        if (!enabled_) {
            return true;
        }
        if (delay.count_microseconds() == 0U) {
            return PlayNow(tone);
        }
        for (Slot& slot : slots_) {
            if (!slot.active) {
                slot.tone = tone;
                slot.remaining_us = delay.count_microseconds();
                slot.active = true;
                return true;
            }
        }
        ++dropped_;
        return false;
    }

    // Advances the pending notes by `delta` and starts those that are due.
    void Advance(Duration delta) {
        if (!enabled_) {
            return;
        }
        const uint64_t delta_us = delta.count_microseconds();
        for (Slot& slot : slots_) {
            if (!slot.active) {
                continue;
            }
            if (delta_us >= slot.remaining_us) {
                slot.active = false;
                (void)PlayNow(slot.tone);
            } else {
                slot.remaining_us -= delta_us;
            }
        }
    }

    // Forgets pending notes without touching what the Host is already playing.
    void Clear() {
        for (Slot& slot : slots_) {
            slot.active = false;
        }
    }

    // Clears pending notes and silences the Host mixer. False when the Host
    // rejected the stop command.
    bool StopAll() {
        Clear();
        if (!enabled_) {
            return true;
        }
        if (audio_.StopAll().has_value()) {
            return true;
        }
        ++dropped_;
        return false;
    }

    [[nodiscard]] uint32_t pending() const {
        uint32_t count = 0U;
        for (const Slot& slot : slots_) {
            count += slot.active ? 1U : 0U;
        }
        return count;
    }
    // Commands dropped since construction. Apps typically log once when this
    // first becomes non-zero; the game itself keeps running.
    [[nodiscard]] constexpr uint32_t dropped() const { return dropped_; }

   private:
    struct Slot final {
        Tone tone{};
        uint64_t remaining_us{};
        bool active{};
    };

    Audio audio_;
    bool enabled_{};
    uint32_t dropped_{};
    Slot slots_[Capacity]{};
};

}  // namespace micropixel

#endif
