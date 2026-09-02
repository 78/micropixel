#pragma once

#include "driver/i2c_master.h"
#include "platform/audio/audio_output_peripheral.hpp"
#include "platform/audio/es8311_i2s_audio_sink.hpp"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::szpi_esp32s3 {

class BoardHardware;

class I2sAudioSink final : public audio::AudioOutputPeripheral {
   public:
    explicit I2sAudioSink(BoardHardware& hardware);

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

   private:
    static esp_err_t SetAmplifier(void* context, bool enabled);

    BoardHardware& hardware_;
    buses::I2cExecutor* executor_{};
    audio::Es8311I2sAudioSink sink_;
};

}  // namespace micropixel::platform::szpi_esp32s3
