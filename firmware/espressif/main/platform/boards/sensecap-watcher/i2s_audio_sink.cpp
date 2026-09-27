// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/boards/sensecap-watcher/i2s_audio_sink.hpp"

#include "platform/boards/sensecap-watcher/board_config.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::sensecap_watcher {
namespace {

constexpr char kTag[] = "sensecap_watcher_audio";

}  // namespace

I2sAudioSink::I2sAudioSink()
    : sink_(
          {
              .name = "SenseCAP Watcher ES8311/I2S",
              .log_tag = kTag,
              .i2c_port = board::kControlI2cPort,
              .i2s_port = I2S_NUM_0,
              .master_clock = board::kAudioMclk,
              .bit_clock = board::kAudioBclk,
              .word_select = board::kAudioWs,
              // The codec receives on the pin the board file names for data into
              // it; the other direction carries the ES7243E microphone and is a
              // separate codec this sink does not drive.
              .data_out = board::kAudioDataIntoCodec,
              .amplifier_enable = GPIO_NUM_NC,
              .codec_i2c_address = board::kEs8311Address,
              .sample_rate = board::kAudioSampleRate,
              .i2c_clock_hz = board::kControlI2cClockHz,
              .amplifier_preroll_ms = 24U,
              .dma_descriptor_count = 6U,
              .dma_frame_count = 240U,
              .amplifier_active_low = false,
              .probe_before_attach = true,
          },
          // The vendor's own configuration for this board drives the amplifier
          // from a 5 V rail (Seeed-Studio/SenseCAP-Watcher-Firmware and the
          // xiaozhi board both set pa_voltage = 5.0 with codec_dac_voltage =
          // 3.3). Declaring 3.3 V, as the other ES8311 boards in this tree do,
          // makes esp_codec_dev push a higher DAC level than that analog stage
          // can carry: the sound is audible but clips, which reads as crackle.
          {.amplifier_voltage = 5.0F, .codec_dac_voltage = 3.3F}) {}

esp_err_t I2sAudioSink::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    return sink_.Configure(bus, executor);
}

}  // namespace micropixel::platform::sensecap_watcher
