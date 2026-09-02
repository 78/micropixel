#include "platform/audio/es8311_i2s_audio_sink.hpp"

#include <cstddef>
#include <cstring>

#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::audio {
namespace {

constexpr uint32_t kMclkMultiple = 256U;
constexpr int kI2cTimeoutMs = 100;

bool ValidConfig(const Es8311I2sAudioConfig& config) {
    return config.name != nullptr && config.log_tag != nullptr && config.master_clock != GPIO_NUM_NC &&
           config.bit_clock != GPIO_NUM_NC && config.word_select != GPIO_NUM_NC && config.data_out != GPIO_NUM_NC &&
           (config.amplifier_enable != GPIO_NUM_NC || config.amplifier_setter != nullptr) &&
           config.codec_i2c_address != 0U && config.sample_rate != 0U && config.i2c_clock_hz != 0U &&
           config.dma_descriptor_count != 0U && config.dma_frame_count != 0U;
}

}  // namespace

Es8311I2sAudioSink::CodecControl& Es8311I2sAudioSink::Control(const audio_codec_ctrl_if_t* interface) {
    return *reinterpret_cast<CodecControl*>(const_cast<audio_codec_ctrl_if_t*>(interface));
}

int Es8311I2sAudioSink::ControlOpen(const audio_codec_ctrl_if_t* interface, void*, int) {
    CodecControl& control = Control(interface);
    control.open = control.executor != nullptr && control.device != nullptr;
    return control.open ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRONG_STATE;
}

bool Es8311I2sAudioSink::ControlIsOpen(const audio_codec_ctrl_if_t* interface) {
    return interface != nullptr && Control(interface).open;
}

int Es8311I2sAudioSink::ControlRead(const audio_codec_ctrl_if_t* interface, int reg, int reg_len, void* data,
                                    int data_len) {
    if (interface == nullptr || data == nullptr || reg_len < 1 || reg_len > 2 || data_len < 1) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    CodecControl& control = Control(interface);
    if (!control.open || control.executor == nullptr || control.device == nullptr) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }
    struct Request final {
        i2c_master_dev_handle_t device;
        uint8_t address[2];
        int address_length;
        void* data;
        int data_length;
    } request{
        .device = control.device,
        .address = {static_cast<uint8_t>(reg_len > 1 ? reg >> 8 : reg), static_cast<uint8_t>(reg)},
        .address_length = reg_len,
        .data = data,
        .data_length = data_len,
    };
    if (reg_len == 1) {
        request.address[0] = static_cast<uint8_t>(reg);
    }
    const esp_err_t status = control.executor->Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            return i2c_master_transmit_receive(requested.device, requested.address, requested.address_length,
                                               static_cast<uint8_t*>(requested.data), requested.data_length,
                                               kI2cTimeoutMs);
        },
        &request);
    return status == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_READ_FAIL;
}

int Es8311I2sAudioSink::ControlWrite(const audio_codec_ctrl_if_t* interface, int reg, int reg_len, void* data,
                                     int data_len) {
    if (interface == nullptr || data == nullptr || reg_len < 1 || reg_len > 2 || data_len < 0 ||
        reg_len + data_len > 8) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    CodecControl& control = Control(interface);
    if (!control.open || control.executor == nullptr || control.device == nullptr) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }
    struct Request final {
        i2c_master_dev_handle_t device;
        uint8_t bytes[8];
        int length;
    } request{.device = control.device, .bytes = {}, .length = reg_len + data_len};
    if (reg_len > 1) {
        request.bytes[0] = static_cast<uint8_t>(reg >> 8);
        request.bytes[1] = static_cast<uint8_t>(reg);
    } else {
        request.bytes[0] = static_cast<uint8_t>(reg);
    }
    std::memcpy(request.bytes + reg_len, data, static_cast<size_t>(data_len));
    const esp_err_t status = control.executor->Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            return i2c_master_transmit(requested.device, requested.bytes, requested.length, kI2cTimeoutMs);
        },
        &request);
    return status == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

