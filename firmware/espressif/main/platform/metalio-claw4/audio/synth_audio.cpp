#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform/audio_backend.hpp"
#include "platform/metalio-claw4/perceptual_control.hpp"
#include "task_policy.hpp"

namespace micropixel::platform {
namespace {

constexpr char kTag[] = "micropixel_audio";
constexpr uint8_t kOutputPort0 = 0x02U;
constexpr uint8_t kOutputPort1 = 0x03U;
constexpr uint8_t kConfigPort0 = 0x06U;
constexpr uint8_t kConfigPort1 = 0x07U;
constexpr uint8_t kPaSwitchMask = 1U << 1U;
constexpr uint8_t kBtPowerMask = 1U << 6U;
constexpr uint8_t kPaEnableMask = 1U << 0U;
constexpr uint32_t kFramesPerChunk = 128U;
constexpr uint32_t kMaxPcmStreams = 2U;
constexpr uint32_t kSineTableSize = 256U;
constexpr uint32_t kAudioTaskStackSize = 4096U;
constexpr BaseType_t kAudioTaskCore = 0;
constexpr uint32_t kMetalioClaw4AudioSampleRate = 16000U;
constexpr uint32_t kAudioOutputWakeupMs = 32U;
constexpr uint32_t kAudioOutputWakeupChunks =
    (kAudioOutputWakeupMs * kMetalioClaw4AudioSampleRate + kFramesPerChunk * 1000U - 1U) / (kFramesPerChunk * 1000U);
constexpr uint32_t kAudioIdleGraceMs = 10000U;
constexpr uint32_t kAudioIdleGraceChunks =
    (kAudioIdleGraceMs * kMetalioClaw4AudioSampleRate + kFramesPerChunk * 1000U - 1U) / (kFramesPerChunk * 1000U);

i2s_std_clk_config_t AudioClockConfig() { return I2S_STD_CLK_DEFAULT_CONFIG(kMetalioClaw4AudioSampleRate); }

enum class BackendState : uint8_t {
    kStopped,
    kInitializing,
    kReady,
    kFailed,
};

struct Voice final {
    uint32_t waveform{};
    uint32_t phase{};
    uint32_t phase_step{};
    uint32_t total_frames{};
    uint32_t remaining_frames{};
    uint32_t attack_frames{};
    uint32_t release_frames{};
    uint32_t volume_per_mille{};
    uint32_t noise{0x51a9e21dU};
    bool active{};
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
    Voice voices[kMaxVoices]{};
    PcmVoice pcm_voices[kMaxPcmStreams]{};
    device::PcmCompletionSink pcm_completion_sink{};
    void* pcm_completion_context{};
    int16_t sine_table[kSineTableSize]{};
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

esp_err_t ReadRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t& value) {
    return i2c_master_transmit_receive(device, &address, sizeof(address), &value, sizeof(value), 1000);
}

esp_err_t WriteRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t value) {
    const uint8_t transaction[] = {address, value};
    return i2c_master_transmit(device, transaction, sizeof(transaction), 1000);
}

esp_err_t UpdateRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t set_mask, uint8_t clear_mask) {
    uint8_t value = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(device, address, value), kTag, "read TCA9555 register 0x%02x", address);
    value = static_cast<uint8_t>((value | set_mask) & ~clear_mask);
    return WriteRegister(device, address, value);
}

esp_err_t ConfigureAudioRails(i2c_master_dev_handle_t device) {
    ESP_RETURN_ON_ERROR(UpdateRegister(device, kOutputPort0, kPaSwitchMask | kBtPowerMask, 0U), kTag,
                        "set audio rail latches");
    ESP_RETURN_ON_ERROR(UpdateRegister(device, kOutputPort1, 0U, kPaEnableMask), kTag, "mute audio amplifier");
    ESP_RETURN_ON_ERROR(UpdateRegister(device, kConfigPort0, 0U, kPaSwitchMask | kBtPowerMask), kTag,
                        "configure audio rail outputs");
    return UpdateRegister(device, kConfigPort1, 0U, kPaEnableMask);
}

