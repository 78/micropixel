#ifndef MICROPIXEL_DEVICE_POWER_HPP
#define MICROPIXEL_DEVICE_POWER_HPP

#include <cstdint>
#include <expected>

namespace micropixel::device {

enum class PowerError {
    kUnavailable,
    kWakeSource,
    kDisplayPrepare,
    kDisplayShutdown,
    kSleepRejected,
    kDisplayRestore,
};

// Returns true only when Host accepted this click as the start of a new power
// request. The board driver uses the result to preserve the original press
// generation while a power transition is already in progress.
using PowerButtonSink = bool (*)(void* context, uint64_t timestamp_us);
using PowerOffButtonSink = bool (*)(void* context, uint64_t timestamp_us);

// Host-only board contract for the physical power key and one synchronous
// light-sleep cycle. Runtime and Guest services never access the board control
// mechanism directly.
class Power {
   public:
    virtual ~Power() = default;
    Power(const Power&) = delete;
    Power& operator=(const Power&) = delete;

    virtual void SetPowerButtonSink(PowerButtonSink sink, void* context) = 0;
    virtual void SetPowerOffButtonSink(PowerOffButtonSink sink, void* context) = 0;

    // Enters light sleep and returns after a configured wake source fires. The
    // implementation restores the display hardware before returning, but keeps
    // the backlight off so HostController can perform the wake fade.
    [[nodiscard]] virtual std::expected<void, PowerError> EnterLowPower() = 0;

    // Terminal board action. Implementations keep pulsing/retrying rather than
    // returning to Host after a failed physical cut.
    [[noreturn]] virtual void PowerOff() = 0;

   protected:
    Power() = default;
};

}  // namespace micropixel::device

#endif
