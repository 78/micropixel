// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "driver/i2c_master.h"
#include "platform/audio/audio_output_peripheral.hpp"
#include "platform/audio/es8311_i2s_audio_sink.hpp"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::sensecap_watcher {

// The Watcher's speaker path: an ES8311 DAC on I2S. The codec shares the control
// I2C bus with the PCA95xx expander, and its own rail - expander P1.4,
// kExpanderPinPowerCodecPa - is already latched by the power sequence, so this
// board has no amplifier line left to switch and leaves the shared sink's
// amplifier hooks unset, which its SetAmplifier() explicitly supports.
class I2sAudioSink final : public audio::AudioOutputPeripheral {
   public:
    I2sAudioSink();
    I2sAudioSink(const I2sAudioSink&) = delete;
    I2sAudioSink& operator=(const I2sAudioSink&) = delete;

    [[nodiscard]] esp_err_t Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor);
    [[nodiscard]] esp_err_t Initialize() override { return sink_.Initialize(); }
    [[nodiscard]] esp_err_t Start(int32_t* scratch_frames, uint32_t frame_count) override {
        return sink_.Start(scratch_frames, frame_count);
    }
    [[nodiscard]] esp_err_t Write(const int32_t* frames, uint32_t frame_count) override {
        return sink_.Write(frames, frame_count);
    }
    [[nodiscard]] esp_err_t Stop() override { return sink_.Stop(); }
    void Shutdown() override { sink_.Shutdown(); }
    [[nodiscard]] const char* Name() const override { return sink_.Name(); }
    [[nodiscard]] uint32_t SampleRate() const override { return sink_.SampleRate(); }

   private:
    audio::Es8311I2sAudioSink sink_;
};

}  // namespace micropixel::platform::sensecap_watcher