int Es8311I2sAudioSink::ControlClose(const audio_codec_ctrl_if_t* interface) {
    if (interface == nullptr) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    Control(interface).open = false;
    return ESP_CODEC_DEV_OK;
}

esp_err_t Es8311I2sAudioSink::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    if (!ValidConfig(config_) || bus == nullptr || codec_i2c_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config_.probe_before_attach) {
        struct ProbeRequest final {
            i2c_master_bus_handle_t bus;
            uint16_t address;
        } request{.bus = bus, .address = config_.codec_i2c_address};
        const esp_err_t probe_status = executor.Invoke(
            buses::I2cExecutor::Priority::kHigh,
            [](void* context) {
                const auto& requested = *static_cast<ProbeRequest*>(context);
                return i2c_master_probe(requested.bus, requested.address, kI2cTimeoutMs);
            },
            &request);
        if (probe_status != ESP_OK) {
            ESP_LOGW(config_.log_tag, "%s did not acknowledge at I2C 0x%02x: %s", config_.name,
                     config_.codec_i2c_address, esp_err_to_name(probe_status));
            return probe_status;
        }
    }
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = config_.codec_i2c_address;
    device_config.scl_speed_hz = config_.i2c_clock_hz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &codec_i2c_), config_.log_tag,
                        "attach ES8311 control bus failed");
    control_.interface = {};
    control_.interface.open = ControlOpen;
    control_.interface.is_open = ControlIsOpen;
    control_.interface.read_reg = ControlRead;
    control_.interface.write_reg = ControlWrite;
    auto configure_optional_info = []<typename Interface>(Interface& interface) {
        if constexpr (requires { interface.get_info; }) {
            interface.get_info = [](const audio_codec_ctrl_if_t* raw, auto* info) -> int {
                if (raw == nullptr || info == nullptr) {
                    return ESP_CODEC_DEV_INVALID_ARG;
                }
                const CodecControl& control = Control(raw);
                *info = {};
                info->type = static_cast<decltype(info->type)>(1);
                info->i2c.addr = static_cast<uint16_t>(control.address << 1U);
                info->i2c.port = static_cast<uint8_t>(control.port);
                return ESP_CODEC_DEV_OK;
            };
        }
    };
    configure_optional_info(control_.interface);
    control_.interface.close = ControlClose;
    control_.executor = &executor;
    control_.device = codec_i2c_;
    control_.address = config_.codec_i2c_address;
    control_.port = config_.i2c_port;
    control_.open = true;
    return ESP_OK;
}

int Es8311I2sAudioSink::AmplifierLevel(bool enabled) const { return enabled != config_.amplifier_active_low ? 1 : 0; }

esp_err_t Es8311I2sAudioSink::SetAmplifier(bool enabled) const {
    if (config_.amplifier_setter != nullptr) {
        return config_.amplifier_setter(config_.amplifier_context, enabled);
    }
    // Set the latch before esp_codec_dev claims/configures the PA pin, avoiding
    // an audible pulse without registering a second GPIO owner.
    return gpio_set_level(config_.amplifier_enable, AmplifierLevel(enabled));
}

esp_err_t Es8311I2sAudioSink::FailInitialization(esp_err_t status) {
    Shutdown();
    return status;
}

