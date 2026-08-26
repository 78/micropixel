#ifndef MICROPIXEL_RUNTIME_AUDIO_AUDIO_PLAYBACK_SERVICE_HPP
#define MICROPIXEL_RUNTIME_AUDIO_AUDIO_PLAYBACK_SERVICE_HPP

#include <atomic>
#include <cstdint>
#include <optional>

#include "device/device_services.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "micro_opus/ogg_opus_decoder.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/event_queue.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class AudioPlaybackService final {
   public:
    AudioPlaybackService(const micropixel_aot_package_t& package, device::AudioService& audio, EventQueue& events,
                         int64_t clock_origin_us);
    AudioPlaybackService(const AudioPlaybackService&) = delete;
    AudioPlaybackService& operator=(const AudioPlaybackService&) = delete;
    ~AudioPlaybackService();

    [[nodiscard]] bool valid() const;  // NOLINT(readability-identifier-naming)
    [[nodiscard]] ServiceResult<micropixel_audio_clip_info_t> LoadClip(uint32_t asset_id);
    [[nodiscard]] ServiceResult<void> ReleaseClip(micropixel_audio_clip_handle_t clip);
    [[nodiscard]] ServiceResult<micropixel_audio_playback_handle_t> Start(
        const micropixel_audio_playback_start_request_t& request);
    [[nodiscard]] ServiceResult<void> Pause(micropixel_audio_playback_handle_t playback);
    [[nodiscard]] ServiceResult<void> Resume(micropixel_audio_playback_handle_t playback);
    [[nodiscard]] ServiceResult<void> SetVolume(micropixel_audio_playback_handle_t playback, uint16_t volume_per_mille);
    [[nodiscard]] ServiceResult<void> Stop(micropixel_audio_playback_handle_t playback);
    [[nodiscard]] ServiceResult<micropixel_audio_playback_state_response_t> State(
        micropixel_audio_playback_handle_t playback);
    [[nodiscard]] ServiceResult<void> StopAll();
    void Shutdown();

   private:
    static constexpr uint32_t kMaxClips = 16U;
    static constexpr uint32_t kMaxPlaybacks = 2U;
    static constexpr uint32_t kRingFrames = 4096U;
    static constexpr uint32_t kDecodeFrames = 1920U;

    struct ClipSlot final {
        micropixel_bundle_asset_view_t asset{};
        uint32_t asset_id{};
        uint32_t generation{};
        uint16_t playback_refs{};
        bool active{};
        bool guest_owned{};
    };

    struct PlaybackSlot final {
        AudioPlaybackService* owner{};
        std::optional<micro_opus::OggOpusDecoder> decoder;
        int16_t* ring{};
        ClipSlot* clip{};
        device::PcmStreamHandle stream{};
        uint32_t generation{};
        uint32_t input_offset{};
        std::atomic<uint32_t> read_position{};
        std::atomic<uint32_t> write_position{};
        std::atomic<bool> eof{};
        std::atomic<int32_t> terminal_status{MICROPIXEL_STATUS_OK};
        uint16_t state{};
        bool loop{};
        bool active{};
        bool pinned{};
    };

    static void WorkerEntry(void* argument);
    void WorkerLoop();
    void Fill(PlaybackSlot& slot);
    static device::PcmReadResult ReadPcm(void* context, int16_t* samples, uint32_t capacity);
    static void OnPcmCompletion(void* context, const device::PcmCompletion& completion);
    void FinishPlayback(uint32_t token, int32_t status);
    void ReleasePin(PlaybackSlot& slot);
    void ClearPlayback(PlaybackSlot& slot);
    [[nodiscard]] ClipSlot* FindClip(micropixel_audio_clip_handle_t handle);
    [[nodiscard]] PlaybackSlot* FindPlayback(micropixel_audio_playback_handle_t handle);
    [[nodiscard]] micropixel_audio_clip_handle_t ClipHandle(const ClipSlot& slot) const;
    [[nodiscard]] micropixel_audio_playback_handle_t PlaybackHandle(const PlaybackSlot& slot) const;

    micropixel_aot_package_t package_{};
    device::AudioService& audio_;
    EventQueue& events_;
    int64_t clock_origin_us_{};
    SemaphoreHandle_t mutex_{};
    QueueHandle_t completions_{};
    SemaphoreHandle_t worker_stopped_{};
    TaskHandle_t worker_{};
    int16_t* ring_storage_{};
    ClipSlot clips_[kMaxClips]{};
    PlaybackSlot playbacks_[kMaxPlaybacks]{};
    std::atomic<bool> stopping_{};
    uint32_t event_sequence_{};
    bool shutdown_complete_{};
};

}  // namespace micropixel::runtime

#endif
