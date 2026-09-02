#include "platform/boards/m5stack-cores3/i2s_audio_sink.hpp"

#include "driver/gpio.h"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::m5stack_cores3 {

I2sAudioSink::I2sAudioSink()
    : sink_(
          {
              .name = "M5Stack CoreS3 AW88298/I2S",
              .log_tag = "cores3_audio",
              .i2c_port = I2C_NUM_0,
              .i2s_port = I2S_NUM_0,
              .master_clock = GPIO_NUM_0,
              .bit_clock = GPIO_NUM_34,
              .word_select = GPIO_NUM_33,
              .data_out = GPIO_NUM_13,
              .amplifier_enable = GPIO_NUM_NC,
              .codec_i2c_address = 0x36U,
              .sample_rate = 16000U,
              .i2c_clock_hz = 400000U,
              .amplifier_preroll_ms = 10U,
              .dma_descriptor_count = 6U,
              .dma_frame_count = 240U,
              .output_channels = 1U,
              .probe_before_attach = false,
          },
          {.pa_gain = 15}) {}

esp_err_t I2sAudioSink::Configure(i2c_master_bus_handle_t i2c_bus, buses::I2cExecutor& i2c_executor) {
    return sink_.Configure(i2c_bus, i2c_executor);
}

}  // namespace micropixel::platform::m5stack_cores3
