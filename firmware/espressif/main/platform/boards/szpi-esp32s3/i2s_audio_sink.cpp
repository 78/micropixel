#include "platform/boards/szpi-esp32s3/i2s_audio_sink.hpp"

#include "esp_codec_dev_defaults.h"
#include "platform/boards/szpi-esp32s3/board_hardware.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::szpi_esp32s3 {

I2sAudioSink::I2sAudioSink(BoardHardware& hardware)
    : hardware_(hardware),
      sink_({
          .name = "SZPI ESP32-S3 ES8311/I2S",
          .log_tag = "szpi_s3_audio",
          .i2c_port = I2C_NUM_0,
          .i2s_port = I2S_NUM_0,
          .master_clock = GPIO_NUM_38,
          .bit_clock = GPIO_NUM_14,
          .word_select = GPIO_NUM_13,
          .data_out = GPIO_NUM_45,
          .amplifier_enable = GPIO_NUM_NC,
          .amplifier_setter = SetAmplifier,
          .amplifier_context = this,
          .codec_i2c_address = static_cast<uint8_t>(ES8311_CODEC_DEFAULT_ADDR >> 1U),
          .sample_rate = 16000U,
          .i2c_clock_hz = 100000U,
          .amplifier_preroll_ms = 24U,
          .dma_descriptor_count = 6U,
          .dma_frame_count = 240U,
          .amplifier_voltage = 3.3F,
          .codec_dac_voltage = 3.3F,
          .amplifier_active_low = false,
          .probe_before_attach = true,
      }) {}

esp_err_t I2sAudioSink::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    executor_ = &executor;
    return sink_.Configure(bus, executor);
}

esp_err_t I2sAudioSink::SetAmplifier(void* context, bool enabled) {
    auto* sink = static_cast<I2sAudioSink*>(context);
    if (sink == nullptr || sink->executor_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return sink->hardware_.SetAmplifier(enabled, *sink->executor_);
}

}  // namespace micropixel::platform::szpi_esp32s3
