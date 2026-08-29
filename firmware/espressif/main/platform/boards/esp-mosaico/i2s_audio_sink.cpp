#include "platform/boards/esp-mosaico/i2s_audio_sink.hpp"

#include <algorithm>
#include <cstring>

#include "audio_codec_ctrl_if.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_audio";
constexpr uint32_t kSampleRate = 16000U;
constexpr uint32_t kMclkMultiple = 256U;
constexpr uint32_t kAmplifierPrerollMs = 24U;
constexpr int kI2cTimeoutMs = 100;

struct CodecControl final {
    audio_codec_ctrl_if_t interface{};
    buses::I2cExecutor* executor{};
    i2c_master_dev_handle_t device{};
    bool open{};
};

CodecControl& Control(const audio_codec_ctrl_if_t* interface) {
    return *reinterpret_cast<CodecControl*>(const_cast<audio_codec_ctrl_if_t*>(interface));
}

int ControlOpen(const audio_codec_ctrl_if_t* interface, void*, int) {
    CodecControl& control = Control(interface);
    control.open = control.executor != nullptr && control.device != nullptr;
    return control.open ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRONG_STATE;
}

bool ControlIsOpen(const audio_codec_ctrl_if_t* interface) { return interface != nullptr && Control(interface).open; }

int ControlRead(const audio_codec_ctrl_if_t* interface, int reg, int reg_len, void* data, int data_len) {
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

int ControlWrite(const audio_codec_ctrl_if_t* interface, int reg, int reg_len, void* data, int data_len) {
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

int ControlGetInfo(const audio_codec_ctrl_if_t* interface, audio_codec_ctrl_info_t* info) {
    if (interface == nullptr || info == nullptr) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    *info = {};
    info->type = AUDIO_CODEC_CTRL_I2C;
    info->i2c.addr = static_cast<uint16_t>(board::kAudioCodecI2cAddress << 1U);
    info->i2c.port = I2C_NUM_0;
    return ESP_CODEC_DEV_OK;
}

int ControlClose(const audio_codec_ctrl_if_t* interface) {
    if (interface == nullptr) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    Control(interface).open = false;
    return ESP_CODEC_DEV_OK;
}

CodecControl& SharedCodecControl() {
    static CodecControl control{
        .interface =
            {
                .open = ControlOpen,
                .is_open = ControlIsOpen,
                .read_reg = ControlRead,
                .write_reg = ControlWrite,
                .get_info = ControlGetInfo,
                .close = ControlClose,
            },
    };
    return control;
}

esp_err_t ConfigureOutput(gpio_num_t pin, int level) {
    gpio_config_t config{};
    config.pin_bit_mask = BIT64(pin);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, level), kTag, "preset audio GPIO%d failed", static_cast<int>(pin));
    return gpio_config(&config);
}

}  // namespace

esp_err_t I2sAudioSink::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = board::kAudioCodecI2cAddress;
    device_config.scl_speed_hz = 100000U;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &codec_i2c_), kTag,
                        "attach ES8311 control bus failed");
    i2c_executor_ = &executor;
    CodecControl& control = SharedCodecControl();
    control.executor = &executor;
    control.device = codec_i2c_;
    control.open = true;
    return ESP_OK;
}

