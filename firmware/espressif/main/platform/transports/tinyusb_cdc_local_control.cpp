#include "platform/transports/tinyusb_cdc_local_control.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/log_lock.h"
#include "work/task_policy.hpp"

namespace micropixel::platform::transports {
namespace {

constexpr char kTag[] = "tinyusb_local_control";
constexpr char kProtocolPrefix[] = "MPX1 ";
constexpr size_t kReadCapacity = 512U;
constexpr uint32_t kTaskStackSize = 4U * 1024U;

}  // namespace

esp_err_t TinyUsbCdcLocalControl::Start(DevelopmentCommandSink development_sink, void* development_context) {
    if (task_ != nullptr) {
        return ESP_OK;
    }
    if (!tinyusb_cdcacm_initialized(interface_)) {
        return ESP_ERR_INVALID_STATE;
    }
    development_sink_ = development_sink;
    development_context_ = development_context;
    command_ =
        static_cast<char*>(heap_caps_calloc(kCommandCapacity, sizeof(char), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    io_workspace_ = static_cast<uint8_t*>(
        heap_caps_calloc(kResponseCapacity, sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (command_ == nullptr || io_workspace_ == nullptr) {
        heap_caps_free(command_);
        command_ = nullptr;
        heap_caps_free(io_workspace_);
        io_workspace_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    line_framer_.Bind(command_, kCommandCapacity);
    if (xTaskCreate(TaskEntry, "micropixel_cdc", kTaskStackSize, this, task_policy::kUsbLocalControlPriority, &task_) !=
        pdPASS) {
        heap_caps_free(command_);
        command_ = nullptr;
        heap_caps_free(io_workspace_);
        io_workspace_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    active_instance_.store(this, std::memory_order_release);
    const esp_err_t status = tinyusb_cdcacm_register_callback(interface_, CDC_EVENT_RX, ReceiveEvent);
    if (status != ESP_OK) {
        active_instance_.store(nullptr, std::memory_order_release);
        vTaskDelete(task_);
        task_ = nullptr;
        heap_caps_free(command_);
        command_ = nullptr;
        heap_caps_free(io_workspace_);
        io_workspace_ = nullptr;
        return status;
    }
    ESP_LOGI(kTag, "TinyUSB CDC transport ready; protocol=MPX1, command_buffer=%zu", kCommandCapacity);
    return ESP_OK;
}

void TinyUsbCdcLocalControl::Bind(device::LocalControlCommandSink command_sink,
                                  device::LocalControlResponseSource response_source, void* context) {
    command_context_.store(context, std::memory_order_release);
    response_source_.store(response_source, std::memory_order_release);
    command_sink_.store(command_sink, std::memory_order_release);
}

void TinyUsbCdcLocalControl::Unbind(void* context) {
    if (command_context_.load(std::memory_order_acquire) != context) {
        return;
    }
    command_sink_.store(nullptr, std::memory_order_release);
    response_source_.store(nullptr, std::memory_order_release);
    command_context_.store(nullptr, std::memory_order_release);
}

void TinyUsbCdcLocalControl::NotifyResponseReady() {
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
}

void TinyUsbCdcLocalControl::LockOutput() {
    esp_log_impl_lock();
    FlushOutput(1000U);
}

bool TinyUsbCdcLocalControl::WriteAll(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (size > 0U) {
        const size_t queued = tinyusb_cdcacm_write_queue(interface_, bytes, size);
        if (queued > 0U) {
            bytes += queued;
            size -= queued;
        }
        if (size > 0U && tinyusb_cdcacm_write_flush(interface_, pdMS_TO_TICKS(1000U)) != ESP_OK) {
            return false;
        }
    }
    return true;
}

void TinyUsbCdcLocalControl::FlushOutput(uint32_t timeout_ms) {
    (void)tinyusb_cdcacm_write_flush(interface_, pdMS_TO_TICKS(timeout_ms));
}

void TinyUsbCdcLocalControl::UnlockOutput() { esp_log_impl_unlock(); }

void TinyUsbCdcLocalControl::TaskEntry(void* context) { static_cast<TinyUsbCdcLocalControl*>(context)->Run(); }

void TinyUsbCdcLocalControl::ReceiveEvent(int interface_number, cdcacm_event_t* event) {
    TinyUsbCdcLocalControl* instance = active_instance_.load(std::memory_order_acquire);
    if (instance == nullptr || event == nullptr || event->type != CDC_EVENT_RX ||
        interface_number != static_cast<int>(instance->interface_) || instance->task_ == nullptr) {
        return;
    }
    xTaskNotifyGive(instance->task_);
}

void TinyUsbCdcLocalControl::Run() {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        size_t received = 0U;
        do {
            received = 0U;
            const esp_err_t status = tinyusb_cdcacm_read(interface_, io_workspace_, kReadCapacity, &received);
            if (status != ESP_OK) {
                ESP_LOGW(kTag, "TinyUSB CDC read failed: %s", esp_err_to_name(status));
                break;
            }
            if (received > 0U) {
                line_framer_.Consume(io_workspace_, received, [this](const char* command) { ProcessCommand(command); });
            }
            DrainResponses();
        } while (received > 0U);
    }
}

void TinyUsbCdcLocalControl::ProcessCommand(const char* command) {
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

void TinyUsbCdcLocalControl::DrainResponses() {
    const device::LocalControlResponseSource source = response_source_.load(std::memory_order_acquire);
    void* context = command_context_.load(std::memory_order_acquire);
    if (source == nullptr || context == nullptr) {
        return;
    }
    auto* response = reinterpret_cast<char*>(io_workspace_);
    std::memset(response, 0, kResponseCapacity);
    while (source(context, response, kResponseCapacity)) {
        const size_t length = ::strnlen(response, kResponseCapacity);
        if (length == 0U || length == kResponseCapacity) {
            continue;
        }
        LockOutput();
        const char newline = '\n';
        const bool written =
            WriteAll(&newline, sizeof(newline)) && WriteAll(response, length) && WriteAll(&newline, sizeof(newline));
        FlushOutput(1000U);
        UnlockOutput();
        if (!written) {
            ESP_LOGW(kTag, "local control response write failed");
            return;
        }
        std::memset(response, 0, kResponseCapacity);
    }
}

}  // namespace micropixel::platform::transports
