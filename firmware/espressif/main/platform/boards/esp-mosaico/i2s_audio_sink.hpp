#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_I2S_AUDIO_SINK_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_I2S_AUDIO_SINK_HPP

#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "platform/audio/audio_output_peripheral.hpp"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::esp_mosaico {

class I2sAudioSink final : public audio::AudioOutputPeripheral {
   public:
    [[nodiscard]] esp_err_t Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor);
    [[nodiscard]] esp_err_t Initialize() override;
    [[nodiscard]] esp_err_t Start(int32_t* scratch_frames, uint32_t frame_count) override;
    [[nodiscard]] esp_err_t Write(const int32_t* frames, uint32_t frame_count) override;
    [[nodiscard]] esp_err_t Stop() override;
    void Shutdown() override;
    [[nodiscard]] const char* Name() const override { return "Mosaico ES8311/I2S"; }

   private:
    static constexpr uint32_t kMaximumChunkFrames = 128U;

    buses::I2cExecutor* i2c_executor_{};
    i2c_master_dev_handle_t codec_i2c_{};
    i2s_chan_handle_t tx_{};
    const audio_codec_data_if_t* data_if_{};
    const audio_codec_gpio_if_t* gpio_if_{};
    const audio_codec_if_t* codec_if_{};
    esp_codec_dev_handle_t codec_{};
    int16_t converted_frames_[kMaximumChunkFrames * 2U]{};
};

}  // namespace micropixel::platform::esp_mosaico

#endif
