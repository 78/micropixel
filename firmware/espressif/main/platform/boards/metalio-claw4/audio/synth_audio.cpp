#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/boards/metalio-claw4/audio/audio_backend.hpp"
#include "platform/boards/metalio-claw4/audio/i2s_audio_sink.hpp"
#include "platform/boards/metalio-claw4/perceptual_control.hpp"
#include "platform/common/audio/synth_mixer.hpp"
#include "platform/common/i2c_executor.hpp"
#include "task_policy.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_audio";
constexpr uint32_t kFramesPerChunk = 128U;
constexpr uint32_t kMaxPcmStreams = 2U;
constexpr uint32_t kAudioTaskStackSize = 4096U;
constexpr BaseType_t kAudioTaskCore = 0;
constexpr uint32_t kMetalioClaw4AudioSampleRate = 16000U;
constexpr uint32_t kAudioIdleGraceMs = 10000U;
constexpr uint32_t kAudioIdleGraceChunks =
    (kAudioIdleGraceMs * kMetalioClaw4AudioSampleRate + kFramesPerChunk * 1000U - 1U) / (kFramesPerChunk * 1000U);

enum class BackendState : uint8_t {
    kStopped,
    kInitializing,
    kReady,
    kFailed,
};

struct PcmVoice final {
    device::PcmSource source{};
    uint32_t token{};
    uint32_t generation{};
    uint16_t volume_per_mille{};
    bool active{};
    bool paused{};
};

constexpr uint32_t kMaxVoices = 8U;

struct AudioBackendState final {
    std::atomic<BackendState> backend_state{BackendState::kStopped};
    std::atomic<bool> suspended{};
    std::atomic<uint16_t> master_volume_per_ten_thousand{
        static_cast<uint16_t>(metalio_claw4::PerceptualVolumeOutputPerTenThousand(70U))};
    std::atomic<TaskHandle_t> task{};
    SemaphoreHandle_t voices_mutex{};
    common::audio::SynthVoice voices[kMaxVoices]{};
    PcmVoice pcm_voices[kMaxPcmStreams]{};
    device::PcmCompletionSink pcm_completion_sink{};
    void* pcm_completion_context{};
    I2sAudioSink sink{};
    int16_t sine_table[common::audio::kSynthSineTableSize]{};
};

AudioBackendState& State() {
    static AudioBackendState state;
    return state;
}

constexpr device::PcmStreamHandle PcmHandle(uint32_t index, uint32_t generation) {
    return (generation << 8U) | (index + 1U);
}

PcmVoice* FindPcmVoiceLocked(device::PcmStreamHandle handle) {
    const uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > kMaxPcmStreams) {
        return nullptr;
    }
    PcmVoice& voice = State().pcm_voices[encoded_index - 1U];
    return voice.active && PcmHandle(encoded_index - 1U, voice.generation) == handle ? &voice : nullptr;
}

struct AudioChunkState final {
    bool rendered_audio{};
    bool active_after{};
    uint32_t completion_count{};
    device::PcmCompletion completions[kMaxPcmStreams]{};
};

bool HasPlayableVoiceLocked() {
    if (State().suspended.load(std::memory_order_acquire)) {
        return false;
    }
    for (const common::audio::SynthVoice& voice : State().voices) {
        if (voice.active) {
            return true;
        }
    }
    for (const PcmVoice& voice : State().pcm_voices) {
        if (voice.active && !voice.paused) {
            return true;
        }
    }
    return false;
}

