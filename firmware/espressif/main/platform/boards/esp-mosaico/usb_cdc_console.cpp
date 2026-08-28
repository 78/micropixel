#include "platform/boards/esp-mosaico/usb_cdc_console.hpp"

#include <cstdio>

#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
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
        if (gApplicationRestartTimer == nullptr || esp_timer_start_once(gApplicationRestartTimer, 50'000U) != ESP_OK) {
            esp_restart();
        }
        return;
    }
    if (request != UsbResetRequest::kDownload) {
        return;
    }

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

esp_err_t InitializeUsbCdcConsole() {
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

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
    usb_config.descriptor.string = gUsbStringDescriptors;
    usb_config.descriptor.string_count = sizeof(gUsbStringDescriptors) / sizeof(gUsbStringDescriptors[0]);
    error = tinyusb_driver_install(&usb_config);
    if (error != ESP_OK) {
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
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
        (void)tinyusb_driver_uninstall();
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
        return error;
    }

    error = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (error != ESP_OK) {
        (void)tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        (void)tinyusb_driver_uninstall();
        (void)esp_timer_delete(gApplicationRestartTimer);
        gApplicationRestartTimer = nullptr;
        (void)esp_timer_delete(gDownloadRestartTimer);
        gDownloadRestartTimer = nullptr;
        return error;
    }
    return ESP_OK;
}

}  // namespace micropixel::platform
