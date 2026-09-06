#ifndef MICROPIXEL_RUNTIME_AUDIO_PCM_STREAM_SERVICE_HPP
#define MICROPIXEL_RUNTIME_AUDIO_PCM_STREAM_SERVICE_HPP

#include <atomic>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/device_services.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "runtime/audio/linear_upsampler.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

// Audio 1.2 Guest PCM stream: the Guest pushes interleaved int16 frames into
// a Host ring buffer and the mixer pulls them at the board's mix rate.
//
// One stream per session. The ring lives in PSRAM and is sized by the Guest
// (clamped by the Host); WRITE accepts as many leading frames as fit and
// returns the count so the Guest experiences backpressure instead of
// truncation. Reads happen on the audio task through the device PcmSource
// callback and never block; underrun plays silence rather than finishing the
// stream. When the buffered amount drops to the Guest's low-water mark the
// audio task posts PCM_STREAM_LOW_WATER once; the next successful WRITE
// re-arms it.
class PcmStreamService final {
   public:
    static constexpr uint32_t kMaxStreams = 1U;

    PcmStreamService(device::AudioService& audio, EventQueue& events, int64_t clock_origin_us);
    PcmStreamService(const PcmStreamService&) = delete;
    PcmStreamService& operator=(const PcmStreamService&) = delete;
    ~PcmStreamService();

    [[nodiscard]] bool valid() const { return mutex_ != nullptr; }  // NOLINT(readability-identifier-naming)

    [[nodiscard]] ServiceResult<micropixel_audio_pcm_stream_open_response_t> Open(
        const micropixel_audio_pcm_stream_open_request_t& request);
    // `samples` is the request payload of `payload_bytes`; it must hold exactly
    // frame_count * channels interleaved int16 values.
    [[nodiscard]] ServiceResult<micropixel_audio_pcm_stream_write_response_t> Write(
        const micropixel_audio_pcm_stream_write_request_t& request, const int16_t* samples, uint32_t payload_bytes);
    [[nodiscard]] ServiceResult<void> Close(micropixel_audio_pcm_stream_handle_t stream);

    // STOP_ALL and session teardown close every stream (streams survive App
    // suspend; only the mixer voice pauses).
    void CloseAll();
    void Shutdown();

   private:
    static constexpr uint32_t kMinCapacityFrames = 256U;

    struct Stream final {
        PcmStreamService* owner{};
        // Written by the Guest task under mutex_, read by the audio task.
        int16_t* ring{};
        uint32_t capacity_frames{};
        uint32_t low_water_frames{};
        uint32_t upsample_factor{1U};
        std::atomic<uint32_t> read_position{};
        std::atomic<uint32_t> write_position{};
        // Set by WRITE, cleared by the audio task once it posted the event.
        std::atomic<bool> low_water_armed{};
        micropixel_audio_pcm_stream_handle_t handle{};
        device::PcmStreamHandle device_stream{};
        uint16_t channels{};
        bool open{};
        // Audio-task-only.
        LinearUpsampler upsampler{};
    };

    static device::PcmReadResult ReadPcm(void* context, int16_t* samples, uint32_t capacity);
    void PostLowWater(Stream& stream, uint32_t free_frames);
    void CloseLocked(Stream& stream);
    [[nodiscard]] Stream* FindLocked(micropixel_audio_pcm_stream_handle_t handle);

    device::AudioService& audio_;
    EventQueue& events_;
    int64_t clock_origin_us_{};
    SemaphoreHandle_t mutex_{};
    uint32_t mix_rate_{};
    uint32_t next_handle_{1U};
    std::atomic<uint32_t> event_sequence_{0U};
    Stream streams_[kMaxStreams]{};
};

}  // namespace micropixel::runtime

#endif