bool HasPlayableVoice() {
    if (xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool playable = HasPlayableVoiceLocked();
    (void)xSemaphoreGive(State().voices_mutex);
    return playable;
}

AudioChunkState FillAudioChunk(int32_t* frames) {
    std::memset(frames, 0, kFramesPerChunk * 2U * sizeof(*frames));
    if (xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return {};
    }
    AudioChunkState chunk{};
    if (!State().suspended.load(std::memory_order_acquire)) {
        int16_t pcm_samples[kMaxPcmStreams][kFramesPerChunk]{};
        uint32_t pcm_frames[kMaxPcmStreams]{};
        uint16_t pcm_volumes[kMaxPcmStreams]{};
        for (uint32_t index = 0U; index < kMaxPcmStreams; ++index) {
            PcmVoice& voice = State().pcm_voices[index];
            if (!voice.active || voice.paused || voice.source.read == nullptr) {
                continue;
            }
            const device::PcmReadResult read =
                voice.source.read(voice.source.context, pcm_samples[index], kFramesPerChunk);
            if (read.frames > kFramesPerChunk) {
                chunk.completions[chunk.completion_count++] = {
                    .token = voice.token,
                    .status = MICROPIXEL_STATUS_INTERNAL,
                };
                voice.active = false;
                continue;
            }
            pcm_frames[index] = read.frames;
            pcm_volumes[index] = voice.volume_per_mille;
            if (read.finished) {
                chunk.completions[chunk.completion_count++] = {
                    .token = voice.token,
                    .status = MICROPIXEL_STATUS_OK,
                };
                voice.active = false;
            }
        }
        for (uint32_t frame = 0U; frame < kFramesPerChunk; ++frame) {
            int32_t mixed = 0;
            for (common::audio::SynthVoice& voice : State().voices) {
                if (voice.active) {
                    chunk.rendered_audio = true;
                    mixed += common::audio::SynthMixer::NextSample(voice, State().sine_table);
                }
            }
            for (uint32_t index = 0U; index < kMaxPcmStreams; ++index) {
                if (frame < pcm_frames[index]) {
                    chunk.rendered_audio = true;
                    mixed += static_cast<int32_t>(pcm_samples[index][frame]) * pcm_volumes[index] / 1000;
                }
            }
            mixed = metalio_claw4::ScaleAudioOutputSample(
                mixed, State().master_volume_per_ten_thousand.load(std::memory_order_relaxed));
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            const int32_t output = mixed * 65536;
            frames[frame * 2U] = output;
            frames[frame * 2U + 1U] = output;
        }
        chunk.active_after = HasPlayableVoiceLocked();
    }
    (void)xSemaphoreGive(State().voices_mutex);
    return chunk;
}

void DeliverCompletions(const AudioChunkState& chunk) {
    if (chunk.completion_count == 0U || xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const device::PcmCompletionSink sink = State().pcm_completion_sink;
    void* context = State().pcm_completion_context;
    (void)xSemaphoreGive(State().voices_mutex);
    if (sink == nullptr) {
        return;
    }
    for (uint32_t index = 0U; index < chunk.completion_count; ++index) {
        sink(context, chunk.completions[index]);
    }
}

void NotifyAudioTask() {
    TaskHandle_t task = State().task.load(std::memory_order_acquire);
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

void AudioTask(void* argument) {
    auto& sink = *static_cast<common::audio::AudioSink*>(argument);
    esp_err_t status = sink.Initialize();
    if (status != ESP_OK) {
        State().backend_state.store(BackendState::kFailed, std::memory_order_release);
        ESP_LOGE(kTag, "%s initialization failed: %s", sink.Name(), esp_err_to_name(status));
        sink.Shutdown();
        vTaskDelete(nullptr);
        return;
    }

    State().backend_state.store(BackendState::kReady, std::memory_order_release);
    ESP_LOGI(kTag, "audio ready: sink=%s, %lu Hz, %u synth voices, core=%d", sink.Name(),
             static_cast<unsigned long>(kMetalioClaw4AudioSampleRate), kMaxVoices, xPortGetCoreID());
    int32_t frames[kFramesPerChunk * 2U]{};
    bool first_output_transition = true;
    while (status == ESP_OK) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!HasPlayableVoice()) {
            continue;
        }
        status = sink.Start(frames, kFramesPerChunk);
        if (status == ESP_OK && first_output_transition) {
            ESP_LOGI(kTag, "audio output started on first tone event");
        }
        uint32_t idle_chunks = 0U;
        while (status == ESP_OK) {
            const AudioChunkState chunk = FillAudioChunk(frames);
            status = sink.Write(frames, kFramesPerChunk);
            if (status != ESP_OK) {
                ESP_LOGE(kTag, "%s mixer stopped: %s", sink.Name(), esp_err_to_name(status));
                break;
            }
            DeliverCompletions(chunk);
            idle_chunks = chunk.rendered_audio || chunk.active_after ? 0U : idle_chunks + 1U;
            if (idle_chunks < kAudioIdleGraceChunks || HasPlayableVoice()) {
                continue;
            }
            status = sink.Stop();
            if (status == ESP_OK && first_output_transition) {
                ESP_LOGI(kTag, "audio output returned to idle after %lu ms grace",
                         static_cast<unsigned long>(kAudioIdleGraceMs));
                first_output_transition = false;
            }
            break;
        }
    }
    State().backend_state.store(BackendState::kFailed, std::memory_order_release);
    State().task.store(nullptr, std::memory_order_release);
    ESP_LOGE(kTag, "%s output failed: %s", sink.Name(), esp_err_to_name(status));
    sink.Shutdown();
    vTaskDelete(nullptr);
}

}  // namespace

