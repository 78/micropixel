#ifndef MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_AUDIO_BACKEND_HPP
#define MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_AUDIO_BACKEND_HPP

#include "device/audio.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform::common {
class I2cExecutor;
}

namespace micropixel::platform::metalio_claw4 {

// Board-local lifecycle extension. Runtime only receives the inherited
// device::AudioBackend contract; shared Platform code never sees Claw4 buses.
class AudioBackend : public device::AudioBackend {
   public:
    [[nodiscard]] virtual esp_err_t Initialize(i2c_master_dev_handle_t io_expander,
                                               common::I2cExecutor& i2c_executor) = 0;
    virtual void SetMasterVolumePercent(uint8_t percent) = 0;
};

[[nodiscard]] AudioBackend& ConfiguredAudioBackend();

}  // namespace micropixel::platform::metalio_claw4

#endif