esp_err_t InitializeBtAudioMode() {
    uart_config_t config{};
    config.baud_rate = 115200;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM_2, 512, 512, 0, nullptr, 0), kTag, "install audio UART");
    esp_err_t status = uart_param_config(UART_NUM_2, &config);
    if (status == ESP_OK) {
        status = uart_set_pin(UART_NUM_2, GPIO_NUM_26, GPIO_NUM_27, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (status != ESP_OK) {
        (void)uart_driver_delete(UART_NUM_2);
        return status;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
    constexpr char kReceivePath[] = "AT+RX=2\r\n";
    constexpr char kModeOne[] = "AT+MODE=1\r\n";
    if (uart_write_bytes(UART_NUM_2, kReceivePath, sizeof(kReceivePath) - 1U) < 0) {
        return ESP_FAIL;
    }
    (void)uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(700));
    if (uart_write_bytes(UART_NUM_2, kModeOne, sizeof(kModeOne) - 1U) < 0) {
        return ESP_FAIL;
    }
    (void)uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(700));

    uint8_t response[160]{};
    int response_size = uart_read_bytes(UART_NUM_2, response, sizeof(response) - 1U, pdMS_TO_TICKS(200));
    if (response_size > 0) {
        response[response_size] = '\0';
        for (int index = 0; index < response_size; ++index) {
            if (response[index] == '\r' || response[index] == '\n') {
                response[index] = ' ';
            }
        }
        ESP_LOGI(kTag, "audio module response: %s", reinterpret_cast<const char*>(response));
    } else {
        ESP_LOGW(kTag, "audio module returned no UART response");
    }
    return ESP_OK;
}

int32_t WaveformSample(Voice& voice) {
    switch (voice.waveform) {
        case MICROPIXEL_AUDIO_WAVE_SINE:
            return State().sine_table[voice.phase >> 24U];
        case MICROPIXEL_AUDIO_WAVE_SQUARE:
            return (voice.phase & 0x80000000U) == 0U ? 32767 : -32768;
        case MICROPIXEL_AUDIO_WAVE_TRIANGLE: {
            uint32_t position = voice.phase >> 16U;
            return position < 32768U ? static_cast<int32_t>(position * 2U) - 32768
                                     : 98303 - static_cast<int32_t>(position * 2U);
        }
        case MICROPIXEL_AUDIO_WAVE_NOISE:
            voice.noise = voice.noise * 1664525U + 1013904223U;
            return static_cast<int16_t>(voice.noise >> 16U);
        default:
            return 0;
    }
}