class MetalioClaw4AudioBackend final : public AudioBackend {
   public:
    [[nodiscard]] esp_err_t Initialize(i2c_master_dev_handle_t io_expander, common::I2cExecutor& i2c_executor) override;
    void SetMasterVolumePercent(uint8_t percent) override;
    [[nodiscard]] int32_t GetInfo(micropixel_audio_info_t& info) override;
    [[nodiscard]] int32_t PlayTone(const micropixel_audio_tone_t& tone) override;
    [[nodiscard]] int32_t StartPcm(const device::PcmSource& source, uint32_t token, uint16_t volume_per_mille,
                                   device::PcmStreamHandle& handle_out) override;
    [[nodiscard]] int32_t PausePcm(device::PcmStreamHandle handle) override;
    [[nodiscard]] int32_t ResumePcm(device::PcmStreamHandle handle) override;
    [[nodiscard]] int32_t SetPcmVolume(device::PcmStreamHandle handle, uint16_t volume_per_mille) override;
    [[nodiscard]] int32_t StopPcm(device::PcmStreamHandle handle) override;
    void BindPcmCompletionSink(device::PcmCompletionSink sink, void* context) override;
    void UnbindPcmCompletionSink(void* context) override;
    [[nodiscard]] int32_t StopAll() override;
    [[nodiscard]] int32_t SuspendAll() override;
    [[nodiscard]] int32_t ResumeAll() override;
};

