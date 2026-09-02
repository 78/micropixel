#include "platform/audio/audio_engine.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include "freertos/idf_additions.h"
#endif
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/audio/audio_mixer.hpp"
#include "platform/audio/audio_output_peripheral.hpp"
#include "platform/audio/audio_output_policy.hpp"
#include "platform/audio/audio_power_controller.hpp"
#include "platform/audio/volume_curve.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform::audio {
namespace {

constexpr char kTag[] = "micropixel_audio";
constexpr uint32_t kFramesPerChunk = 128U;
constexpr uint32_t kMaxPcmStreams = 2U;
constexpr uint32_t kAudioTaskStackSize = 4096U;
constexpr BaseType_t kAudioTaskCore = 0;
constexpr uint32_t kAudioIdleGraceMs = 10000U;

enum class EngineState : uint8_t {
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

struct AudioEngineState final {
    std::atomic<EngineState> engine_state{EngineState::kStopped};
    std::atomic<bool> suspended{};
    std::atomic<bool> app_foreground{};
    std::atomic<bool> hardware_muted{};
    std::atomic<uint16_t> master_volume_per_ten_thousand{static_cast<uint16_t>(VolumeOutputPerTenThousand(70U))};
    std::atomic<TaskHandle_t> task{};
    SemaphoreHandle_t voices_mutex{};
    SynthVoice voices[kMaxVoices]{};
    PcmVoice pcm_voices[kMaxPcmStreams]{};
    device::PcmCompletionSink pcm_completion_sink{};
    void* pcm_completion_context{};
    AudioOutputPeripheral* sink{};
    AudioPowerController* power_controller{};
    uint32_t sample_rate{};
    int16_t sine_table[kSynthSineTableSize]{};
};

AudioEngineState& State() {
    static AudioEngineState state;
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

struct AudioTaskBuffers final {
    int32_t output_frames[kFramesPerChunk * 2U]{};
    int16_t pcm_samples[kMaxPcmStreams][kFramesPerChunk]{};
};

bool HasPlayableVoiceLocked() {
    if (State().suspended.load(std::memory_order_acquire)) {
        return false;
    }
    for (const SynthVoice& voice : State().voices) {
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

bool AppKeepsOutputActive() {
    return State().app_foreground.load(std::memory_order_acquire) && !State().suspended.load(std::memory_order_acquire);
}

bool HasOutputDemand() { return ShouldRunAudioOutput(AppKeepsOutputActive(), HasPlayableVoice()); }

AudioChunkState FillAudioChunk(AudioTaskBuffers& buffers) {
    std::memset(buffers.output_frames, 0, sizeof(buffers.output_frames));
    if (xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return {};
    }
    AudioChunkState chunk{};
    if (!State().suspended.load(std::memory_order_acquire)) {
        std::memset(buffers.pcm_samples, 0, sizeof(buffers.pcm_samples));
        uint32_t pcm_frames[kMaxPcmStreams]{};
        uint16_t pcm_volumes[kMaxPcmStreams]{};
        for (uint32_t index = 0U; index < kMaxPcmStreams; ++index) {
            PcmVoice& voice = State().pcm_voices[index];
            if (!voice.active || voice.paused || voice.source.read == nullptr) {
                continue;
            }
            const device::PcmReadResult read =
                voice.source.read(voice.source.context, buffers.pcm_samples[index], kFramesPerChunk);
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
            for (SynthVoice& voice : State().voices) {
                if (voice.active) {
                    chunk.rendered_audio = true;
                    mixed += AudioMixer::NextSample(voice, State().sine_table);
                }
            }
            for (uint32_t index = 0U; index < kMaxPcmStreams; ++index) {
                if (frame < pcm_frames[index]) {
                    chunk.rendered_audio = true;
                    mixed += static_cast<int32_t>(buffers.pcm_samples[index][frame]) * pcm_volumes[index] / 1000;
                }
            }
            mixed = ApplyHostOutputGain(mixed, State().master_volume_per_ten_thousand.load(std::memory_order_relaxed),
                                        State().hardware_muted.load(std::memory_order_relaxed));
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            const int32_t output = mixed * 65536;
            buffers.output_frames[frame * 2U] = output;
            buffers.output_frames[frame * 2U + 1U] = output;
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
    auto& sink = *static_cast<AudioOutputPeripheral*>(argument);
    auto* buffers = static_cast<AudioTaskBuffers*>(
        heap_caps_calloc(1U, sizeof(AudioTaskBuffers), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffers == nullptr) {
        State().engine_state.store(EngineState::kFailed, std::memory_order_release);
        ESP_LOGE(kTag, "unable to allocate audio task buffers in PSRAM");
        vTaskDelete(nullptr);
        return;
    }
    AudioPowerController* power_controller = State().power_controller;
    bool sink_initialized = false;
    esp_err_t status = power_controller == nullptr ? sink.Initialize() : ESP_OK;
    sink_initialized = status == ESP_OK && power_controller == nullptr;
    if (status != ESP_OK) {
        State().engine_state.store(EngineState::kFailed, std::memory_order_release);
        ESP_LOGE(kTag, "%s initialization failed: %s", sink.Name(), esp_err_to_name(status));
        sink.Shutdown();
        heap_caps_free(buffers);
        vTaskDelete(nullptr);
        return;
    }

    State().engine_state.store(EngineState::kReady, std::memory_order_release);
    ESP_LOGI(kTag, "audio ready: sink=%s, %lu Hz, %u synth voices, core=%d", sink.Name(),
             static_cast<unsigned long>(State().sample_rate), kMaxVoices, xPortGetCoreID());
    bool first_output_transition = true;
    while (status == ESP_OK) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!HasOutputDemand()) {
            continue;
        }
        if (power_controller != nullptr) {
            status = power_controller->PowerOnAudio();
            if (status == ESP_OK) {
                status = sink.Initialize();
                sink_initialized = status == ESP_OK;
                if (status != ESP_OK) {
                    sink.Shutdown();
                }
            }
            if (status != ESP_OK) {
                ESP_LOGE(kTag, "%s on-demand initialization failed: %s", sink.Name(), esp_err_to_name(status));
                break;
            }
        }
        status = sink.Start(buffers->output_frames, kFramesPerChunk);
        if (status == ESP_OK && first_output_transition) {
            ESP_LOGI(kTag, "audio output started for foreground App");
        }
        uint32_t idle_chunks = 0U;
        while (status == ESP_OK) {
            const AudioChunkState chunk = FillAudioChunk(*buffers);
            status = sink.Write(buffers->output_frames, kFramesPerChunk);
            if (status != ESP_OK) {
                ESP_LOGE(kTag, "%s mixer stopped: %s", sink.Name(), esp_err_to_name(status));
                break;
            }
            DeliverCompletions(chunk);
            const bool app_foreground = AppKeepsOutputActive();
            const bool playable_voice = HasPlayableVoice();
            idle_chunks = NextAudioIdleChunkCount(idle_chunks, app_foreground, chunk.rendered_audio,
                                                  chunk.active_after || playable_voice);
            const uint32_t idle_grace_chunks =
                (kAudioIdleGraceMs * State().sample_rate + kFramesPerChunk * 1000U - 1U) / (kFramesPerChunk * 1000U);
            if (!ShouldStopAudioOutput(app_foreground, idle_chunks, idle_grace_chunks, playable_voice)) {
                continue;
            }
            status = sink.Stop();
            if (status == ESP_OK && power_controller != nullptr) {
                sink.Shutdown();
                sink_initialized = false;
                status = power_controller->PowerOffAudio();
            }
            if (status == ESP_OK && first_output_transition) {
                ESP_LOGI(kTag, "audio output returned to idle after %lu ms grace",
                         static_cast<unsigned long>(kAudioIdleGraceMs));
                first_output_transition = false;
            }
            break;
        }
    }
    State().engine_state.store(EngineState::kFailed, std::memory_order_release);
    State().task.store(nullptr, std::memory_order_release);
    ESP_LOGE(kTag, "%s output failed: %s", sink.Name(), esp_err_to_name(status));
    if (sink_initialized) {
        sink.Shutdown();
    }
    if (power_controller != nullptr) {
        (void)power_controller->PowerOffAudio();
    }
    heap_caps_free(buffers);
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t AudioEngine::Initialize(AudioOutputPeripheral& sink, uint32_t sample_rate,
                                  AudioPowerController* power_controller) {
    if (sample_rate == 0U || State().engine_state.load(std::memory_order_acquire) != EngineState::kStopped) {
        return ESP_ERR_INVALID_STATE;
    }
    State().voices_mutex = xSemaphoreCreateMutex();
    if (State().voices_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    State().sink = &sink;
    State().power_controller = power_controller;
    State().sample_rate = sample_rate;
    AudioMixer::InitializeSineTable(State().sine_table);
    State().engine_state.store(EngineState::kInitializing, std::memory_order_release);
    TaskHandle_t task = nullptr;
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const BaseType_t task_created = xTaskCreatePinnedToCoreWithCaps(
        AudioTask, "micropixel_audio", kAudioTaskStackSize, State().sink, task_policy::kAudioPriority, &task,
        kAudioTaskCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t task_created =
        xTaskCreatePinnedToCore(AudioTask, "micropixel_audio", kAudioTaskStackSize, State().sink,
                                task_policy::kAudioPriority, &task, kAudioTaskCore);
#endif
    if (task_created != pdPASS) {
        State().engine_state.store(EngineState::kStopped, std::memory_order_release);
        vSemaphoreDelete(State().voices_mutex);
        State().voices_mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    State().task.store(task, std::memory_order_release);
    return ESP_OK;
}

void AudioEngine::SetMasterVolumePercent(uint8_t percent) {
    const uint16_t perceptual_output = VolumeOutputPerTenThousand(percent);
    State().master_volume_per_ten_thousand.store(perceptual_output, std::memory_order_relaxed);
}

void AudioEngine::SetHardwareMuted(bool muted) { State().hardware_muted.store(muted, std::memory_order_relaxed); }

int32_t AudioEngine::GetInfo(micropixel_audio_info_t& info) {
    EngineState state = State().engine_state.load(std::memory_order_acquire);
    if (state == EngineState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == EngineState::kFailed) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    info = {};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_AUDIO_INTERFACE_MINOR;
    info.max_voices = kMaxVoices;
    info.sample_rate = State().sample_rate;
    info.supported_waveforms = (1U << MICROPIXEL_AUDIO_WAVE_SINE) | (1U << MICROPIXEL_AUDIO_WAVE_SQUARE) |
                               (1U << MICROPIXEL_AUDIO_WAVE_TRIANGLE) | (1U << MICROPIXEL_AUDIO_WAVE_NOISE);
    info.max_tone_duration_ms = MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS;
    info.capabilities = MICROPIXEL_AUDIO_CAPABILITY_OGG_OPUS;
    info.max_clips = 16U;
    info.max_playbacks = kMaxPcmStreams;
    return MICROPIXEL_STATUS_OK;
}

int32_t AudioEngine::PlayTone(const micropixel_audio_tone_t& tone) {
    if (!AudioMixer::ValidTone(tone)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    EngineState state = State().engine_state.load(std::memory_order_acquire);
    if (state == EngineState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == EngineState::kFailed || State().voices_mutex == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    SynthVoice* selected = nullptr;
    for (SynthVoice& voice : State().voices) {
        if (!voice.active) {
            selected = &voice;
            break;
        }
        if (selected == nullptr || voice.remaining_frames < selected->remaining_frames) {
            selected = &voice;
        }
    }
    AudioMixer::StartVoice(*selected, tone, State().sample_rate);
    (void)xSemaphoreGive(State().voices_mutex);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

int32_t AudioEngine::StartPcm(const device::PcmSource& source, uint32_t token, uint16_t volume_per_mille,
                              device::PcmStreamHandle& handle_out) {
    handle_out = 0U;
    if (source.read == nullptr || source.context == nullptr || volume_per_mille > 1000U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const EngineState state = State().engine_state.load(std::memory_order_acquire);
    if (state == EngineState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == EngineState::kFailed || State().voices_mutex == nullptr) {
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

int32_t AudioEngine::PausePcm(device::PcmStreamHandle handle) {
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

int32_t AudioEngine::ResumePcm(device::PcmStreamHandle handle) {
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

int32_t AudioEngine::SetPcmVolume(device::PcmStreamHandle handle, uint16_t volume_per_mille) {
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

int32_t AudioEngine::StopPcm(device::PcmStreamHandle handle) {
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

void AudioEngine::BindPcmCompletionSink(device::PcmCompletionSink sink, void* context) {
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    State().pcm_completion_sink = sink;
    State().pcm_completion_context = context;
    (void)xSemaphoreGive(State().voices_mutex);
}

void AudioEngine::UnbindPcmCompletionSink(void* context) {
    if (State().voices_mutex == nullptr || xSemaphoreTake(State().voices_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (State().pcm_completion_context == context) {
        State().pcm_completion_sink = nullptr;
        State().pcm_completion_context = nullptr;
    }
    (void)xSemaphoreGive(State().voices_mutex);
}

int32_t AudioEngine::StopAll() {
    EngineState state = State().engine_state.load(std::memory_order_acquire);
    if (state == EngineState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == EngineState::kFailed || State().voices_mutex == nullptr) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (xSemaphoreTake(State().voices_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    for (SynthVoice& voice : State().voices) {
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

int32_t AudioEngine::SuspendAll() {
    const EngineState state = State().engine_state.load(std::memory_order_acquire);
    if (state == EngineState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == EngineState::kFailed) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    State().suspended.store(true, std::memory_order_release);
    State().app_foreground.store(false, std::memory_order_release);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

int32_t AudioEngine::ResumeAll() {
    const EngineState state = State().engine_state.load(std::memory_order_acquire);
    if (state == EngineState::kStopped) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (state == EngineState::kFailed) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    State().suspended.store(false, std::memory_order_release);
    State().app_foreground.store(true, std::memory_order_release);
    NotifyAudioTask();
    return MICROPIXEL_STATUS_OK;
}

}  // namespace micropixel::platform::audio
