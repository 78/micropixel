#ifndef MICROPIXEL_PLATFORM_AUDIO_BACKEND_HPP
#define MICROPIXEL_PLATFORM_AUDIO_BACKEND_HPP

#include "device/audio.hpp"
#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform {

// Platform-only extension that keeps board startup out of the Device contract.
// The selected board initializes this backend with its shared I2C bus, while
// Runtime sees only device::AudioBackend.
class InitializableAudioBackend : public device::AudioBackend {
   public:
    [[nodiscard]] virtual esp_err_t Initialize(i2c_master_bus_handle_t i2c_bus) = 0;
};

[[nodiscard]] InitializableAudioBackend& ConfiguredAudioBackend();

}  // namespace micropixel::platform

#endif
