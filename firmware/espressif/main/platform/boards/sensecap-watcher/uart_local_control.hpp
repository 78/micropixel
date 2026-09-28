// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef MICROPIXEL_PLATFORM_SENSECAP_WATCHER_UART_LOCAL_CONTROL_HPP
#define MICROPIXEL_PLATFORM_SENSECAP_WATCHER_UART_LOCAL_CONTROL_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/transports/ascii_line_framer.hpp"
#include "platform/transports/development_local_control.hpp"

namespace micropixel::platform::sensecap_watcher {

// Owns the board's UART byte stream and its line framing. Unlike every other
// board in this tree, the Watcher reaches the PC through an external USB-UART
// bridge (a WCH CH342 on the Type-C connector) instead of the SoC's own USB
// peripheral, so neither shared transport applies: UsbSerialJtagLocalControl
// drives the chip-internal USB Serial/JTAG and TinyUsbCdcLocalControl drives
// USB-OTG, and both stay silent on this wiring. MPX1 itself is unchanged -- the
// same LocalControlAgent, ControlDispatcher and AsciiLineFramer serve this
// stream, only the bytes' carrier differs.
//
// UART0 is also this board's console, so the protocol shares one port with
// ESP-IDF's log output. That is the same arrangement the USB boards have with
// their USB console: responses are written under the global log lock, and the
// CLI ignores log lines whose request id does not match.
class UartLocalControl final : public transports::DevelopmentLocalControlTransport {
   public:
    [[nodiscard]] esp_err_t Start(transports::DevelopmentCommandSink development_sink = nullptr,
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
    static constexpr size_t kResponseCapacity = 2048U;

    static void TaskEntry(void* context);
    void Run();
    void ProcessCommand(const char* command);
    void DrainResponses();

    std::atomic<device::LocalControlCommandSink> command_sink_{};
    std::atomic<device::LocalControlResponseSource> response_source_{};
    std::atomic<void*> command_context_{};
    transports::DevelopmentCommandSink development_sink_{};
    void* development_context_{};
    char* command_{};
    // Response scratch lives in PSRAM as well. DrainResponses hands this buffer
    // to a caller-owned LocalControlResponseSource, and a 2 KiB array on the
    // transport task's stack would break the frame-size limit the Host build
    // enforces for every task entry point and its callees.
    char* response_{};
    transports::AsciiLineFramer line_framer_{};
    TaskHandle_t task_{};
    // Response producers run on other tasks, so the handle is published through
    // an atomic exactly as the shared USB transport does.
    inline static std::atomic<TaskHandle_t> active_task_{};
};

}  // namespace micropixel::platform::sensecap_watcher

#endif  // MICROPIXEL_PLATFORM_SENSECAP_WATCHER_UART_LOCAL_CONTROL_HPP