esp_err_t I2sAudioSink::Initialize() {
    if (i2c_executor_ == nullptr || codec_i2c_ == nullptr || tx_ != nullptr || codec_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ConfigureOutput(board::kAudioAmplifierEnable, 0), kTag, "mute NS4150B failed");

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6U;
    channel_config.dma_frame_num = 240U;
    channel_config.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &tx_, nullptr), kTag, "create Mosaico I2S channel failed");
    i2s_std_config_t standard_config{};
    standard_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    standard_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    standard_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    standard_config.gpio_cfg.mclk = board::kAudioMasterClock;
    standard_config.gpio_cfg.bclk = board::kAudioBitClock;
    standard_config.gpio_cfg.ws = board::kAudioWordSelect;
    standard_config.gpio_cfg.dout = board::kAudioDataOut;
    standard_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_, &standard_config), kTag, "configure Mosaico I2S failed");

    audio_codec_i2s_cfg_t i2s_config{};
    i2s_config.port = I2S_NUM_0;
    i2s_config.tx_handle = tx_;
    data_if_ = audio_codec_new_i2s_data(&i2s_config);
    gpio_if_ = audio_codec_new_gpio();
    if (data_if_ == nullptr || gpio_if_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    es8311_codec_cfg_t codec_config{};
    codec_config.ctrl_if = &SharedCodecControl().interface;
    codec_config.gpio_if = gpio_if_;
    codec_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    codec_config.pa_pin = board::kAudioAmplifierEnable;
    codec_config.pa_reverted = false;
    codec_config.master_mode = false;
    codec_config.use_mclk = true;
    codec_config.hw_gain.pa_voltage = 3.3F;
    codec_config.hw_gain.codec_dac_voltage = 3.3F;
    codec_config.mclk_div = kMclkMultiple;
    codec_if_ = es8311_codec_new(&codec_config);
    if (codec_if_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t device_config{};
    device_config.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    device_config.codec_if = codec_if_;
    device_config.data_if = data_if_;
    codec_ = esp_codec_dev_new(&device_config);
    if (codec_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_sample_info_t sample_config{};
    sample_config.bits_per_sample = 16U;
    sample_config.channel = 2U;
    sample_config.channel_mask = 0x03U;
    sample_config.sample_rate = kSampleRate;
    sample_config.mclk_multiple = kMclkMultiple;
    if (esp_codec_dev_open(codec_, &sample_config) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(codec_, 100) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(codec_, true) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(board::kAudioAmplifierEnable, 0), kTag,
                        "hold NS4150B disabled while audio is idle");
    if (data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, false) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(kTag, "ES8311 ready at I2C 0x%02x, 16 kHz stereo I2S", board::kAudioCodecI2cAddress);
    return ESP_OK;
}

esp_err_t I2sAudioSink::Start(int32_t* scratch_frames, uint32_t frame_count) {
    if (codec_ == nullptr || data_if_ == nullptr || scratch_frames == nullptr || frame_count == 0U ||
        frame_count > kMaximumChunkFrames) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, true) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    std::memset(scratch_frames, 0, static_cast<size_t>(frame_count) * 2U * sizeof(*scratch_frames));
    esp_err_t status = Write(scratch_frames, frame_count);
    if (status == ESP_OK && esp_codec_dev_set_out_mute(codec_, false) != ESP_CODEC_DEV_OK) {
        status = ESP_FAIL;
    }
    if (status == ESP_OK) {
        status = gpio_set_level(board::kAudioAmplifierEnable, 1);
    }
    const uint32_t preroll_chunks =
        (kAmplifierPrerollMs * kSampleRate + frame_count * 1000U - 1U) / (frame_count * 1000U);
    for (uint32_t chunk = 0U; status == ESP_OK && chunk < preroll_chunks; ++chunk) {
        status = Write(scratch_frames, frame_count);
    }
    if (status != ESP_OK) {
        (void)gpio_set_level(board::kAudioAmplifierEnable, 0);
        (void)esp_codec_dev_set_out_mute(codec_, true);
        (void)data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, false);
    }
    return status;
}

esp_err_t I2sAudioSink::Write(const int32_t* frames, uint32_t frame_count) {
    if (tx_ == nullptr || frames == nullptr || frame_count == 0U || frame_count > kMaximumChunkFrames) {
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

esp_err_t I2sAudioSink::Stop() {
    if (codec_ == nullptr || data_if_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t amplifier_status = gpio_set_level(board::kAudioAmplifierEnable, 0);
    const int mute_status = esp_codec_dev_set_out_mute(codec_, true);
    const int disable_status = data_if_->enable(data_if_, ESP_CODEC_DEV_TYPE_OUT, false);
    if (amplifier_status != ESP_OK || mute_status != ESP_CODEC_DEV_OK || disable_status != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void I2sAudioSink::Shutdown() {
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
        (void)i2s_channel_disable(tx_);
        (void)i2s_del_channel(tx_);
        tx_ = nullptr;
    }
    (void)gpio_set_level(board::kAudioAmplifierEnable, 0);
}

}  // namespace micropixel::platform::esp_mosaico
