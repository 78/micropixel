#ifndef MICROPIXEL_RUNTIME_ENGINE_HPP
#define MICROPIXEL_RUNTIME_ENGINE_HPP

#include <expected>

namespace micropixel::device {
class DeviceServices;
}

namespace micropixel::runtime {

enum class EngineError {
    kPackageLoad,
    kLaunchBitmap,
    kRuntimeInitialization,
    kNativeApiRegistration,
    kModuleLoad,
    kGuestInstantiation,
    kExecEnvironment,
    kMissingEntry,
    kGuestServices,
    kConformanceTestHook,
    kWatchdogTimeout,
    kGuestTrap,
    kGuestExit,
};

class Engine final {
   public:
    explicit Engine(device::DeviceServices& devices) : devices_(devices) {}
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] std::expected<void, EngineError> Run();

   private:
    device::DeviceServices& devices_;
};

}  // namespace micropixel::runtime

#endif
