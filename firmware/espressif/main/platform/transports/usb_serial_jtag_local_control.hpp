#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/usb_serial_jtag_select.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/transports/ascii_line_framer.hpp"
#include "platform/transports/development_local_control.hpp"

namespace micropixel::platform::transports {

// Owns the USB Serial/JTAG byte stream and line framing. MPX1 commands are
// forwarded through LocalControl; optional development commands (for
// example capture/touch injection) remain outside the product protocol.
class UsbSerialJtagLocalControl final : public DevelopmentLocalControlTransport {
   public:
    [[nodiscard]] esp_err_t Start(DevelopmentCommandSink development_sink = nullptr,
                                  void* development_context = nullptr) override;

    void Bind(device::LocalControlCommandSink command_sink, device::LocalControlResponseSource response_source,
              void* context) override;
    void Unbind(void* context) override;
    void NotifyResponseReady() override;

    // A development producer may stream a framed binary response while logs
    // are paused, preventing console output from corrupting the byte stream.
    void LockOutput() override;
    [[nodiscard]] bool WriteAll(const void* data, size_t size) override;
    void FlushOutput(uint32_t timeout_ms) override;
    void UnlockOutput() override;

   private:
    static constexpr size_t kCommandCapacity = 4608U;
    static constexpr size_t kResponseCapacity = 1024U;

    static void TaskEntry(void* context);
    static void UsbDriverEvent(usj_select_notif_t event, int* task_woken);
    void Run();
    void ProcessCommand(const char* command);
    void DrainResponses();

    std::atomic<device::LocalControlCommandSink> command_sink_{};
    std::atomic<device::LocalControlResponseSource> response_source_{};
    std::atomic<void*> command_context_{};
    DevelopmentCommandSink development_sink_{};
    void* development_context_{};
    char* command_{};
    AsciiLineFramer line_framer_{};
    TaskHandle_t task_{};
    inline static std::atomic<TaskHandle_t> usb_task_{};
};

}  // namespace micropixel::platform::transports