int32_t NextVoiceSample(Voice& voice) {
    const uint32_t played_frames = voice.total_frames - voice.remaining_frames;
    uint32_t gain = voice.volume_per_mille;
    if (voice.attack_frames != 0U && played_frames < voice.attack_frames) {
        gain = gain * played_frames / voice.attack_frames;
    }
    if (voice.release_frames != 0U && voice.remaining_frames < voice.release_frames) {
        uint32_t release_gain = voice.volume_per_mille * voice.remaining_frames / voice.release_frames;
        if (release_gain < gain) {
            gain = release_gain;
        }
    }
    int32_t sample = WaveformSample(voice) * static_cast<int32_t>(gain) / 1000;
    voice.phase += voice.phase_step;
    --voice.remaining_frames;
    if (voice.remaining_frames == 0U) {
        voice.active = false;
    }
    return sample;
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
    for (const Voice& voice : State().voices) {
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
            for (Voice& voice : State().voices) {
                if (voice.active) {
                    chunk.rendered_audio = true;
                    mixed += NextVoiceSample(voice);
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

esp_err_t WriteSilentAudioChunk(i2s_chan_handle_t tx, int32_t* frames) {
    std::memset(frames, 0, kFramesPerChunk * 2U * sizeof(*frames));
    size_t bytes_written = 0U;
    const size_t bytes = kFramesPerChunk * 2U * sizeof(*frames);
    const esp_err_t status = i2s_channel_write(tx, frames, bytes, &bytes_written, pdMS_TO_TICKS(500));
    if (status != ESP_OK) {
        return status;
    }
    return bytes_written == bytes ? ESP_OK : ESP_FAIL;
}

esp_err_t StartAudioOutput(i2c_master_dev_handle_t expander, i2s_chan_handle_t tx, int32_t* frames) {
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), kTag, "enable idle-gated I2S output");

    const esp_err_t prefill_status = WriteSilentAudioChunk(tx, frames);
    if (prefill_status != ESP_OK) {
        (void)i2s_channel_disable(tx);
        ESP_LOGE(kTag, "I2S silent prefill failed: %s", esp_err_to_name(prefill_status));
        return prefill_status;
    }

    const esp_err_t amplifier_status = UpdateRegister(expander, kOutputPort1, kPaEnableMask, 0U);
    if (amplifier_status != ESP_OK) {
        (void)i2s_channel_disable(tx);
        return amplifier_status;
    }

    // Keep real voice frames out of the amplifier wake-up interval. Without this
    // pre-roll, a short gameplay effect can finish before the PA becomes audible.
    for (uint32_t chunk = 0U; chunk < kAudioOutputWakeupChunks; ++chunk) {
        const esp_err_t wakeup_status = WriteSilentAudioChunk(tx, frames);
        if (wakeup_status != ESP_OK) {
            (void)UpdateRegister(expander, kOutputPort1, 0U, kPaEnableMask);
            (void)i2s_channel_disable(tx);
            ESP_LOGE(kTag, "I2S amplifier wake-up pre-roll failed: %s", esp_err_to_name(wakeup_status));
            return wakeup_status;
        }
    }
    return ESP_OK;
}

esp_err_t StopAudioOutput(i2c_master_dev_handle_t expander, i2s_chan_handle_t tx) {
    const esp_err_t amplifier_status = UpdateRegister(expander, kOutputPort1, 0U, kPaEnableMask);
    const esp_err_t i2s_status = i2s_channel_disable(tx);
    if (amplifier_status != ESP_OK) {
        return amplifier_status;
    }
    return i2s_status;
}

void AudioTask(void* argument) {
    auto expander = static_cast<i2c_master_dev_handle_t>(argument);
    esp_err_t status = ConfigureAudioRails(expander);
    if (status == ESP_OK) {
        status = InitializeBtAudioMode();
    }

    i2s_chan_handle_t tx = nullptr;
    if (status == ESP_OK) {
        i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
        channel_config.dma_desc_num = 6U;
        channel_config.dma_frame_num = 240U;
        channel_config.auto_clear_after_cb = true;
        status = i2s_new_channel(&channel_config, &tx, nullptr);
    }
    if (status == ESP_OK) {
        i2s_std_config_t standard_config{};
        standard_config.clk_cfg = AudioClockConfig();
        standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
        standard_config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        standard_config.gpio_cfg.bclk = GPIO_NUM_12;
        standard_config.gpio_cfg.ws = GPIO_NUM_10;
        standard_config.gpio_cfg.dout = GPIO_NUM_9;
        standard_config.gpio_cfg.din = I2S_GPIO_UNUSED;
        status = i2s_channel_init_std_mode(tx, &standard_config);
    }
    if (status != ESP_OK) {
        State().backend_state.store(BackendState::kFailed, std::memory_order_release);
        ESP_LOGE(kTag, "Metalio-Claw4 audio backend failed: %s", esp_err_to_name(status));
        if (tx != nullptr) {
            (void)i2s_del_channel(tx);
        }
        (void)uart_driver_delete(UART_NUM_2);
        vTaskDelete(nullptr);
        return;
    }

    State().backend_state.store(BackendState::kReady, std::memory_order_release);
    ESP_LOGI(kTag, "Metalio-Claw4 audio ready: %lu Hz, %u synth voices, idle-gated I2S/PA, core=%d",
             static_cast<unsigned long>(kMetalioClaw4AudioSampleRate), kMaxVoices, xPortGetCoreID());
    int32_t frames[kFramesPerChunk * 2U]{};
    bool first_output_transition = true;
    while (status == ESP_OK) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!HasPlayableVoice()) {
            continue;
        }
        status = StartAudioOutput(expander, tx, frames);
        if (status == ESP_OK && first_output_transition) {
            ESP_LOGI(kTag, "audio output started on first tone event");
        }
        uint32_t idle_chunks = 0U;
        while (status == ESP_OK) {
            const AudioChunkState chunk = FillAudioChunk(frames);
            size_t bytes_written = 0U;
            status = i2s_channel_write(tx, frames, sizeof(frames), &bytes_written, pdMS_TO_TICKS(500));
            if (status != ESP_OK || bytes_written != sizeof(frames)) {
                ESP_LOGE(kTag, "I2S mixer stopped: %s, wrote=%zu/%zu", esp_err_to_name(status), bytes_written,
                         sizeof(frames));
                status = status != ESP_OK ? status : ESP_FAIL;
                break;
            }
            DeliverCompletions(chunk);
            idle_chunks = chunk.rendered_audio || chunk.active_after ? 0U : idle_chunks + 1U;
            if (idle_chunks < kAudioIdleGraceChunks || HasPlayableVoice()) {
                continue;
            }
            status = StopAudioOutput(expander, tx);
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
    ESP_LOGE(kTag, "Metalio-Claw4 audio output failed: %s", esp_err_to_name(status));
    (void)UpdateRegister(expander, kOutputPort1, 0U, kPaEnableMask);
    (void)i2s_channel_disable(tx);
    (void)i2s_del_channel(tx);
    (void)uart_driver_delete(UART_NUM_2);
    vTaskDelete(nullptr);
}

bool ValidTone(const micropixel_audio_tone_t& tone) {
    return tone.size == sizeof(tone) && tone.interface_major == MICROPIXEL_AUDIO_INTERFACE_MAJOR &&
           tone.waveform >= MICROPIXEL_AUDIO_WAVE_SINE && tone.waveform <= MICROPIXEL_AUDIO_WAVE_NOISE &&
           tone.volume_per_mille <= 1000U && tone.duration_ms != 0U &&
           tone.duration_ms <= MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS && tone.attack_ms <= tone.duration_ms &&
           tone.release_ms <= tone.duration_ms && tone.reserved[0] == 0U && tone.reserved[1] == 0U &&
           tone.reserved[2] == 0U &&
           (tone.waveform == MICROPIXEL_AUDIO_WAVE_NOISE ||
            (tone.frequency_millihz >= 20000U && tone.frequency_millihz <= 20000000U));
}

}  // namespace

class MetalioClaw4AudioBackend final : public InitializableAudioBackend {
   public:
    [[nodiscard]] esp_err_t Initialize(i2c_master_dev_handle_t io_expander) override;
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

esp_err_t MetalioClaw4AudioBackend::Initialize(i2c_master_dev_handle_t io_expander) {
    if (io_expander == nullptr || State().backend_state.load(std::memory_order_acquire) != BackendState::kStopped) {
        return ESP_ERR_INVALID_STATE;
    }
    State().voices_mutex = xSemaphoreCreateMutex();
    if (State().voices_mutex == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    for (uint32_t index = 0U; index < kSineTableSize; ++index) {
        constexpr double kTau = 6.28318530717958647692;
        State().sine_table[index] =
            static_cast<int16_t>(std::sin(kTau * static_cast<double>(index) / kSineTableSize) * 32767.0);
    }
    State().backend_state.store(BackendState::kInitializing, std::memory_order_release);
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(AudioTask, "micropixel_audio", kAudioTaskStackSize, io_expander,
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
    if (!ValidTone(tone)) {
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
    Voice* selected = nullptr;
    for (Voice& voice : State().voices) {
        if (!voice.active) {
            selected = &voice;
            break;
        }
        if (selected == nullptr || voice.remaining_frames < selected->remaining_frames) {
            selected = &voice;
        }
    }
    const uint64_t frequency = tone.waveform == MICROPIXEL_AUDIO_WAVE_NOISE ? 0U : tone.frequency_millihz;
    *selected = Voice{
        .waveform = tone.waveform,
        .phase = 0U,
        .phase_step = static_cast<uint32_t>(frequency * (1ULL << 32U) /
                                            (static_cast<uint64_t>(kMetalioClaw4AudioSampleRate) * 1000ULL)),
        .total_frames = tone.duration_ms * kMetalioClaw4AudioSampleRate / 1000U,
        .remaining_frames = tone.duration_ms * kMetalioClaw4AudioSampleRate / 1000U,
        .attack_frames = tone.attack_ms * kMetalioClaw4AudioSampleRate / 1000U,
        .release_frames = tone.release_ms * kMetalioClaw4AudioSampleRate / 1000U,
        .volume_per_mille = tone.volume_per_mille,
        .noise = 0x51a9e21dU ^ tone.frequency_millihz ^ tone.duration_ms,
        .active = true,
    };
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
    for (Voice& voice : State().voices) {
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

InitializableAudioBackend& ConfiguredAudioBackend() {
    static MetalioClaw4AudioBackend backend;
    return backend;
}

}  // namespace micropixel::platform
