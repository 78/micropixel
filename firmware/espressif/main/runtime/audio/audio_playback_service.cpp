#include "runtime/audio/audio_playback_service.hpp"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "runtime/bundle/bundle_format.h"
#include "work/task_policy.hpp"

namespace micropixel::runtime {
namespace {

constexpr uint32_t kWorkerStackBytes = 8U * 1024U;
constexpr BaseType_t kWorkerCore = 0;
constexpr uint32_t kInputChunkBytes = 4096U;

uint32_t NextGeneration(uint32_t current) {
    uint32_t next = (current + 1U) & 0x00ffffffU;
    return next == 0U ? 1U : next;
}

}  // namespace

AudioPlaybackService::AudioPlaybackService(const micropixel_aot_package_t& package, device::AudioService& audio,
                                           EventQueue& events, int64_t clock_origin_us)
    : package_(package),
      audio_(audio),
      events_(events),
      clock_origin_us_(clock_origin_us),
      mutex_(xSemaphoreCreateMutex()),
      completions_(xQueueCreate(kMaxPlaybacks, sizeof(device::PcmCompletion))),
      worker_stopped_(xSemaphoreCreateBinary()),
      ring_storage_(static_cast<int16_t*>(
          heap_caps_calloc(kMaxPlaybacks * kRingFrames, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
    if (mutex_ == nullptr || completions_ == nullptr || worker_stopped_ == nullptr || ring_storage_ == nullptr) {
        return;
    }
    for (uint32_t index = 0U; index < kMaxPlaybacks; ++index) {
        playbacks_[index].owner = this;
        playbacks_[index].ring = ring_storage_ + index * kRingFrames;
    }
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const BaseType_t task_created = xTaskCreatePinnedToCoreWithCaps(WorkerEntry, "micropixel_opus", kWorkerStackBytes,
                                                                    this, task_policy::kAudioDecodePriority, &worker_,
                                                                    kWorkerCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t task_created = xTaskCreatePinnedToCore(WorkerEntry, "micropixel_opus", kWorkerStackBytes, this,
                                                            task_policy::kAudioDecodePriority, &worker_, kWorkerCore);
#endif
    if (task_created != pdPASS) {
        worker_ = nullptr;
        return;
    }
    audio_.BindPcmCompletionSink(OnPcmCompletion, this);
}

AudioPlaybackService::~AudioPlaybackService() {
    Shutdown();
    heap_caps_free(ring_storage_);
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
    if (completions_ != nullptr) {
        vQueueDelete(completions_);
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
    }
}

bool AudioPlaybackService::valid() const {
    return mutex_ != nullptr && completions_ != nullptr && worker_stopped_ != nullptr && ring_storage_ != nullptr &&
           worker_ != nullptr;
}

micropixel_audio_clip_handle_t AudioPlaybackService::ClipHandle(const ClipSlot& slot) const {
    const uint32_t index = static_cast<uint32_t>(&slot - clips_);
    return (slot.generation << 8U) | (index + 1U);
}

micropixel_audio_playback_handle_t AudioPlaybackService::PlaybackHandle(const PlaybackSlot& slot) const {
    const uint32_t index = static_cast<uint32_t>(&slot - playbacks_);
    return (slot.generation << 8U) | (index + 1U);
}

AudioPlaybackService::ClipSlot* AudioPlaybackService::FindClip(micropixel_audio_clip_handle_t handle) {
    const uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > kMaxClips) {
        return nullptr;
    }
    ClipSlot& slot = clips_[encoded_index - 1U];
    return slot.active && ClipHandle(slot) == handle ? &slot : nullptr;
}

AudioPlaybackService::PlaybackSlot* AudioPlaybackService::FindPlayback(micropixel_audio_playback_handle_t handle) {
    const uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > kMaxPlaybacks) {
        return nullptr;
    }
    PlaybackSlot& slot = playbacks_[encoded_index - 1U];
    return slot.active && PlaybackHandle(slot) == handle ? &slot : nullptr;
}

ServiceResult<micropixel_audio_clip_info_t> AudioPlaybackService::LoadClip(uint32_t asset_id) {
    if (!valid()) {
        return FailService<micropixel_audio_clip_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    if (stopping_.load(std::memory_order_acquire)) {
        return FailService<micropixel_audio_clip_info_t>(MICROPIXEL_STATUS_CLOSED);
    }
    if (asset_id == 0U) {
        return FailService<micropixel_audio_clip_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_bundle_asset_view_t asset{};
    if (!micropixel_bundle_find_asset(&package_, asset_id, &asset)) {
        return FailService<micropixel_audio_clip_info_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (asset.format != MICROPIXEL_BUNDLE_FORMAT_OGG_OPUS || asset.data == nullptr || asset.size == 0U) {
        return FailService<micropixel_audio_clip_info_t>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<micropixel_audio_clip_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    ClipSlot* selected = nullptr;
    for (ClipSlot& slot : clips_) {
        if (!slot.active) {
            selected = &slot;
            break;
        }
    }
    if (selected == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<micropixel_audio_clip_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    selected->generation = NextGeneration(selected->generation);
    selected->asset = asset;
    selected->asset_id = asset_id;
    selected->playback_refs = 0U;
    selected->active = true;
    selected->guest_owned = true;
    micropixel_audio_clip_info_t info{};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_AUDIO_INTERFACE_MINOR;
    info.clip = ClipHandle(*selected);
    info.format = MICROPIXEL_AUDIO_FORMAT_OGG_OPUS;
    (void)xSemaphoreGive(mutex_);
    return info;
}

ServiceResult<void> AudioPlaybackService::ReleaseClip(micropixel_audio_clip_handle_t clip) {
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    ClipSlot* slot = FindClip(clip);
    if (slot == nullptr || !slot->guest_owned) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    slot->guest_owned = false;
    if (slot->playback_refs == 0U) {
        slot->active = false;
        slot->asset = {};
    }
    (void)xSemaphoreGive(mutex_);
    return {};
}

void AudioPlaybackService::Fill(PlaybackSlot& slot) {
    if (!slot.active || !slot.decoder.has_value() || slot.clip == nullptr || slot.eof.load(std::memory_order_acquire)) {
        return;
    }
    int16_t decoded[kDecodeFrames]{};
    while (slot.write_position.load(std::memory_order_relaxed) - slot.read_position.load(std::memory_order_acquire) <=
           kRingFrames - kDecodeFrames) {
        const uint32_t remaining = slot.clip->asset.size - slot.input_offset;
        if (remaining == 0U) {
            if (slot.loop) {
                slot.decoder->reset();
                slot.input_offset = 0U;
                continue;
            }
            slot.eof.store(true, std::memory_order_release);
            return;
        }
        size_t consumed = 0U;
        size_t samples = 0U;
        const uint32_t input_size = std::min(remaining, kInputChunkBytes);
        const micro_opus::OggOpusResult result =
            slot.decoder->decode(slot.clip->asset.data + slot.input_offset, input_size,
                                 reinterpret_cast<uint8_t*>(decoded), sizeof(decoded), consumed, samples);
        if (result != micro_opus::OGG_OPUS_OK || consumed > input_size || samples > kDecodeFrames) {
            slot.terminal_status.store(MICROPIXEL_STATUS_INTERNAL, std::memory_order_release);
            slot.eof.store(true, std::memory_order_release);
            return;
        }
        slot.input_offset += static_cast<uint32_t>(consumed);
        if (samples == 0U) {
            if (consumed == 0U) {
                slot.terminal_status.store(MICROPIXEL_STATUS_INTERNAL, std::memory_order_release);
                slot.eof.store(true, std::memory_order_release);
                return;
            }
            continue;
        }
        const uint32_t write = slot.write_position.load(std::memory_order_relaxed);
        const uint32_t first = std::min(static_cast<uint32_t>(samples), kRingFrames - (write % kRingFrames));
        std::memcpy(slot.ring + (write % kRingFrames), decoded, first * sizeof(int16_t));
        std::memcpy(slot.ring, decoded + first, (samples - first) * sizeof(int16_t));
        slot.write_position.store(write + static_cast<uint32_t>(samples), std::memory_order_release);
    }
}

ServiceResult<micropixel_audio_playback_handle_t> AudioPlaybackService::Start(
    const micropixel_audio_playback_start_request_t& request) {
    if ((request.flags & ~MICROPIXEL_AUDIO_PLAYBACK_LOOP) != 0U || request.volume_per_mille > 1000U ||
        request.reserved0 != 0U || request.reserved1 != 0U) {
        return FailService<micropixel_audio_playback_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (stopping_.load(std::memory_order_acquire)) {
        return FailService<micropixel_audio_playback_handle_t>(MICROPIXEL_STATUS_CLOSED);
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<micropixel_audio_playback_handle_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    ClipSlot* clip = FindClip(request.clip);
    PlaybackSlot* selected = nullptr;
    for (PlaybackSlot& slot : playbacks_) {
        if (!slot.active) {
            selected = &slot;
            break;
        }
    }
    if (clip == nullptr || !clip->guest_owned) {
        (void)xSemaphoreGive(mutex_);
        return FailService<micropixel_audio_playback_handle_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (selected == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<micropixel_audio_playback_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    selected->generation = NextGeneration(selected->generation);
    selected->decoder.emplace(true, 16000U, 1U);
    selected->clip = clip;
    selected->stream = 0U;
    selected->input_offset = 0U;
    selected->read_position.store(0U, std::memory_order_relaxed);
    selected->write_position.store(0U, std::memory_order_relaxed);
    selected->eof.store(false, std::memory_order_relaxed);
    selected->terminal_status.store(MICROPIXEL_STATUS_OK, std::memory_order_relaxed);
    selected->state = MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING;
    selected->loop = (request.flags & MICROPIXEL_AUDIO_PLAYBACK_LOOP) != 0U;
    selected->active = true;
    selected->pinned = true;
    ++clip->playback_refs;
    Fill(*selected);
    if (selected->terminal_status.load(std::memory_order_acquire) != MICROPIXEL_STATUS_OK &&
        selected->write_position.load(std::memory_order_acquire) == 0U) {
        ClearPlayback(*selected);
        (void)xSemaphoreGive(mutex_);
        return FailService<micropixel_audio_playback_handle_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    const micropixel_audio_playback_handle_t handle = PlaybackHandle(*selected);
    auto stream = audio_.StartPcm(device::PcmSource{ReadPcm, selected}, handle, request.volume_per_mille);
    if (!stream) {
        const int32_t status = stream.error().status;
        ClearPlayback(*selected);
        (void)xSemaphoreGive(mutex_);
        return FailService<micropixel_audio_playback_handle_t>(status);
    }
    selected->stream = *stream;
    (void)xSemaphoreGive(mutex_);
    if (worker_ != nullptr) {
        xTaskNotifyGive(worker_);
    }
    return handle;
}

ServiceResult<void> AudioPlaybackService::Pause(micropixel_audio_playback_handle_t playback) {
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    PlaybackSlot* slot = FindPlayback(playback);
    if (slot == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_FINISHED ||
        slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_FAILED) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(MICROPIXEL_STATUS_CLOSED);
    }
    if (slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_PAUSED) {
        (void)xSemaphoreGive(mutex_);
        return {};
    }
    auto result = audio_.PausePcm(slot->stream);
    if (result) {
        slot->state = MICROPIXEL_AUDIO_PLAYBACK_STATE_PAUSED;
    }
    (void)xSemaphoreGive(mutex_);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> AudioPlaybackService::Resume(micropixel_audio_playback_handle_t playback) {
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    PlaybackSlot* slot = FindPlayback(playback);
    if (slot == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_FINISHED ||
        slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_FAILED) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(MICROPIXEL_STATUS_CLOSED);
    }
    if (slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING) {
        (void)xSemaphoreGive(mutex_);
        return {};
    }
    auto result = audio_.ResumePcm(slot->stream);
    if (result) {
        slot->state = MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING;
    }
    (void)xSemaphoreGive(mutex_);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> AudioPlaybackService::SetVolume(micropixel_audio_playback_handle_t playback,
                                                    uint16_t volume_per_mille) {
    if (volume_per_mille > 1000U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    PlaybackSlot* slot = FindPlayback(playback);
    if (slot == nullptr || slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_FINISHED ||
        slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_FAILED) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(slot == nullptr ? MICROPIXEL_STATUS_NOT_FOUND : MICROPIXEL_STATUS_CLOSED);
    }
    auto result = audio_.SetPcmVolume(slot->stream, volume_per_mille);
    (void)xSemaphoreGive(mutex_);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

void AudioPlaybackService::ReleasePin(PlaybackSlot& slot) {
    if (!slot.pinned || slot.clip == nullptr) {
        return;
    }
    ClipSlot* clip = slot.clip;
    if (clip->playback_refs > 0U) {
        --clip->playback_refs;
    }
    if (clip->playback_refs == 0U && !clip->guest_owned) {
        clip->active = false;
        clip->asset = {};
    }
    slot.pinned = false;
}

void AudioPlaybackService::ClearPlayback(PlaybackSlot& slot) {
    ReleasePin(slot);
    slot.decoder.reset();
    slot.clip = nullptr;
    slot.stream = 0U;
    slot.active = false;
    slot.state = 0U;
    slot.eof.store(true, std::memory_order_release);
}

ServiceResult<void> AudioPlaybackService::Stop(micropixel_audio_playback_handle_t playback) {
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    PlaybackSlot* slot = FindPlayback(playback);
    if (slot == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING ||
        slot->state == MICROPIXEL_AUDIO_PLAYBACK_STATE_PAUSED) {
        (void)audio_.StopPcm(slot->stream);
    }
    ClearPlayback(*slot);
    (void)xSemaphoreGive(mutex_);
    return {};
}

ServiceResult<micropixel_audio_playback_state_response_t> AudioPlaybackService::State(
    micropixel_audio_playback_handle_t playback) {
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<micropixel_audio_playback_state_response_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    PlaybackSlot* slot = FindPlayback(playback);
    if (slot == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<micropixel_audio_playback_state_response_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    micropixel_audio_playback_state_response_t response{
        .size = sizeof(response),
        .state = slot->state,
        .playback = playback,
    };
    (void)xSemaphoreGive(mutex_);
    return response;
}

ServiceResult<void> AudioPlaybackService::StopAll() {
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    for (PlaybackSlot& slot : playbacks_) {
        if (slot.active) {
            if (slot.state == MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING ||
                slot.state == MICROPIXEL_AUDIO_PLAYBACK_STATE_PAUSED) {
                (void)audio_.StopPcm(slot.stream);
            }
            ClearPlayback(slot);
        }
    }
    (void)xSemaphoreGive(mutex_);
    auto result = audio_.StopAll();
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

device::PcmReadResult AudioPlaybackService::ReadPcm(void* context, int16_t* samples, uint32_t capacity) {
    auto* slot = static_cast<PlaybackSlot*>(context);
    if (slot == nullptr || samples == nullptr || capacity == 0U) {
        return {.finished = true};
    }
    const uint32_t read = slot->read_position.load(std::memory_order_relaxed);
    const uint32_t write = slot->write_position.load(std::memory_order_acquire);
    const uint32_t count = std::min(capacity, write - read);
    const uint32_t first = std::min(count, kRingFrames - (read % kRingFrames));
    std::memcpy(samples, slot->ring + (read % kRingFrames), first * sizeof(int16_t));
    std::memcpy(samples + first, slot->ring, (count - first) * sizeof(int16_t));
    slot->read_position.store(read + count, std::memory_order_release);
    if (slot->owner != nullptr && slot->owner->worker_ != nullptr) {
        xTaskNotifyGive(slot->owner->worker_);
    }
    return {
        .frames = count,
        .finished = slot->eof.load(std::memory_order_acquire) && read + count == write,
    };
}

void AudioPlaybackService::OnPcmCompletion(void* context, const device::PcmCompletion& completion) {
    auto* service = static_cast<AudioPlaybackService*>(context);
    if (service == nullptr || service->completions_ == nullptr || service->stopping_.load(std::memory_order_acquire)) {
        return;
    }
    (void)xQueueSend(service->completions_, &completion, 0U);
    if (service->worker_ != nullptr) {
        xTaskNotifyGive(service->worker_);
    }
}

void AudioPlaybackService::FinishPlayback(uint32_t token, int32_t status) {
    micropixel_event_t event{};
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    PlaybackSlot* slot = FindPlayback(token);
    if (slot == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return;
    }
    const int32_t decode_status = slot->terminal_status.load(std::memory_order_acquire);
    if (status == MICROPIXEL_STATUS_OK && decode_status != MICROPIXEL_STATUS_OK) {
        status = decode_status;
    }
    slot->state = status == MICROPIXEL_STATUS_OK ? MICROPIXEL_AUDIO_PLAYBACK_STATE_FINISHED
                                                 : MICROPIXEL_AUDIO_PLAYBACK_STATE_FAILED;
    slot->decoder.reset();
    ReleasePin(*slot);
    slot->clip = nullptr;
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_AUDIO_EVENT_PLAYBACK_FINISHED;
    event.service_id = MICROPIXEL_SERVICE_AUDIO;
    event.source = token;
    event.timestamp_us = static_cast<uint64_t>(esp_timer_get_time() - clock_origin_us_);
    event.sequence = ++event_sequence_;
    event.status = status;
    micropixel_audio_event_payload_t payload{};
    payload.playback = token;
    std::memcpy(event.payload, &payload, sizeof(payload));
    (void)xSemaphoreGive(mutex_);
    (void)events_.PushRequired(event);
}

void AudioPlaybackService::WorkerEntry(void* argument) { static_cast<AudioPlaybackService*>(argument)->WorkerLoop(); }

void AudioPlaybackService::WorkerLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (stopping_.load(std::memory_order_acquire)) {
            break;
        }
        device::PcmCompletion completion{};
        while (xQueueReceive(completions_, &completion, 0U) == pdTRUE) {
            FinishPlayback(completion.token, completion.status);
        }
        if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
            for (PlaybackSlot& slot : playbacks_) {
                if (slot.active && slot.state == MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING) {
                    Fill(slot);
                }
            }
            (void)xSemaphoreGive(mutex_);
        }
    }
    (void)xSemaphoreGive(worker_stopped_);
    // Shutdown owns deletion so a task created with explicit PSRAM stack caps
    // is destroyed through the matching API. Self-deleting with vTaskDelete()
    // only releases the FreeRTOS task object; it leaks the separately allocated
    // WithCaps stack and TCB on every AppSession.
    vTaskSuspend(nullptr);
}

void AudioPlaybackService::Shutdown() {
    if (shutdown_complete_) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    audio_.UnbindPcmCompletionSink(this);
    if (mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
        for (PlaybackSlot& slot : playbacks_) {
            if (slot.active) {
                if (slot.stream != 0U) {
                    (void)audio_.StopPcm(slot.stream);
                }
                ClearPlayback(slot);
            }
        }
        (void)xSemaphoreGive(mutex_);
    }
    if (worker_ != nullptr) {
        xTaskNotifyGive(worker_);
        (void)xSemaphoreTake(worker_stopped_, portMAX_DELAY);
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
        vTaskDeleteWithCaps(worker_);
#else
        vTaskDelete(worker_);
#endif
        worker_ = nullptr;
    }
    for (ClipSlot& clip : clips_) {
        clip.active = false;
        clip.asset = {};
    }
    shutdown_complete_ = true;
}

}  // namespace micropixel::runtime
