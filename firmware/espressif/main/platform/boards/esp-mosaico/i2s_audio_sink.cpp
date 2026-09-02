#include "platform/boards/esp-mosaico/i2s_audio_sink.hpp"

#include "platform/boards/esp-mosaico/board_config.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::esp_mosaico {

I2sAudioSink::I2sAudioSink()
    : sink_(
          {
              .name = "Mosaico ES8311/I2S",
              .log_tag = "mosaico_audio",
              .i2c_port = I2C_NUM_0,
              .i2s_port = I2S_NUM_0,
              .master_clock = board::kAudioMasterClock,
              .bit_clock = board::kAudioBitClock,
              .word_select = board::kAudioWordSelect,
              .data_out = board::kAudioDataOut,
              .amplifier_enable = board::kAudioAmplifierEnable,
              .codec_i2c_address = board::kAudioCodecI2cAddress,
              .sample_rate = 16000U,
              .i2c_clock_hz = 100000U,
              .amplifier_preroll_ms = 24U,
              .dma_descriptor_count = 6U,
              .dma_frame_count = 240U,
              .amplifier_active_low = false,
          },
          {.amplifier_voltage = 3.3F, .codec_dac_voltage = 3.3F}) {}

esp_err_t I2sAudioSink::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    return sink_.Configure(bus, executor);
}

}  // namespace micropixel::platform::esp_mosaico
