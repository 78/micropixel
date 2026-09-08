#ifndef MICROPIXEL_SDK_AUDIO_HPP
#define MICROPIXEL_SDK_AUDIO_HPP

#include <stdint.h>

#include "sdk/event.hpp"
#include "sdk/result.hpp"
#include "sdk/types.hpp"

namespace micropixel {

class AssetId;

enum class Waveform : uint16_t {
    kSine = 1U,
    kSquare = 2U,
    kTriangle = 3U,
    kNoise = 4U,
};

struct AudioInfo final {
    uint32_t sample_rate{};
    uint16_t max_voices{};
    uint32_t supported_waveforms{};
    Duration maximum_tone_duration{};
    uint16_t max_clips{};
    uint16_t max_playbacks{};
    bool supports_ogg_opus{};
    // Audio 1.2: Guest PCM streams (0 when the Host predates them).
    uint16_t max_pcm_streams{};
    bool supports_pcm_stream{};

    [[nodiscard]] constexpr bool Supports(Waveform waveform) const {
        const uint32_t bit = static_cast<uint32_t>(waveform);
        return bit < 32U && (supported_waveforms & (1U << bit)) != 0U;
    }
};

struct PlaybackOptions final {
    uint16_t volume_per_mille{1000U};
    bool loop{};
};

enum class PlaybackState : uint8_t {
    kPlaying,
    kPaused,
    kFinished,
    kFailed,
};

struct Tone final {
    Waveform waveform{Waveform::kSine};
    uint32_t frequency_hz{440U};
    Duration duration{Duration::Milliseconds(150U)};
    uint16_t volume_per_mille{60U};
    Duration attack{Duration::Milliseconds(5U)};
    Duration release{Duration::Milliseconds(20U)};
};

// Move-only reusable compressed source. A playing instance pins the underlying
// Bundle asset, so Reset() may be called immediately after Play().
class AudioClip final {
   public:
    AudioClip() = default;
    AudioClip(const AudioClip&) = delete;
    AudioClip& operator=(const AudioClip&) = delete;
    AudioClip(AudioClip&& other) noexcept;
    AudioClip& operator=(AudioClip&& other) noexcept;
    ~AudioClip();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    void Reset();

   private:
    explicit constexpr AudioClip(uint32_t handle) : handle_(handle) {}
    uint32_t handle_{};

    friend class Audio;
};

// Move-only playback instance. Stop() is terminal; create a new Playback to
// play a clip again. Natural completion is reported through Event.
class Playback final {
   public:
    Playback() = default;
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    Playback(Playback&& other) noexcept;
    Playback& operator=(Playback&& other) noexcept;
    ~Playback();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] Result<void> Pause();
    [[nodiscard]] Result<void> Resume();
    [[nodiscard]] Result<void> SetVolume(uint16_t volume_per_mille);
    [[nodiscard]] Result<PlaybackState> state() const;
    [[nodiscard]] Result<void> Stop();
    void Reset();

   private:
    explicit constexpr Playback(uint32_t handle) : handle_(handle) {}
    [[nodiscard]] constexpr bool Matches(const AudioPlaybackEvent& event) const {
        return handle_ != 0U && event.source_ == handle_;
    }

    uint32_t handle_{};
    friend class Audio;
    friend class Event;
};

struct PcmStreamOptions final {
    // Must equal AudioInfo::sample_rate or divide it evenly; the Host upsamples
    // by linear interpolation.
    uint32_t sample_rate{16000U};
    // 1 or 2 interleaved channels; stereo is averaged down to the mono mixer.
    uint16_t channels{1U};
    // Host ring size in frames at `sample_rate`; also the furthest the Guest can
    // write ahead. The Host may round it up.
    uint32_t capacity_frames{4096U};
    // A PcmStreamEvent fires once each time the buffered frames drop to this
    // value; 0 disables the event and the Guest polls Write()'s free count.
    uint32_t low_water_frames{};
    uint16_t volume_per_mille{1000U};
};

// Move-only Guest-generated PCM stream. Frames are copied into a Host ring at
// Write(); the stream keeps playing (silence on underrun) until Close(),
// Audio::StopAll() or destruction. Only one stream per App at a time.
class PcmStream final {
   public:
    PcmStream() = default;
    PcmStream(const PcmStream&) = delete;
    PcmStream& operator=(const PcmStream&) = delete;
    PcmStream(PcmStream&& other) noexcept;
    PcmStream& operator=(PcmStream&& other) noexcept;
    ~PcmStream();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr uint32_t sample_rate() const { return sample_rate_; }
    [[nodiscard]] constexpr uint16_t channels() const { return channels_; }
    [[nodiscard]] constexpr uint32_t capacity_frames() const { return capacity_frames_; }
    // Frames the Host still had room for after the last Write().
    [[nodiscard]] constexpr uint32_t free_frames() const { return free_frames_; }

    // Copies up to frame_count interleaved frames and returns how many the Host
    // accepted; a short count means the ring is full and the caller should
    // wait for the low-water event (or the next frame) before retrying.
    [[nodiscard]] Result<uint32_t> Write(const int16_t* frames, uint32_t frame_count);
    [[nodiscard]] Result<void> Close();
    void Reset();

   private:
    constexpr PcmStream(uint32_t handle, uint32_t sample_rate, uint16_t channels, uint32_t capacity_frames)
        : handle_(handle),
          sample_rate_(sample_rate),
          channels_(channels),
          capacity_frames_(capacity_frames),
          free_frames_(capacity_frames) {}
    [[nodiscard]] constexpr bool Matches(const PcmStreamEvent& event) const {
        return handle_ != 0U && event.source_ == handle_;
    }

    uint32_t handle_{};
    uint32_t sample_rate_{};
    uint16_t channels_{};
    uint32_t capacity_frames_{};
    uint32_t free_frames_{};
    friend class Audio;
    friend class Event;
};

// Bounded tone synthesis plus reusable Ogg Opus clips. Compressed data and PCM
// buffers stay Host-side; Guest apps deal only in source and playback handles.
class Audio final {
   public:
    [[nodiscard]] Result<AudioInfo> info() const;
    [[nodiscard]] Result<void> Play(const Tone& tone) const;
    [[nodiscard]] Result<AudioClip> Load(AssetId asset) const;
    [[nodiscard]] Result<Playback> Play(const AudioClip& clip, PlaybackOptions options = {}) const;
    [[nodiscard]] Result<Playback> Play(AssetId asset, PlaybackOptions options = {}) const;
    [[nodiscard]] Result<PcmStream> OpenPcmStream(const PcmStreamOptions& options) const;
    [[nodiscard]] Result<void> StopAll() const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Audio(CapabilityToken) noexcept {}
    friend class Application;
};

inline const AudioPlaybackEvent* Event::PlaybackFrom(const Playback& source) const {
    const AudioPlaybackEvent* candidate = audio_playback();
    return candidate != nullptr && source.Matches(*candidate) ? candidate : nullptr;
}

inline const PcmStreamEvent* Event::LowWaterFrom(const PcmStream& source) const {
    const PcmStreamEvent* candidate = pcm_stream();
    return candidate != nullptr && source.Matches(*candidate) ? candidate : nullptr;
}

}  // namespace micropixel

#endif
