#include "sdk/audio.hpp"

#include "runtime/service_binding.hpp"
#include "sdk/resources.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::OpenService;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache audio_service;

}  // namespace

namespace micropixel {

Result<AudioInfo> Audio::info() const {
    micropixel_audio_info_t raw{};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    uint32_t response_size = 0U;
    if (status == MICROPIXEL_STATUS_OK) {
        status =
            CallService(audio_service, MICROPIXEL_AUDIO_METHOD_GET_INFO, nullptr, 0U, &raw, sizeof(raw), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(raw) || raw.size < sizeof(raw) ||
        raw.interface_major != MICROPIXEL_AUDIO_INTERFACE_MAJOR ||
        raw.interface_minor < MICROPIXEL_AUDIO_INTERFACE_MINOR) {
        return unexpected(Error{ErrorCode::kUnsupported});
    }
    const bool pcm_streams = (raw.capabilities & MICROPIXEL_AUDIO_CAPABILITY_PCM_STREAM) != 0U;
    return AudioInfo{
        raw.sample_rate,
        raw.max_voices,
        raw.supported_waveforms,
        Duration::Milliseconds(raw.max_tone_duration_ms),
        raw.max_clips,
        raw.max_playbacks,
        (raw.capabilities & MICROPIXEL_AUDIO_CAPABILITY_OGG_OPUS) != 0U,
        pcm_streams ? raw.max_pcm_streams : static_cast<uint16_t>(0U),
        pcm_streams,
    };
}

Result<void> Audio::Play(const Tone& tone) const {
    const uint64_t duration_us = tone.duration.count_microseconds();
    const uint64_t attack_us = tone.attack.count_microseconds();
    const uint64_t release_us = tone.release.count_microseconds();
    const uint32_t waveform = static_cast<uint32_t>(tone.waveform);
    if (duration_us == 0U || duration_us > static_cast<uint64_t>(MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS) * 1000U ||
        attack_us > duration_us || release_us > duration_us || tone.volume_per_mille > 1000U ||
        waveform < MICROPIXEL_AUDIO_WAVE_SINE || waveform > MICROPIXEL_AUDIO_WAVE_NOISE ||
        (tone.waveform != Waveform::kNoise && (tone.frequency_hz < 20U || tone.frequency_hz > 20000U))) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_audio_tone_t raw{};
    raw.size = sizeof(raw);
    raw.interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR;
    raw.waveform = static_cast<uint16_t>(tone.waveform);
    raw.volume_per_mille = tone.volume_per_mille;
    raw.frequency_millihz = tone.frequency_hz * 1000U;
    raw.duration_ms = static_cast<uint32_t>((duration_us + 999U) / 1000U);
    raw.attack_ms = static_cast<uint16_t>((attack_us + 999U) / 1000U);
    raw.release_ms = static_cast<uint16_t>((release_us + 999U) / 1000U);
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAY_TONE, &raw, sizeof(raw));
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

Result<void> Audio::StopAll() const {
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_STOP_ALL, nullptr, 0U);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

AudioClip::AudioClip(AudioClip&& other) noexcept : handle_(other.handle_) { other.handle_ = 0U; }

AudioClip& AudioClip::operator=(AudioClip&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        other.handle_ = 0U;
    }
    return *this;
}

AudioClip::~AudioClip() { Reset(); }

void AudioClip::Reset() {
    if (handle_ == 0U) {
        return;
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    if (OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                    MICROPIXEL_AUDIO_INTERFACE_MINOR) == MICROPIXEL_STATUS_OK) {
        (void)CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_CLIP_RELEASE, &request, sizeof(request));
    }
    handle_ = 0U;
}

Playback::Playback(Playback&& other) noexcept : handle_(other.handle_) { other.handle_ = 0U; }

Playback& Playback::operator=(Playback&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        other.handle_ = 0U;
    }
    return *this;
}

Playback::~Playback() { Reset(); }

Result<void> Playback::Pause() {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_PAUSE, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> Playback::Resume() {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_RESUME, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> Playback::SetVolume(uint16_t volume_per_mille) {
    if (handle_ == 0U || volume_per_mille > 1000U) {
        return unexpected(Error{handle_ == 0U ? ErrorCode::kInvalidState : ErrorCode::kInvalidArgument});
    }
    micropixel_audio_playback_volume_request_t request{};
    request.size = sizeof(request);
    request.playback = handle_;
    request.volume_per_mille = volume_per_mille;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_SET_VOLUME, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<PlaybackState> Playback::state() const {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    micropixel_audio_playback_state_response_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_GET_STATE, &request, sizeof(request),
                             &response, sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.playback != handle_ ||
        response.state < MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING ||
        response.state > MICROPIXEL_AUDIO_PLAYBACK_STATE_FAILED) {
        runtime::Panic("audio.playback.state", MICROPIXEL_STATUS_INTERNAL);
    }
    return static_cast<PlaybackState>(response.state - MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING);
}

Result<void> Playback::Stop() {
    if (handle_ == 0U) {
        return {};
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_STOP, &request, sizeof(request));
    }
    if (status == MICROPIXEL_STATUS_OK || status == MICROPIXEL_STATUS_NOT_FOUND) {
        handle_ = 0U;
        return {};
    }
    return unexpected(ErrorFromStatus(status));
}

void Playback::Reset() {
    if (handle_ != 0U) {
        (void)Stop();
        handle_ = 0U;
    }
}

Result<AudioClip> Audio::Load(AssetId asset) const {
    micropixel_audio_clip_load_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, asset.value()};
    micropixel_audio_clip_info_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_CLIP_LOAD, &request, sizeof(request), &response,
                             sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_AUDIO_INTERFACE_MAJOR || response.clip == 0U ||
        response.reserved0 != 0U || response.format != MICROPIXEL_AUDIO_FORMAT_OGG_OPUS) {
        runtime::Panic("audio.clip.load", MICROPIXEL_STATUS_INTERNAL);
    }
    return AudioClip{response.clip};
}

