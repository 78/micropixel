#pragma once

#include <cstddef>
#include <cstdint>

#include "device/contracts/local_control.hpp"
#include "esp_err.h"

namespace micropixel::platform::transports {

using DevelopmentCommandSink = void (*)(void* context, const char* command);

// USB development transport shared by board implementations. MPX1 remains on
// the hardware-independent LocalControl contract; framed binary output
// is deliberately kept in the Platform layer for diagnostics such as screen
// capture.
class DevelopmentLocalControlTransport : public device::LocalControl {
   public:
    [[nodiscard]] virtual esp_err_t Start(DevelopmentCommandSink development_sink = nullptr,
                                          void* development_context = nullptr) = 0;
    virtual void LockOutput() = 0;
    [[nodiscard]] virtual bool WriteAll(const void* data, size_t size) = 0;
    virtual void FlushOutput(uint32_t timeout_ms) = 0;
    virtual void UnlockOutput() = 0;

   protected:
    DevelopmentLocalControlTransport() = default;
};

}  // namespace micropixel::platform::transports
