#pragma once

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "platform/audio/audio_output_peripheral.hpp"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::metalio_claw4 {

class I2sAudioSink final : public audio::AudioOutputPeripheral {
   public:
    void Configure(i2c_master_dev_handle_t io_expander, buses::I2cExecutor& i2c_executor);
    [[nodiscard]] esp_err_t Initialize() override;
    [[nodiscard]] esp_err_t Start(int32_t* scratch_frames, uint32_t frame_count) override;
    [[nodiscard]] esp_err_t Write(const int32_t* frames, uint32_t frame_count) override;
    [[nodiscard]] esp_err_t Stop() override;
    void Shutdown() override;
    [[nodiscard]] const char* Name() const override { return "Claw4 I2S/BT"; }

   private:
    [[nodiscard]] esp_err_t UpdateRegister(uint8_t address, uint8_t set_mask, uint8_t clear_mask);
    [[nodiscard]] esp_err_t WriteSilence(int32_t* frames, uint32_t frame_count);

    i2c_master_dev_handle_t io_expander_{};
    buses::I2cExecutor* i2c_executor_{};
    i2s_chan_handle_t tx_{};
};

}  // namespace micropixel::platform::metalio_claw4
