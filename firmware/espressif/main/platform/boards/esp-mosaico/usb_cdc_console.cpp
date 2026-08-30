#include "platform/boards/esp-mosaico/usb_cdc_console.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/logging/system_log_buffer.hpp"
#include "platform/boards/esp-mosaico/usb_cdc_early_log_buffer.hpp"
#include "platform/boards/esp-mosaico/usb_download_reset_detector.hpp"
#include "soc/lp_system_struct.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"

namespace micropixel::platform {
namespace {

char kLanguageId[] = {0x09, 0x04};
char gUsbSerialNumber[13] = {};
const char* gUsbStringDescriptors[] = {
    kLanguageId, "Espressif Systems", "MicroPixel ESP-Mosaico", gUsbSerialNumber, "MicroPixel CDC Console",
};
UsbDownloadResetDetector gDownloadResetDetector;
esp_timer_handle_t gApplicationRestartTimer = nullptr;
esp_timer_handle_t gDownloadRestartTimer = nullptr;
esp_timer_handle_t gEarlyLogReplayTimer = nullptr;
esp_timer_handle_t gEarlyLogExpiryTimer = nullptr;

constexpr size_t kEarlyLogCapacity = 32U * 1024U;
constexpr uint64_t kEarlyLogLifetimeUs = 10U * 1000U * 1000U;

struct HeapCapsDeleter final {
    void operator()(char* storage) const { heap_caps_free(storage); }
};

using EarlyLogStorage = std::unique_ptr<char[], HeapCapsDeleter>;
esp_mosaico::UsbCdcEarlyLogBuffer gEarlyLogBuffer;
EarlyLogStorage gEarlyLogStorage;
portMUX_TYPE gEarlyLogLock = portMUX_INITIALIZER_UNLOCKED;
bool gEarlyLogCaptureActive = false;
bool gEarlyLogReplayScheduled = false;
bool gSystemLogSubscribed = false;

void CaptureEarlyLog(void*, const firmware::logging::LogRecordView& record) {
    portENTER_CRITICAL(&gEarlyLogLock);
    if (gEarlyLogCaptureActive) {
        if (record.source == firmware::logging::LogSource::kApp) {
            constexpr std::string_view kAppPrefix = "[APP ";
            constexpr std::string_view kAppSeparator = "] ";
            constexpr std::string_view kNewline = "\n";
            gEarlyLogBuffer.Append(std::span<const char>(kAppPrefix));
            gEarlyLogBuffer.Append(std::span<const char>(record.app_id));
            gEarlyLogBuffer.Append(std::span<const char>(kAppSeparator));
            gEarlyLogBuffer.Append(std::span<const char>(record.message));
            gEarlyLogBuffer.Append(std::span<const char>(kNewline));
        } else {
            gEarlyLogBuffer.Append(std::span<const char>(record.message));
        }
    }
    portEXIT_CRITICAL(&gEarlyLogLock);
}

void StopEarlyLogCapture() {
    if (gEarlyLogReplayTimer != nullptr) {
        (void)esp_timer_stop(gEarlyLogReplayTimer);
    }
    if (gEarlyLogExpiryTimer != nullptr) {
        (void)esp_timer_stop(gEarlyLogExpiryTimer);
    }
    portENTER_CRITICAL(&gEarlyLogLock);
    const bool was_active = gEarlyLogCaptureActive;
    gEarlyLogCaptureActive = false;
    EarlyLogStorage storage = std::move(gEarlyLogStorage);
    gEarlyLogBuffer.Release();
    portEXIT_CRITICAL(&gEarlyLogLock);
    (void)was_active;
}

void ReplayEarlyLog(void*) {
    if (gEarlyLogExpiryTimer != nullptr) {
        (void)esp_timer_stop(gEarlyLogExpiryTimer);
    }
    portENTER_CRITICAL(&gEarlyLogLock);
    if (!gEarlyLogCaptureActive) {
        portEXIT_CRITICAL(&gEarlyLogLock);
        return;
    }
    gEarlyLogCaptureActive = false;
    const auto segments = gEarlyLogBuffer.Segments();
    const size_t buffered_bytes = gEarlyLogBuffer.size();
    const size_t dropped_bytes = gEarlyLogBuffer.dropped_bytes();
    EarlyLogStorage storage = std::move(gEarlyLogStorage);
    portEXIT_CRITICAL(&gEarlyLogLock);

    std::array<char, 128U> heading{};
    const int heading_size = std::snprintf(heading.data(), heading.size(),
                                           "\r\n--- MicroPixel early boot log: %zu bytes, %zu dropped ---\r\n",
                                           buffered_bytes, dropped_bytes);
    if (heading_size > 0) {
        (void)std::fwrite(heading.data(), 1U, std::min(static_cast<size_t>(heading_size), heading.size() - 1U), stdout);
    }
    for (std::span<const char> segment : segments) {
        if (!segment.empty()) {
            (void)std::fwrite(segment.data(), 1U, segment.size(), stdout);
        }
    }
    constexpr char kFooter[] = "\r\n--- end MicroPixel early boot log ---\r\n";
    (void)std::fwrite(kFooter, 1U, sizeof(kFooter) - 1U, stdout);
    (void)std::fflush(stdout);

    portENTER_CRITICAL(&gEarlyLogLock);
    gEarlyLogBuffer.Release();
    portEXIT_CRITICAL(&gEarlyLogLock);
}

void ExpireEarlyLog(void*) { StopEarlyLogCapture(); }

bool StartEarlyLogCapture() {
    portENTER_CRITICAL(&gEarlyLogLock);
    const bool already_active = gEarlyLogCaptureActive;
    portEXIT_CRITICAL(&gEarlyLogLock);
    if (already_active) {
        return true;
    }

    EarlyLogStorage storage(
        static_cast<char*>(heap_caps_malloc(kEarlyLogCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (storage == nullptr) {
        return false;
    }
    if (!gSystemLogSubscribed) {
        gSystemLogSubscribed = firmware::logging::SystemLogs().Subscribe(CaptureEarlyLog, nullptr);
        if (!gSystemLogSubscribed) {
            return false;
        }
    }
    portENTER_CRITICAL(&gEarlyLogLock);
    gEarlyLogStorage = std::move(storage);
    gEarlyLogBuffer.Reset(std::span<char>(gEarlyLogStorage.get(), kEarlyLogCapacity));
    gEarlyLogCaptureActive = true;
    gEarlyLogReplayScheduled = false;
    portEXIT_CRITICAL(&gEarlyLogLock);
    return true;
}

void ArmEarlyLogExpiry() {
    portENTER_CRITICAL(&gEarlyLogLock);
    const bool capture_active = gEarlyLogCaptureActive;
    portEXIT_CRITICAL(&gEarlyLogLock);
    if (capture_active && gEarlyLogExpiryTimer != nullptr) {
        (void)esp_timer_stop(gEarlyLogExpiryTimer);
        if (esp_timer_start_once(gEarlyLogExpiryTimer, kEarlyLogLifetimeUs) != ESP_OK) {
            StopEarlyLogCapture();
        }
    }
}

void CancelEarlyLogReplay(bool resume_expiry) {
    if (gEarlyLogReplayTimer != nullptr) {
        (void)esp_timer_stop(gEarlyLogReplayTimer);
    }
    portENTER_CRITICAL(&gEarlyLogLock);
    gEarlyLogReplayScheduled = false;
    portEXIT_CRITICAL(&gEarlyLogLock);
    if (resume_expiry) {
        ArmEarlyLogExpiry();
    }
}

void ScheduleEarlyLogReplay() {
    portENTER_CRITICAL(&gEarlyLogLock);
    const bool should_schedule = gEarlyLogCaptureActive && !gEarlyLogReplayScheduled;
    if (should_schedule) {
        gEarlyLogReplayScheduled = true;
    }
    portEXIT_CRITICAL(&gEarlyLogLock);
    if (should_schedule && gEarlyLogExpiryTimer != nullptr) {
        (void)esp_timer_stop(gEarlyLogExpiryTimer);
    }
    if (should_schedule && gEarlyLogReplayTimer != nullptr &&
        esp_timer_start_once(gEarlyLogReplayTimer, 100'000U) != ESP_OK) {
        StopEarlyLogCapture();
    }
}

void RestartApplication(void*) { esp_restart(); }

void RestartInDownloadMode(void*) { esp_restart(); }

void OnUsbLineStateChanged(int interface_number, cdcacm_event_t* event) {
    if (interface_number != TINYUSB_CDC_ACM_0 || event == nullptr || event->type != CDC_EVENT_LINE_STATE_CHANGED) {
        return;
    }

    const auto& state = event->line_state_changed_data;
    const UsbResetRequest request =
        gDownloadResetDetector.Observe(state.dtr, state.rts, static_cast<uint64_t>(esp_timer_get_time()));
    if (request == UsbResetRequest::kApplication) {
        CancelEarlyLogReplay(false);
        if (gApplicationRestartTimer == nullptr || esp_timer_start_once(gApplicationRestartTimer, 50'000U) != ESP_OK) {
            esp_restart();
        }
        return;
    }
    if (request != UsbResetRequest::kDownload) {
        if (state.dtr && !state.rts) {
            ScheduleEarlyLogReplay();
        } else if (state.dtr && state.rts) {
            // The local-control client holds both lines asserted. Cancel a
            // transient terminal-open state so replay cannot enter MPX1 data.
            CancelEarlyLogReplay(true);
        }
        return;
    }

    CancelEarlyLogReplay(false);

    // ESP32-S31 exposes USB-OTG on the onboard Type-C connector. Request ROM
    // download boot before resetting, then defer the restart so the current
    // CDC control transfer can complete and esptool can reopen the port.
    if (gApplicationRestartTimer != nullptr) {
        (void)esp_timer_stop(gApplicationRestartTimer);
    }
    LP_SYS.sys_ctrl.force_download_boot = 1;
    if (gDownloadRestartTimer == nullptr || esp_timer_start_once(gDownloadRestartTimer, 50'000U) != ESP_OK) {
        esp_restart();
    }
}

esp_err_t InitializeUsbIdentity() {
    uint8_t base_mac[6] = {};
    const esp_err_t error = esp_efuse_mac_get_default(base_mac);
    if (error != ESP_OK) {
        return error;
    }
    const int length = std::snprintf(gUsbSerialNumber, sizeof(gUsbSerialNumber), "%02X%02X%02X%02X%02X%02X",
                                     base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    return length == (sizeof(gUsbSerialNumber) - 1U) ? ESP_OK : ESP_FAIL;
}

}  // namespace

bool BeginUsbCdcEarlyLogCapture() { return StartEarlyLogCapture(); }

esp_err_t InitializeUsbCdcConsole() {
    (void)BeginUsbCdcEarlyLogCapture();
    esp_err_t error = InitializeUsbIdentity();
    if (error != ESP_OK) {
        return error;
    }

    const esp_timer_create_args_t application_restart_timer_config = {
        .callback = RestartApplication,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mosaico_restart",
        .skip_unhandled_events = true,
    };
    error = esp_timer_create(&application_restart_timer_config, &gApplicationRestartTimer);
    if (error != ESP_OK) {
        return error;
    }

    const esp_timer_create_args_t download_restart_timer_config = {
        .callback = RestartInDownloadMode,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mosaico_download",
        .skip_unhandled_events = true,
    };
    error = esp_timer_create(&download_restart_timer_config, &gDownloadRestartTimer);
    if (error != ESP_OK) {
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        return error;
    }

    const esp_timer_create_args_t early_log_replay_timer_config = {
        .callback = ReplayEarlyLog,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mosaico_log",
        .skip_unhandled_events = true,
    };
    error = esp_timer_create(&early_log_replay_timer_config, &gEarlyLogReplayTimer);
    if (error != ESP_OK) {
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
        return error;
    }

    const esp_timer_create_args_t early_log_expiry_timer_config = {
        .callback = ExpireEarlyLog,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mosaico_log_gc",
        .skip_unhandled_events = true,
    };
    error = esp_timer_create(&early_log_expiry_timer_config, &gEarlyLogExpiryTimer);
    if (error != ESP_OK) {
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
        (void)esp_timer_delete(gEarlyLogReplayTimer);
        gEarlyLogReplayTimer = nullptr;
        return error;
    }

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
    usb_config.descriptor.string = gUsbStringDescriptors;
    usb_config.descriptor.string_count = sizeof(gUsbStringDescriptors) / sizeof(gUsbStringDescriptors[0]);
    error = tinyusb_driver_install(&usb_config);
    if (error != ESP_OK) {
        StopEarlyLogCapture();
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
        (void)esp_timer_delete(gEarlyLogReplayTimer);
        gEarlyLogReplayTimer = nullptr;
        (void)esp_timer_delete(gEarlyLogExpiryTimer);
        gEarlyLogExpiryTimer = nullptr;
        return error;
    }

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = OnUsbLineStateChanged,
        .callback_line_coding_changed = nullptr,
    };
    error = tinyusb_cdcacm_init(&cdc_config);
    if (error != ESP_OK) {
        StopEarlyLogCapture();
        (void)tinyusb_driver_uninstall();
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
        (void)esp_timer_delete(gEarlyLogReplayTimer);
        gEarlyLogReplayTimer = nullptr;
        (void)esp_timer_delete(gEarlyLogExpiryTimer);
        gEarlyLogExpiryTimer = nullptr;
        return error;
    }

    error = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (error != ESP_OK) {
        StopEarlyLogCapture();
        (void)tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        (void)tinyusb_driver_uninstall();
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
        (void)esp_timer_delete(gEarlyLogReplayTimer);
        gEarlyLogReplayTimer = nullptr;
        (void)esp_timer_delete(gEarlyLogExpiryTimer);
        gEarlyLogExpiryTimer = nullptr;
        return error;
    }
    ArmEarlyLogExpiry();
    return ESP_OK;
}

}  // namespace micropixel::platform
