#include "platform/transports/usb_serial_jtag_local_control.hpp"

#include <array>
#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/log_lock.h"
#include "freertos/idf_additions.h"
#include "work/task_policy.hpp"

namespace micropixel::platform::transports {
namespace {

constexpr char kTag[] = "usb_local_control";
constexpr char kProtocolPrefix[] = "MPX1 ";
// APP_LIST serializes a catalog page through mbedTLS Base64 before it is
// copied into the fixed response queue. Keep enough internal-RAM stack for
// that deepest command path; the command and catalog workspaces stay in
// PSRAM.
constexpr uint32_t kTaskStackSize = 10U * 1024U;
constexpr BaseType_t kTaskCore = 0;

}  // namespace

esp_err_t UsbSerialJtagLocalControl::Start(DevelopmentCommandSink development_sink, void* development_context) {
    if (task_ != nullptr) {
        return ESP_OK;
    }
    development_sink_ = development_sink;
    development_context_ = development_context;
    if (command_ == nullptr) {
        command_ =
            static_cast<char*>(heap_caps_calloc(kCommandCapacity, sizeof(char), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (command_ == nullptr) {
            ESP_LOGE(kTag, "command parser requires %zu bytes of PSRAM", kCommandCapacity);
            return ESP_ERR_NO_MEM;
        }
        line_framer_.Bind(command_, kCommandCapacity);
    }
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        config.rx_buffer_size = 8192U;
        config.tx_buffer_size = 4096U;
        const esp_err_t status = usb_serial_jtag_driver_install(&config);
        if (status != ESP_OK) {
            return status;
        }
    }
    if (xTaskCreatePinnedToCore(TaskEntry, "micropixel_usb", kTaskStackSize, this,
                                task_policy::kUsbLocalControlPriority, &task_, kTaskCore) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    usb_task_.store(task_, std::memory_order_release);
    usb_serial_jtag_set_select_notif_callback(UsbDriverEvent);
    xTaskNotifyGive(task_);
    ESP_LOGI(kTag, "USB Serial/JTAG transport ready; protocol=MPX1, command_buffer=%zu", kCommandCapacity);
    return ESP_OK;
}

void UsbSerialJtagLocalControl::Bind(device::LocalControlCommandSink command_sink,
                                     device::LocalControlResponseSource response_source, void* context) {
    command_context_.store(context, std::memory_order_release);
    response_source_.store(response_source, std::memory_order_release);
    command_sink_.store(command_sink, std::memory_order_release);
}

void UsbSerialJtagLocalControl::Unbind(void* context) {
    if (command_context_.load(std::memory_order_acquire) != context) {
        return;
    }
    command_sink_.store(nullptr, std::memory_order_release);
    response_source_.store(nullptr, std::memory_order_release);
    command_context_.store(nullptr, std::memory_order_release);
}

void UsbSerialJtagLocalControl::NotifyResponseReady() {
    TaskHandle_t task = usb_task_.load(std::memory_order_acquire);
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

void UsbSerialJtagLocalControl::LockOutput() {
    esp_log_impl_lock();
    FlushOutput(1000U);
}

bool UsbSerialJtagLocalControl::WriteAll(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (size > 0U) {
        const size_t chunk = size > 1024U ? 1024U : size;
        const int written = usb_serial_jtag_write_bytes(bytes, chunk, pdMS_TO_TICKS(1000));
        if (written <= 0) {
            return false;
        }
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

void UsbSerialJtagLocalControl::FlushOutput(uint32_t timeout_ms) {
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(timeout_ms));
}

void UsbSerialJtagLocalControl::UnlockOutput() { esp_log_impl_unlock(); }

void UsbSerialJtagLocalControl::TaskEntry(void* context) { static_cast<UsbSerialJtagLocalControl*>(context)->Run(); }

void UsbSerialJtagLocalControl::UsbDriverEvent(usj_select_notif_t event, int* task_woken) {
    if (event != USJ_SELECT_READ_NOTIF) {
        return;
    }
    TaskHandle_t task = usb_task_.load(std::memory_order_acquire);
    if (task == nullptr) {
        return;
    }
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    if (task_woken != nullptr && higher_priority_task_woken == pdTRUE) {
        *task_woken = pdTRUE;
    }
}

void UsbSerialJtagLocalControl::Run() {
    std::array<uint8_t, 512U> bytes{};
    for (;;) {
        // Both USB RX interrupts and response producers use a counting task
        // notification. Counts survive until consumed, so this wait can remain
        // event-driven without a timer fallback or a lost-wakeup window.
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int received = 0;
        do {
            received = usb_serial_jtag_read_bytes(bytes.data(), bytes.size(), 0U);
            if (received > 0) {
                line_framer_.Consume(bytes.data(), static_cast<size_t>(received),
                                     [this](const char* command) { ProcessCommand(command); });
            }
            DrainResponses();
        } while (received > 0);
    }
}

void UsbSerialJtagLocalControl::ProcessCommand(const char* command) {
    if (std::strncmp(command, kProtocolPrefix, sizeof(kProtocolPrefix) - 1U) == 0) {
        const device::LocalControlCommandSink sink = command_sink_.load(std::memory_order_acquire);
        void* context = command_context_.load(std::memory_order_acquire);
        if (sink != nullptr && context != nullptr) {
            sink(context, command);
        }
        return;
    }
    if (development_sink_ != nullptr) {
        development_sink_(development_context_, command);
    }
}

void UsbSerialJtagLocalControl::DrainResponses() {
    const device::LocalControlResponseSource source = response_source_.load(std::memory_order_acquire);
    void* context = command_context_.load(std::memory_order_acquire);
    if (source == nullptr || context == nullptr) {
        return;
    }
    std::array<char, kResponseCapacity> response{};
    while (source(context, response.data(), response.size())) {
        const size_t length = ::strnlen(response.data(), response.size());
        if (length == 0U || length == response.size()) {
            continue;
        }
        LockOutput();
        const char newline = '\n';
        const bool written = WriteAll(&newline, sizeof(newline)) && WriteAll(response.data(), length) &&
                             WriteAll(&newline, sizeof(newline));
        FlushOutput(1000U);
        UnlockOutput();
        if (!written) {
            ESP_LOGW(kTag, "local control response write failed");
            return;
        }
        response = {};
    }
}

}  // namespace micropixel::platform::transports
