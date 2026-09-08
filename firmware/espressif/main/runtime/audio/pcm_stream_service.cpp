#include "runtime/audio/pcm_stream_service.hpp"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::runtime {
namespace {

constexpr const char* kTag = "pcm_stream";
// Completion tokens with a zero low byte never resolve to an Opus playback in
// the shared completion sink, and this stream never completes on its own.
constexpr uint32_t kCompletionToken = 0U;

}  // namespace

PcmStreamService::PcmStreamService(device::AudioService& audio, EventQueue& events, int64_t clock_origin_us)
    : audio_(audio), events_(events), clock_origin_us_(clock_origin_us), mutex_(xSemaphoreCreateMutex()) {
    if (auto info = audio_.GetInfo(); info) {
        mix_rate_ = info->sample_rate;
    }
    for (Stream& stream : streams_) {
        stream.owner = this;
    }
}

PcmStreamService::~PcmStreamService() {
    Shutdown();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

PcmStreamService::Stream* PcmStreamService::FindLocked(micropixel_audio_pcm_stream_handle_t handle) {
    for (Stream& stream : streams_) {
        if (stream.open && stream.handle == handle) {
            return &stream;
        }
    }
    return nullptr;
}

ServiceResult<micropixel_audio_pcm_stream_open_response_t> PcmStreamService::Open(
    const micropixel_audio_pcm_stream_open_request_t& request) {
    using Response = micropixel_audio_pcm_stream_open_response_t;
    if (request.size != sizeof(request) || request.flags != MICROPIXEL_AUDIO_PCM_STREAM_NONE ||
        request.volume_per_mille > 1000U || request.channels == 0U ||
        request.channels > MICROPIXEL_AUDIO_PCM_MAX_CHANNELS || request.sample_rate == 0U ||
        request.capacity_frames == 0U || request.capacity_frames > kMaxCapacityFrames ||
        request.low_water_frames >= request.capacity_frames) {
        return FailService<Response>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (!valid() || mix_rate_ == 0U) {
        return FailService<Response>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    // Only the mix rate or an integer divisor of it; anything else would need
    // a real resampler on the audio task.
    if (request.sample_rate > mix_rate_ || mix_rate_ % request.sample_rate != 0U) {
        return FailService<Response>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<Response>(MICROPIXEL_STATUS_INTERNAL);
    }
    Stream* selected = nullptr;
    for (Stream& stream : streams_) {
        if (!stream.open) {
            selected = &stream;
            break;
        }
    }
    if (selected == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<Response>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    // Power of two so the monotonic read/write positions keep indexing the
    // ring correctly when they wrap at 2^32 (2^32 mod capacity == 0).
    const uint32_t capacity = std::bit_ceil(std::max(request.capacity_frames, kMinCapacityFrames));
    auto* ring =
        static_cast<int16_t*>(heap_caps_calloc(capacity, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (ring == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<Response>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    selected->ring = ring;
    selected->capacity_frames = capacity;
    selected->low_water_frames = request.low_water_frames;
    selected->upsample_factor = mix_rate_ / request.sample_rate;
    selected->channels = request.channels;
    selected->read_position.store(0U, std::memory_order_relaxed);
    selected->write_position.store(0U, std::memory_order_relaxed);
    selected->low_water_armed.store(false, std::memory_order_relaxed);
    selected->upsampler.Reset(selected->upsample_factor);
    selected->handle = next_handle_++;
    if (next_handle_ == 0U) {
        next_handle_ = 1U;
    }
    selected->open = true;
    auto device_stream =
        audio_.StartPcm(device::PcmSource{ReadPcm, selected}, kCompletionToken, request.volume_per_mille);
    if (!device_stream) {
        const int32_t status = device_stream.error().status;
        selected->open = false;
        heap_caps_free(selected->ring);
        selected->ring = nullptr;
        (void)xSemaphoreGive(mutex_);
        return FailService<Response>(status);
    }
    selected->device_stream = *device_stream;
    Response response{};
    response.size = sizeof(response);
    response.stream_handle = selected->handle;
    response.capacity_frames = capacity;
    ESP_LOGI(kTag, "stream %" PRIu32 " open: %" PRIu32 " Hz x%" PRIu32 " ch=%u capacity=%" PRIu32 " low_water=%" PRIu32,
             selected->handle, request.sample_rate, selected->upsample_factor, request.channels, capacity,
             request.low_water_frames);
    (void)xSemaphoreGive(mutex_);
    return response;
}

ServiceResult<micropixel_audio_pcm_stream_write_response_t> PcmStreamService::Write(
    const micropixel_audio_pcm_stream_write_request_t& request, const int16_t* samples, uint32_t payload_bytes) {
    using Response = micropixel_audio_pcm_stream_write_response_t;
    if (request.size != sizeof(request) + payload_bytes || request.reserved0 != 0U || request.stream_handle == 0U ||
        request.size > MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES || (payload_bytes != 0U && samples == nullptr) ||
        (payload_bytes % sizeof(int16_t)) != 0U) {
        return FailService<Response>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (!valid()) {
        return FailService<Response>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<Response>(MICROPIXEL_STATUS_INTERNAL);
    }
    Stream* stream = FindLocked(request.stream_handle);
    if (stream == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<Response>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (static_cast<uint64_t>(request.frame_count) * stream->channels * sizeof(int16_t) != payload_bytes) {
        (void)xSemaphoreGive(mutex_);
        return FailService<Response>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t read = stream->read_position.load(std::memory_order_acquire);
    const uint32_t write = stream->write_position.load(std::memory_order_relaxed);
    const uint32_t free_before = stream->capacity_frames - (write - read);
    const uint32_t accepted = std::min(request.frame_count, free_before);
    if (stream->channels == 1U) {
        const uint32_t first = std::min(accepted, stream->capacity_frames - (write % stream->capacity_frames));
        std::memcpy(stream->ring + (write % stream->capacity_frames), samples, first * sizeof(int16_t));
        std::memcpy(stream->ring, samples + first, (accepted - first) * sizeof(int16_t));
    } else {
        // The mixer is mono: average the channel pair. memcpy keeps this safe
        // for Guest payloads that are not naturally aligned.
        for (uint32_t frame = 0U; frame < accepted; ++frame) {
            int16_t pair[2]{};
            std::memcpy(pair, samples + frame * 2U, sizeof(pair));
            stream->ring[(write + frame) % stream->capacity_frames] =
                static_cast<int16_t>((static_cast<int32_t>(pair[0]) + static_cast<int32_t>(pair[1])) / 2);
        }
    }
    stream->write_position.store(write + accepted, std::memory_order_release);
    if (accepted != 0U) {
        stream->low_water_armed.store(stream->low_water_frames != 0U, std::memory_order_release);
    }
    Response response{};
    response.size = sizeof(response);
    response.accepted_frames = accepted;
    response.free_frames = free_before - accepted;
    (void)xSemaphoreGive(mutex_);
    return response;
}

void PcmStreamService::CloseLocked(Stream& stream) {
    if (!stream.open) {
        return;
    }
    // StopPcm serializes against the mixer, so no ReadPcm is in flight once it
    // returns and the ring can go away.
    (void)audio_.StopPcm(stream.device_stream);
    stream.open = false;
    stream.device_stream = 0U;
    heap_caps_free(stream.ring);
    stream.ring = nullptr;
    stream.capacity_frames = 0U;
    ESP_LOGI(kTag, "stream %" PRIu32 " closed", stream.handle);
}

ServiceResult<void> PcmStreamService::Close(micropixel_audio_pcm_stream_handle_t handle) {
    if (!valid()) {
        return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    if (xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Stream* stream = FindLocked(handle);
    if (stream == nullptr) {
        (void)xSemaphoreGive(mutex_);
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    CloseLocked(*stream);
    (void)xSemaphoreGive(mutex_);
    return {};
}

void PcmStreamService::CloseAll() {
    if (!valid() || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
        return;
    }
    for (Stream& stream : streams_) {
        CloseLocked(stream);
    }
    (void)xSemaphoreGive(mutex_);
}

void PcmStreamService::Shutdown() { CloseAll(); }

void PcmStreamService::PostLowWater(Stream& stream, uint32_t free_frames) {
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_AUDIO_EVENT_PCM_STREAM_LOW_WATER;
    event.service_id = MICROPIXEL_SERVICE_AUDIO;
    event.source = stream.handle;
    event.timestamp_us = static_cast<uint64_t>(esp_timer_get_time() - clock_origin_us_);
    event.sequence = event_sequence_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    event.status = MICROPIXEL_STATUS_OK;
    micropixel_audio_pcm_event_payload_t payload{};
    payload.stream_handle = stream.handle;
    payload.free_frames = free_frames;
    std::memcpy(event.payload, &payload, sizeof(payload));
    // Advisory: when the queue is full the stream stays armed and the next
    // chunk tries again, so the Guest still hears about the crossing.
    if (events_.PushAdvisory(event)) {
        stream.low_water_armed.store(false, std::memory_order_release);
    }
}

device::PcmReadResult PcmStreamService::ReadPcm(void* context, int16_t* samples, uint32_t capacity) {
    auto* stream = static_cast<Stream*>(context);
    if (stream == nullptr || samples == nullptr || capacity == 0U || stream->ring == nullptr) {
        return {};
    }
    const uint32_t read = stream->read_position.load(std::memory_order_relaxed);
    const uint32_t write = stream->write_position.load(std::memory_order_acquire);
    const uint32_t ring_capacity = stream->capacity_frames;
    uint32_t consumed = 0U;
    uint32_t produced = 0U;
    if (stream->upsample_factor == 1U) {
        consumed = std::min(capacity, write - read);
        const uint32_t first = std::min(consumed, ring_capacity - (read % ring_capacity));
        std::memcpy(samples, stream->ring + (read % ring_capacity), first * sizeof(int16_t));
        std::memcpy(samples + first, stream->ring, (consumed - first) * sizeof(int16_t));
        produced = consumed;
    } else {
        produced = stream->upsampler.Produce(samples, capacity, [&](int16_t& sample) {
            if (read + consumed == write) {
                return false;
            }
            sample = stream->ring[(read + consumed) % ring_capacity];
            ++consumed;
            return true;
        });
    }
    stream->read_position.store(read + consumed, std::memory_order_release);
    const uint32_t buffered = write - (read + consumed);
    if (buffered <= stream->low_water_frames && stream->low_water_armed.load(std::memory_order_acquire)) {
        stream->owner->PostLowWater(*stream, ring_capacity - buffered);
    }
    // Underrun (produced < capacity) mixes as silence; the stream only ends on Close.
    return {.frames = produced, .finished = false};
}

}  // namespace micropixel::runtime
