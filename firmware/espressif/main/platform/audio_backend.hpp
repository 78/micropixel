#ifndef MICROPIXEL_PLATFORM_AUDIO_BACKEND_HPP
#define MICROPIXEL_PLATFORM_AUDIO_BACKEND_HPP

#include "device/audio.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform {

namespace metalio_claw4 {
class I2cExecutor;
}

// Platform-only extension that keeps board startup out of the Device contract.
// The selected board initializes this backend with its shared I/O expander,
// while Runtime sees only device::AudioBackend.
class InitializableAudioBackend : public device::AudioBackend {
   public:
    [[nodiscard]] virtual esp_err_t Initialize(i2c_master_dev_handle_t io_expander,
                                               metalio_claw4::I2cExecutor& i2c_executor) = 0;
    virtual void SetMasterVolumePercent(uint8_t percent) = 0;
};

[[nodiscard]] InitializableAudioBackend& ConfiguredAudioBackend();

}  // namespace micropixel::platform

#endif
