#pragma once

#include <cstdint>

#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "platform/audio/audio_output_peripheral.hpp"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::audio {

struct I2sCodecAudioConfig final {
    using AmplifierSetter = esp_err_t (*)(void* context, bool enabled);

    const char* name{};
    const char* log_tag{};
    int i2c_port{};
    int i2s_port{};
    gpio_num_t master_clock{GPIO_NUM_NC};
    gpio_num_t bit_clock{GPIO_NUM_NC};
    gpio_num_t word_select{GPIO_NUM_NC};
    gpio_num_t data_out{GPIO_NUM_NC};
    gpio_num_t amplifier_enable{GPIO_NUM_NC};
    AmplifierSetter amplifier_setter{};
    void* amplifier_context{};
    // Seven-bit address as required by i2c_master_bus_add_device().
    uint8_t codec_i2c_address{};
    uint32_t sample_rate{16000U};
    uint32_t i2c_clock_hz{100000U};
    uint32_t amplifier_preroll_ms{24U};
    uint32_t dma_descriptor_count{6U};
    uint32_t dma_frame_count{240U};
    uint8_t output_channels{2U};
    bool amplifier_active_low{};
    // Always-on boards can reject a missing codec before publishing audio.
    // Power-gated boards leave this false and probe when the rail is enabled.
    bool probe_before_attach{};
};

// Shared transport and lifecycle base for register-controlled I2S codecs.
// Concrete codecs only create their codec interface; this class owns I2C
// serialization, I2S DMA, sample conversion and esp_codec_dev lifetime.
class I2sCodecAudioSink : public AudioOutputPeripheral {
   public:
    explicit I2sCodecAudioSink(I2sCodecAudioConfig config) : config_(config) {}

    [[nodiscard]] esp_err_t Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor);
    [[nodiscard]] esp_err_t Initialize() override;
    [[nodiscard]] esp_err_t Start(int32_t* scratch_frames, uint32_t frame_count) override;
    [[nodiscard]] esp_err_t Write(const int32_t* frames, uint32_t frame_count) override;
    [[nodiscard]] esp_err_t Stop() override;
    void Shutdown() override;
    [[nodiscard]] const char* Name() const override { return config_.name; }

   protected:
    [[nodiscard]] virtual const audio_codec_if_t* CreateCodec(const audio_codec_ctrl_if_t* control,
                                                              const audio_codec_gpio_if_t* gpio) = 0;
    [[nodiscard]] const I2sCodecAudioConfig& Config() const { return config_; }

   private:
    static constexpr uint32_t kMaximumChunkFrames = 128U;

    struct CodecControl final {
        audio_codec_ctrl_if_t interface{};
        buses::I2cExecutor* executor{};
        i2c_master_dev_handle_t device{};
        uint8_t address{};
        int port{};
        bool open{};
    };

    [[nodiscard]] int AmplifierLevel(bool enabled) const;
    [[nodiscard]] esp_err_t SetAmplifier(bool enabled) const;
    [[nodiscard]] esp_err_t FailInitialization(esp_err_t status);
    static CodecControl& Control(const audio_codec_ctrl_if_t* interface);
    static int ControlOpen(const audio_codec_ctrl_if_t* interface, void*, int);
    static bool ControlIsOpen(const audio_codec_ctrl_if_t* interface);
    static int ControlRead(const audio_codec_ctrl_if_t* interface, int reg, int reg_len, void* data, int data_len);
    static int ControlWrite(const audio_codec_ctrl_if_t* interface, int reg, int reg_len, void* data, int data_len);
    static int ControlClose(const audio_codec_ctrl_if_t* interface);

    I2sCodecAudioConfig config_{};
    CodecControl control_{};
    i2c_master_dev_handle_t codec_i2c_{};
    i2s_chan_handle_t tx_{};
    const audio_codec_data_if_t* data_if_{};
    const audio_codec_gpio_if_t* gpio_if_{};
    const audio_codec_if_t* codec_if_{};
    esp_codec_dev_handle_t codec_{};
    bool codec_opened_{};
    bool output_enabled_{};
    int16_t* converted_frames_{};
};

}  // namespace micropixel::platform::audio