esp_err_t Es8311I2sAudioSink::Initialize() {
    if (control_.executor == nullptr || codec_i2c_ == nullptr || tx_ != nullptr || codec_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ControlOpen(&control_.interface, nullptr, 0) != ESP_CODEC_DEV_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t status = SetAmplifier(false);
    if (status != ESP_OK) {
        return FailInitialization(status);
    }
    converted_frames_ = static_cast<int16_t*>(heap_caps_aligned_calloc(
        16U, kMaximumChunkFrames * 2U, sizeof(*converted_frames_), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (converted_frames_ == nullptr) {
        ESP_LOGE(config_.log_tag, "%s requires a PSRAM audio conversion block", config_.name);
        return FailInitialization(ESP_ERR_NO_MEM);
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(config_.i2s_port, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = config_.dma_descriptor_count;
    channel_config.dma_frame_num = config_.dma_frame_count;
    channel_config.auto_clear_after_cb = true;
    status = i2s_new_channel(&channel_config, &tx_, nullptr);
    if (status != ESP_OK) {
        return FailInitialization(status);
    }
    i2s_std_config_t standard_config{};
    standard_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config_.sample_rate);
    standard_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    standard_config.gpio_cfg.mclk = config_.master_clock;
    standard_config.gpio_cfg.bclk = config_.bit_clock;
    standard_config.gpio_cfg.ws = config_.word_select;
    standard_config.gpio_cfg.dout = config_.data_out;
    standard_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    status = i2s_channel_init_std_mode(tx_, &standard_config);
    if (status != ESP_OK) {
        return FailInitialization(status);
    }

    audio_codec_i2s_cfg_t i2s_config{};
    i2s_config.port = config_.i2s_port;
    i2s_config.tx_handle = tx_;
    data_if_ = audio_codec_new_i2s_data(&i2s_config);
    gpio_if_ = audio_codec_new_gpio();
    if (data_if_ == nullptr || gpio_if_ == nullptr) {
        return FailInitialization(ESP_ERR_NO_MEM);
    }
    es8311_codec_cfg_t codec_config{};
    codec_config.ctrl_if = &control_.interface;
    codec_config.gpio_if = gpio_if_;
    codec_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec_config.pa_pin = config_.amplifier_enable;
    codec_config.pa_reverted = config_.amplifier_active_low;
    codec_config.master_mode = false;
    codec_config.use_mclk = true;
    codec_config.hw_gain.pa_voltage = config_.amplifier_voltage;
    codec_config.hw_gain.codec_dac_voltage = config_.codec_dac_voltage;
    codec_config.mclk_div = kMclkMultiple;
    codec_if_ = es8311_codec_new(&codec_config);
    if (codec_if_ == nullptr) {
        return FailInitialization(ESP_FAIL);
    }
    esp_codec_dev_cfg_t device_config{};
    device_config.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    device_config.codec_if = codec_if_;
    device_config.data_if = data_if_;
    codec_ = esp_codec_dev_new(&device_config);
    if (codec_ == nullptr) {
        return FailInitialization(ESP_ERR_NO_MEM);
    }
    esp_codec_dev_sample_info_t sample_config{};
    sample_config.bits_per_sample = 16U;
    sample_config.channel = 2U;
    sample_config.channel_mask = 0x03U;
    sample_config.sample_rate = config_.sample_rate;
    sample_config.mclk_multiple = kMclkMultiple;
    // esp_codec_dev 1.x reconfigures by disabling the channel without checking
    // its initial state. Prime it once so that operation remains a clean state
    // transition on IDF's strict I2S driver.
    status = i2s_channel_enable(tx_);
    if (status != ESP_OK) {
        return FailInitialization(status);
    }
    output_enabled_ = true;
    if (esp_codec_dev_open(codec_, &sample_config) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(codec_, 100) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(codec_, true) != ESP_CODEC_DEV_OK) {
        return FailInitialization(ESP_FAIL);
    }
    status = SetAmplifier(false);
    if (status != ESP_OK || data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, false) != ESP_CODEC_DEV_OK) {
        return FailInitialization(status == ESP_OK ? ESP_FAIL : status);
    }
    output_enabled_ = false;
    ESP_LOGI(config_.log_tag, "%s ready at I2C 0x%02x, %lu Hz stereo I2S", config_.name, config_.codec_i2c_address,
             static_cast<unsigned long>(config_.sample_rate));
    return ESP_OK;
}

esp_err_t Es8311I2sAudioSink::Start(int32_t* scratch_frames, uint32_t frame_count) {
    if (codec_ == nullptr || data_if_ == nullptr || scratch_frames == nullptr || frame_count == 0U ||
        frame_count > kMaximumChunkFrames) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, true) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    output_enabled_ = true;
    std::memset(scratch_frames, 0, static_cast<size_t>(frame_count) * 2U * sizeof(*scratch_frames));
    esp_err_t status = Write(scratch_frames, frame_count);
    if (status == ESP_OK && esp_codec_dev_set_out_mute(codec_, false) != ESP_CODEC_DEV_OK) {
        status = ESP_FAIL;
    }
    if (status == ESP_OK) {
        status = SetAmplifier(true);
    }
    const uint32_t preroll_chunks =
        (config_.amplifier_preroll_ms * config_.sample_rate + frame_count * 1000U - 1U) / (frame_count * 1000U);
    for (uint32_t chunk = 0U; status == ESP_OK && chunk < preroll_chunks; ++chunk) {
        status = Write(scratch_frames, frame_count);
    }
    if (status != ESP_OK) {
        (void)SetAmplifier(false);
        (void)esp_codec_dev_set_out_mute(codec_, true);
        if (data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, false) == ESP_CODEC_DEV_OK) {
            output_enabled_ = false;
        }
    }
    return status;
}

esp_err_t Es8311I2sAudioSink::Write(const int32_t* frames, uint32_t frame_count) {
    if (tx_ == nullptr || converted_frames_ == nullptr || frames == nullptr || frame_count == 0U ||
        frame_count > kMaximumChunkFrames) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint32_t index = 0U; index < frame_count * 2U; ++index) {
        converted_frames_[index] = static_cast<int16_t>(frames[index] >> 16U);
    }
    const size_t bytes = static_cast<size_t>(frame_count) * 2U * sizeof(converted_frames_[0]);
    size_t written = 0U;
    const esp_err_t status = i2s_channel_write(tx_, converted_frames_, bytes, &written, pdMS_TO_TICKS(500));
    return status == ESP_OK && written == bytes ? ESP_OK : (status == ESP_OK ? ESP_FAIL : status);
}

