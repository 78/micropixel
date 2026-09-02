#include "platform/boards/esp32-s3-box-3/i2s_audio_sink.hpp"

#include "esp_codec_dev_defaults.h"
#include "platform/boards/esp32-s3-box-3/board_hardware.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::esp32_s3_box_3 {

I2sAudioSink::I2sAudioSink()
    : sink_(
          {
              .name = "BOX-3 ES8311/I2S",
              .log_tag = "box3_audio",
              .i2c_port = kI2cPort,
              .i2s_port = kI2sPort,
              .master_clock = kI2sMasterClock,
              .bit_clock = kI2sBitClock,
              .word_select = kI2sWordSelect,
              .data_out = kI2sDataOut,
              .amplifier_enable = kAmplifierEnable,
              // esp_codec_dev publishes the legacy 8-bit wire address; the IDF
              // master-bus API takes the corresponding 7-bit device address.
              .codec_i2c_address = static_cast<uint8_t>(ES8311_CODEC_DEFAULT_ADDR >> 1U),
              .sample_rate = 16000U,
              .i2c_clock_hz = 100000U,
              .amplifier_preroll_ms = 24U,
              .dma_descriptor_count = 6U,
              .dma_frame_count = 240U,
              .amplifier_active_low = false,
              .probe_before_attach = true,
          },
          {.amplifier_voltage = 5.0F, .codec_dac_voltage = 3.3F}) {}

esp_err_t I2sAudioSink::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    return sink_.Configure(bus, executor);
}

}  // namespace micropixel::platform::esp32_s3_box_3
