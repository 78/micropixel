// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/boards/sensecap-watcher/uart_local_control.hpp"

#include <array>
#include <cstring>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#include "freertos/idf_additions.h"
#endif
#include "platform/transports/log_output_lock.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform::sensecap_watcher {
namespace {

constexpr char kTag[] = "uart_local_control";
constexpr char kProtocolPrefix[] = "MPX1 ";
// The board's console UART. On the Watcher's Type-C connector the CH342 bridge
// channel that esptool flashes through lands here, so this is also COM3 on a
// Windows host.
constexpr uart_port_t kPort = UART_NUM_0;
constexpr int kBaudRate = 115200;
constexpr int kReceiveBufferBytes = 8192;
constexpr int kTransmitBufferBytes = 4096;
// Response producers wake the task directly. This is only the fallback poll
// that keeps UART receive flowing, and it is short enough that a CLI request
// never waits on it for long.
constexpr uint32_t kReceivePollMs = 20U;
// APP_LIST serializes a catalog page through mbedTLS Base64 before it is
// copied into the fixed response queue. The task never owns a flash operation,
// so both its large stack and command workspace can stay in PSRAM.
constexpr uint32_t kTaskStackSize = 10U * 1024U;
constexpr BaseType_t kTaskCore = task_policy::kSystemCore;

}  // namespace

esp_err_t UartLocalControl::Start(transports::DevelopmentCommandSink development_sink, void* development_context) {
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
    if (response_ == nullptr) {
        response_ =
            static_cast<char*>(heap_caps_calloc(kResponseCapacity, sizeof(char), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (response_ == nullptr) {
            ESP_LOGE(kTag, "response workspace requires %zu bytes of PSRAM", kResponseCapacity);
            return ESP_ERR_NO_MEM;
        }
    }
    // ESP-IDF installs this port's driver for its console during start-up
    // (esp_stdio -> uart_vfs_dev_port_init), so the driver is normally already
    // there and installing a second one would fail with ESP_ERR_INVALID_STATE.
    // Sharing it is also what keeps logs and the protocol on one port.
    //
    // The consequence is that the driver keeps the sizes the console asked for:
    // a 256-byte RX ring and no TX ring, so this transport cannot enlarge them.
    // That is safe for MPX1 because the CLI is strictly request/response and
    // waits for each answer before it sends the next line, and because the read
    // loop below drains the ring as fast as it fills.
    if (!uart_is_driver_installed(kPort)) {
        uart_config_t config{};
        config.baud_rate = kBaudRate;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.source_clk = UART_SCLK_DEFAULT;
        ESP_RETURN_ON_ERROR(uart_param_config(kPort, &config), kTag, "configure UART%d failed", kPort);
        ESP_RETURN_ON_ERROR(uart_driver_install(kPort, kReceiveBufferBytes, kTransmitBufferBytes, 0, nullptr, 0), kTag,
                            "install UART%d driver failed", kPort);
        ESP_LOGI(kTag, "UART%d driver installed at %d baud", kPort, kBaudRate);
    } else {
        uint32_t baud_rate = 0U;
        if (uart_get_baudrate(kPort, &baud_rate) == ESP_OK && baud_rate != static_cast<uint32_t>(kBaudRate)) {
            ESP_LOGW(kTag, "console UART%d runs at %u baud: the CLI assumes %d", kPort,
                     static_cast<unsigned>(baud_rate), kBaudRate);
        }
        ESP_LOGI(kTag, "reusing the console's UART%d driver", kPort);
    }
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const BaseType_t task_created = xTaskCreatePinnedToCoreWithCaps(TaskEntry, "micropixel_uart", kTaskStackSize, this,
                                                                    task_policy::kUsbLocalControlPriority, &task_,
                                                                    kTaskCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t task_created = xTaskCreatePinnedToCore(TaskEntry, "micropixel_uart", kTaskStackSize, this,
                                                            task_policy::kUsbLocalControlPriority, &task_, kTaskCore);
#endif
    if (task_created != pdPASS) {
        task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    active_task_.store(task_, std::memory_order_release);
    ESP_LOGI(kTag, "UART transport ready; protocol=MPX1, command_buffer=%zu", kCommandCapacity);
    return ESP_OK;
}

void UartLocalControl::Bind(device::LocalControlCommandSink command_sink,
                            device::LocalControlResponseSource response_source, void* context) {
    command_context_.store(context, std::memory_order_release);
    response_source_.store(response_source, std::memory_order_release);
    command_sink_.store(command_sink, std::memory_order_release);
}

void UartLocalControl::Unbind(void* context) {
    if (command_context_.load(std::memory_order_acquire) != context) {
        return;
    }
    command_sink_.store(nullptr, std::memory_order_release);
    response_source_.store(nullptr, std::memory_order_release);
    command_context_.store(nullptr, std::memory_order_release);
}

void UartLocalControl::NotifyResponseReady() {
    const TaskHandle_t task = active_task_.load(std::memory_order_acquire);
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

void UartLocalControl::LockOutput() {
    transports::LockLogOutput();
    FlushOutput(1000U);
}

bool UartLocalControl::WriteAll(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (size > 0U) {
        const size_t chunk = size > 1024U ? 1024U : size;
        const int written = uart_write_bytes(kPort, bytes, chunk);
        if (written <= 0) {
            return false;
        }
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

void UartLocalControl::FlushOutput(uint32_t timeout_ms) { (void)uart_wait_tx_done(kPort, pdMS_TO_TICKS(timeout_ms)); }

void UartLocalControl::UnlockOutput() { transports::UnlockLogOutput(); }

void UartLocalControl::TaskEntry(void* context) { static_cast<UartLocalControl*>(context)->Run(); }

void UartLocalControl::Run() {
    std::array<uint8_t, 512U> bytes{};
    for (;;) {
        // Two things wake this task: bytes arriving for the framer, and
        // responses pushed by Host tasks. The response path notifies directly,
        // but the UART driver offers no select-notification hook (and no event
        // queue, because the console installed the driver without one), so the
        // bounded wait is what keeps receive flowing. It is a blocking sleep in
        // the scheduler, not a busy poll, and it is short enough that a CLI
        // request is never held up noticeably.
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kReceivePollMs));
        int received = 0;
        do {
            received = uart_read_bytes(kPort, bytes.data(), bytes.size(), 0U);
            if (received > 0) {
                line_framer_.Consume(bytes.data(), static_cast<size_t>(received),
                                     [this](const char* command) { ProcessCommand(command); });
            }
            DrainResponses();
        } while (received > 0);
    }
}

void UartLocalControl::ProcessCommand(const char* command) {
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

void UartLocalControl::DrainResponses() {
    const device::LocalControlResponseSource source = response_source_.load(std::memory_order_acquire);
    void* context = command_context_.load(std::memory_order_acquire);
    if (source == nullptr || context == nullptr || response_ == nullptr) {
        return;
    }
    while (source(context, response_, kResponseCapacity)) {
        const size_t length = ::strnlen(response_, kResponseCapacity);
        if (length == 0U || length == kResponseCapacity) {
            continue;
        }
        LockOutput();
        const char newline = '\n';
        const bool written =
            WriteAll(&newline, sizeof(newline)) && WriteAll(response_, length) && WriteAll(&newline, sizeof(newline));
        FlushOutput(1000U);
        UnlockOutput();
        if (!written) {
            ESP_LOGW(kTag, "local control response write failed");
            return;
        }
        std::memset(response_, 0, kResponseCapacity);
    }
}

}  // namespace micropixel::platform::sensecap_watcher
