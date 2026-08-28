#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/transports/ascii_line_framer.hpp"
#include "platform/transports/development_local_control.hpp"
#include "tinyusb_cdc_acm.h"

namespace micropixel::platform::transports {

// Event-driven local-control transport over an already initialized TinyUSB CDC
// interface. ESP-Mosaico shares this endpoint with the application console.
class TinyUsbCdcLocalControl final : public DevelopmentLocalControlTransport {
   public:
    explicit TinyUsbCdcLocalControl(tinyusb_cdcacm_itf_t interface = TINYUSB_CDC_ACM_0) : interface_(interface) {}

    [[nodiscard]] esp_err_t Start(DevelopmentCommandSink development_sink = nullptr,
                                  void* development_context = nullptr) override;

    void Bind(device::LocalControlCommandSink command_sink, device::LocalControlResponseSource response_source,
              void* context) override;
    void Unbind(void* context) override;
    void NotifyResponseReady() override;

    void LockOutput() override;
    [[nodiscard]] bool WriteAll(const void* data, size_t size) override;
    void FlushOutput(uint32_t timeout_ms) override;
    void UnlockOutput() override;

   private:
    static constexpr size_t kCommandCapacity = 4608U;
    static constexpr size_t kResponseCapacity = 1024U;

    static void TaskEntry(void* context);
    static void ReceiveEvent(int interface_number, cdcacm_event_t* event);
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
    tinyusb_cdcacm_itf_t interface_{};
    inline static std::atomic<TinyUsbCdcLocalControl*> active_instance_{};
};

}  // namespace micropixel::platform::transports