esp_err_t MetalioClaw4AudioBackend::Initialize(i2c_master_dev_handle_t io_expander, common::I2cExecutor& i2c_executor) {
    if (io_expander == nullptr || State().backend_state.load(std::memory_order_acquire) != BackendState::kStopped) {
        return ESP_ERR_INVALID_STATE;
    }
    State().voices_mutex = xSemaphoreCreateMutex();
    if (State().voices_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    State().sink.Configure(io_expander, i2c_executor);
    common::audio::SynthMixer::InitializeSineTable(State().sine_table);
    State().backend_state.store(BackendState::kInitializing, std::memory_order_release);
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(AudioTask, "micropixel_audio", kAudioTaskStackSize, &State().sink,
                                task_policy::kAudioPriority, &task, kAudioTaskCore) != pdPASS) {
        State().backend_state.store(BackendState::kStopped, std::memory_order_release);
        vSemaphoreDelete(State().voices_mutex);
        State().voices_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    State().task.store(task, std::memory_order_release);
    return ESP_OK;
}

void MetalioClaw4AudioBackend::SetMasterVolumePercent(uint8_t percent) {
    const uint16_t perceptual_output =
        static_cast<uint16_t>(metalio_claw4::PerceptualVolumeOutputPerTenThousand(percent));
    State().master_volume_per_ten_thousand.store(perceptual_output, std::memory_order_relaxed);
}

int32_t MetalioClaw4AudioBackend::GetInfo(micropixel_audio_info_t& info) {
    BackendState state = State().backend_state.load(std::memory_order_acquire);
    if (state == BackendState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == BackendState::kFailed) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    info = {};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_AUDIO_INTERFACE_MINOR;
    info.max_voices = kMaxVoices;
    info.sample_rate = kMetalioClaw4AudioSampleRate;
    info.supported_waveforms = (1U << MICROPIXEL_AUDIO_WAVE_SINE) | (1U << MICROPIXEL_AUDIO_WAVE_SQUARE) |
                               (1U << MICROPIXEL_AUDIO_WAVE_TRIANGLE) | (1U << MICROPIXEL_AUDIO_WAVE_NOISE);
    info.max_tone_duration_ms = MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS;
    info.capabilities = MICROPIXEL_AUDIO_CAPABILITY_OGG_OPUS;
    info.max_clips = 16U;
    info.max_playbacks = kMaxPcmStreams;
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::PlayTone(const micropixel_audio_tone_t& tone) {
    if (!common::audio::SynthMixer::ValidTone(tone)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    BackendState state = State().backend_state.load(std::memory_order_acquire);
    if (state == BackendState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == BackendState::kFailed || State().voices_mutex == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    common::audio::SynthVoice* selected = nullptr;
    for (common::audio::SynthVoice& voice : State().voices) {
        if (!voice.active) {
            selected = &voice;
            break;
        }
        if (selected == nullptr || voice.remaining_frames < selected->remaining_frames) {
            selected = &voice;
        }
    }
    common::audio::SynthMixer::StartVoice(*selected, tone, kMetalioClaw4AudioSampleRate);
    (void)xSemaphoreGive(State().voices_mutex);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::StartPcm(const device::PcmSource& source, uint32_t token, uint16_t volume_per_mille,
                                           device::PcmStreamHandle& handle_out) {
    handle_out = 0U;
    if (source.read == nullptr || source.context == nullptr || volume_per_mille > 1000U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const BackendState state = State().backend_state.load(std::memory_order_acquire);
    if (state == BackendState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == BackendState::kFailed || State().voices_mutex == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    PcmVoice* selected = nullptr;
    uint32_t selected_index = 0U;
    for (uint32_t index = 0U; index < kMaxPcmStreams; ++index) {
        if (!State().pcm_voices[index].active) {
            selected = &State().pcm_voices[index];
            selected_index = index;
            break;
        }
    }
    if (selected == nullptr) {
        (void)xSemaphoreGive(State().voices_mutex);
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    uint32_t generation = (selected->generation + 1U) & 0x00ffffffU;
    if (generation == 0U) {
        generation = 1U;
    }
    *selected = PcmVoice{
        .source = source,
        .token = token,
        .generation = generation,
        .volume_per_mille = volume_per_mille,
        .active = true,
        .paused = false,
    };
    handle_out = PcmHandle(selected_index, generation);
    (void)xSemaphoreGive(State().voices_mutex);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::PausePcm(device::PcmStreamHandle handle) {
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    PcmVoice* voice = FindPcmVoiceLocked(handle);
    if (voice == nullptr) {
        (void)xSemaphoreGive(State().voices_mutex);
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    voice->paused = true;
    (void)xSemaphoreGive(State().voices_mutex);
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::ResumePcm(device::PcmStreamHandle handle) {
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    PcmVoice* voice = FindPcmVoiceLocked(handle);
    if (voice == nullptr) {
        (void)xSemaphoreGive(State().voices_mutex);
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    voice->paused = false;
    (void)xSemaphoreGive(State().voices_mutex);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::SetPcmVolume(device::PcmStreamHandle handle, uint16_t volume_per_mille) {
    if (volume_per_mille > 1000U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    PcmVoice* voice = FindPcmVoiceLocked(handle);
    if (voice == nullptr) {
        (void)xSemaphoreGive(State().voices_mutex);
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    voice->volume_per_mille = volume_per_mille;
    (void)xSemaphoreGive(State().voices_mutex);
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::StopPcm(device::PcmStreamHandle handle) {
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    PcmVoice* voice = FindPcmVoiceLocked(handle);
    if (voice == nullptr) {
        (void)xSemaphoreGive(State().voices_mutex);
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    voice->active = false;
    voice->source = {};
    (void)xSemaphoreGive(State().voices_mutex);
    return MICROPIXEL_STATUS_OK;
}

void MetalioClaw4AudioBackend::BindPcmCompletionSink(device::PcmCompletionSink sink, void* context) {
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    State().pcm_completion_sink = sink;
    State().pcm_completion_context = context;
    (void)xSemaphoreGive(State().voices_mutex);
}

void MetalioClaw4AudioBackend::UnbindPcmCompletionSink(void* context) {
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (State().pcm_completion_context == context) {
        State().pcm_completion_sink = nullptr;
        State().pcm_completion_context = nullptr;
    }
    (void)xSemaphoreGive(State().voices_mutex);
}

int32_t MetalioClaw4AudioBackend::StopAll() {
    BackendState state = State().backend_state.load(std::memory_order_acquire);
    if (state == BackendState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == BackendState::kFailed || State().voices_mutex == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    for (common::audio::SynthVoice& voice : State().voices) {
        voice.active = false;
    }
    for (PcmVoice& voice : State().pcm_voices) {
        voice.active = false;
        voice.source = {};
    }
    (void)xSemaphoreGive(State().voices_mutex);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::SuspendAll() {
    const BackendState state = State().backend_state.load(std::memory_order_acquire);
    if (state == BackendState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == BackendState::kFailed) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    State().suspended.store(true, std::memory_order_release);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

int32_t MetalioClaw4AudioBackend::ResumeAll() {
    const BackendState state = State().backend_state.load(std::memory_order_acquire);
    if (state == BackendState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == BackendState::kFailed) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    State().suspended.store(false, std::memory_order_release);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

AudioBackend& ConfiguredAudioBackend() {
    static MetalioClaw4AudioBackend backend;
    return backend;
}

}  // namespace micropixel::platform::metalio_claw4