esp_err_t Es8311I2sAudioSink::Stop() {
    if (codec_ == nullptr || data_if_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t amplifier_status = SetAmplifier(false);
    const int mute_status = esp_codec_dev_set_out_mute(codec_, true);
    const int disable_status = data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, false);
    if (disable_status == ESP_CODEC_DEV_OK) {
        output_enabled_ = false;
    }
    if (amplifier_status != ESP_OK || mute_status != ESP_CODEC_DEV_OK || disable_status != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void Es8311I2sAudioSink::Shutdown() {
    if (config_.amplifier_enable != GPIO_NUM_NC || config_.amplifier_setter != nullptr) {
        (void)SetAmplifier(false);
    }
    if (output_enabled_ && data_if_ != nullptr &&
        data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, false) == ESP_CODEC_DEV_OK) {
        output_enabled_ = false;
    }
    if (codec_ != nullptr) {
        (void)esp_codec_dev_set_out_mute(codec_, true);
        (void)esp_codec_dev_close(codec_);
        esp_codec_dev_delete(codec_);
        codec_ = nullptr;
    }
    if (codec_if_ != nullptr) {
        (void)audio_codec_delete_codec_if(codec_if_);
        codec_if_ = nullptr;
    }
    if (data_if_ != nullptr) {
        (void)audio_codec_delete_data_if(data_if_);
        data_if_ = nullptr;
    }
    if (gpio_if_ != nullptr) {
        (void)audio_codec_delete_gpio_if(gpio_if_);
        gpio_if_ = nullptr;
    }
    if (tx_ != nullptr) {
        if (output_enabled_) {
            (void)i2s_channel_disable(tx_);
            output_enabled_ = false;
        }
        (void)i2s_del_channel(tx_);
        tx_ = nullptr;
    }
    if (converted_frames_ != nullptr) {
        heap_caps_free(converted_frames_);
        converted_frames_ = nullptr;
    }
    // Keep the board-lifetime I2C attachment/executor binding. Mosaico powers
    // the codec down while idle and must be able to Initialize() this sink
    // again without re-running Board::Initialize().
    control_.open = false;
}

}  // namespace micropixel::platform::audio
