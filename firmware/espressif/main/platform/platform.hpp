#ifndef MICROPIXEL_PLATFORM_PLATFORM_HPP
#define MICROPIXEL_PLATFORM_PLATFORM_HPP

#include "device/audio.hpp"
#include "device/battery.hpp"
#include "device/graphics.hpp"
#include "device/input.hpp"
#include "device/power.hpp"
#include "device/random.hpp"
#include "device/wifi.hpp"
#include "esp_err.h"
#include "host_ui/system_ui.hpp"

namespace micropixel::platform {

// Application-level board boundary selected by CMake. It owns board startup
// order and exposes only hardware-independent Device contracts to FirmwareApp.
class Platform {
   public:
    virtual ~Platform() = default;
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    [[nodiscard]] virtual esp_err_t Initialize() = 0;
    [[nodiscard]] virtual device::GraphicsBackend& graphics() = 0;    // NOLINT(readability-identifier-naming)
    [[nodiscard]] virtual device::InputBackend& input() = 0;          // NOLINT(readability-identifier-naming)
    [[nodiscard]] virtual device::AudioBackend& audio() = 0;          // NOLINT(readability-identifier-naming)
    [[nodiscard]] virtual device::BatteryBackend& battery() = 0;      // NOLINT(readability-identifier-naming)
    [[nodiscard]] virtual device::RandomBackend& random() = 0;        // NOLINT(readability-identifier-naming)
    [[nodiscard]] virtual device::WifiBackend& wifi() = 0;            // NOLINT(readability-identifier-naming)
    [[nodiscard]] virtual device::PowerBackend& power() = 0;          // NOLINT(readability-identifier-naming)
    [[nodiscard]] virtual host_ui::SystemUiBackend& system_ui() = 0;  // NOLINT(readability-identifier-naming)

   protected:
    Platform() = default;
};

[[nodiscard]] Platform& ConfiguredPlatform();

}  // namespace micropixel::platform

#endif