Result<Playback> Audio::Play(const AudioClip& clip, PlaybackOptions options) const {
    if (!clip.valid() || options.volume_per_mille > 1000U) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_audio_playback_start_request_t request{};
    request.size = sizeof(request);
    request.flags = options.loop ? MICROPIXEL_AUDIO_PLAYBACK_LOOP : 0U;
    request.clip = clip.handle_;
    request.volume_per_mille = options.volume_per_mille;
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_START, &request, sizeof(request),
                             &response, sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("audio.playback.start", MICROPIXEL_STATUS_INTERNAL);
    }
    return Playback{response.handle};
}

Result<Playback> Audio::Play(AssetId asset, PlaybackOptions options) const {
    auto clip = Load(asset);
    if (!clip) {
        return unexpected(clip.error());
    }
    return Play(*clip, options);
}

Result<PcmStream> Audio::OpenPcmStream(const PcmStreamOptions& options) const {
    if (options.sample_rate == 0U || options.channels == 0U || options.channels > MICROPIXEL_AUDIO_PCM_MAX_CHANNELS ||
        options.capacity_frames == 0U || options.capacity_frames > MICROPIXEL_AUDIO_PCM_MAX_CAPACITY_FRAMES ||
        options.low_water_frames >= options.capacity_frames || options.volume_per_mille > 1000U) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_audio_pcm_stream_open_request_t request{};
    request.size = sizeof(request);
    request.volume_per_mille = options.volume_per_mille;
    request.sample_rate = options.sample_rate;
    request.channels = options.channels;
    request.flags = MICROPIXEL_AUDIO_PCM_STREAM_NONE;
    request.capacity_frames = options.capacity_frames;
    request.low_water_frames = options.low_water_frames;
    micropixel_audio_pcm_stream_open_response_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_PCM_STREAM_OPEN, &request, sizeof(request),
                             &response, sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.stream == 0U ||
        response.capacity_frames < options.capacity_frames) {
        runtime::Panic("audio.pcm_stream.open", MICROPIXEL_STATUS_INTERNAL);
    }
    return PcmStream{response.stream, options.sample_rate, options.channels, response.capacity_frames};
}

PcmStream::PcmStream(PcmStream&& other) noexcept
    : handle_(other.handle_),
      sample_rate_(other.sample_rate_),
      channels_(other.channels_),
      capacity_frames_(other.capacity_frames_),
      free_frames_(other.free_frames_) {
    other.handle_ = 0U;
}

PcmStream& PcmStream::operator=(PcmStream&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        sample_rate_ = other.sample_rate_;
        channels_ = other.channels_;
        capacity_frames_ = other.capacity_frames_;
        free_frames_ = other.free_frames_;
        other.handle_ = 0U;
    }
    return *this;
}

PcmStream::~PcmStream() { Reset(); }

Result<uint32_t> PcmStream::Write(const int16_t* frames, uint32_t frame_count) {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    if (frames == nullptr && frame_count != 0U) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    constexpr uint32_t kHeaderBytes = sizeof(micropixel_audio_pcm_stream_write_request_t);
    static_assert(kHeaderBytes < MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES, "PCM write header exceeds ABI request");
    const uint32_t frame_bytes = static_cast<uint32_t>(channels_) * sizeof(int16_t);
    const uint32_t frames_per_request = (MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES - kHeaderBytes) / frame_bytes;
    // Not zero-initialised: every byte sent is written by the copies below.
    alignas(4) uint8_t request[MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES];
    uint32_t written = 0U;
    // The ring only takes what fits; stop at the first short acceptance so the
    // caller sees a contiguous prefix and can resume from `written`.
    while (written < frame_count) {
        uint32_t chunk = frame_count - written;
        if (chunk > frames_per_request) {
            chunk = frames_per_request;
        }
        const uint32_t payload_bytes = chunk * frame_bytes;
        const uint32_t request_size = kHeaderBytes + payload_bytes;
        micropixel_audio_pcm_stream_write_request_t header{};
        header.size = static_cast<uint16_t>(request_size);
        header.stream = handle_;
        header.frame_count = chunk;
        CopyBytes(request, &header, sizeof(header));
        CopyBytes(request + kHeaderBytes, frames + static_cast<size_t>(written) * channels_, payload_bytes);
        micropixel_audio_pcm_stream_write_response_t response{};
        uint32_t response_size = 0U;
        const int32_t status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_PCM_STREAM_WRITE, request,
                                           request_size, &response, sizeof(response), response_size);
        if (status != MICROPIXEL_STATUS_OK) {
            return unexpected(ErrorFromStatus(status));
        }
        if (response_size < sizeof(response) || response.size < sizeof(response) || response.accepted_frames > chunk) {
            runtime::Panic("audio.pcm_stream.write", MICROPIXEL_STATUS_INTERNAL);
        }
        written += response.accepted_frames;
        free_frames_ = response.free_frames;
        if (response.accepted_frames < chunk) {
            break;
        }
    }
    return written;
}

Result<void> PcmStream::Close() {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    const int32_t status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PCM_STREAM_CLOSE, &request, sizeof(request));
    handle_ = 0U;
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

void PcmStream::Reset() {
    if (handle_ != 0U) {
        (void)Close();
    }
}

}  // namespace micropixel
