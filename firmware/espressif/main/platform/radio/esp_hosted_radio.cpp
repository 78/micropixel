#include "platform/radio/esp_hosted_radio.hpp"

#include <cinttypes>

#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_wifi.h"

namespace micropixel::platform::radio {
namespace {

constexpr char kTag[] = "micropixel_radio";

void LogCoprocessorInfo() {
    esp_hosted_coprocessor_fwver_t version{};
    const esp_err_t version_status = static_cast<esp_err_t>(esp_hosted_get_coprocessor_fwversion(&version));
    if (version_status == ESP_OK) {
        ESP_LOGI(kTag, "coprocessor firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32 " rev=%" PRId32, version.major1,
                 version.minor1, version.patch1, version.revision);
    } else {
        ESP_LOGW(kTag, "could not read coprocessor firmware version: %s", esp_err_to_name(version_status));
    }

    uint32_t chip_id = 0U;
    char target[32]{};
    const esp_err_t info_status = static_cast<esp_err_t>(esp_hosted_get_cp_info(&chip_id, target, sizeof(target)));
    if (info_status == ESP_OK) {
        ESP_LOGI(kTag, "coprocessor target=%s chip_id=0x%08" PRIx32, target[0] == '\0' ? "unknown" : target, chip_id);
    } else {
        ESP_LOGW(kTag, "could not read coprocessor info: %s", esp_err_to_name(info_status));
    }
}

}  // namespace

esp_err_t EspHostedRadio::Initialize() { return static_cast<esp_err_t>(esp_hosted_init()); }

esp_err_t EspHostedRadio::OnStationStarted() {
#if CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5
    const esp_err_t status = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (status != ESP_OK) {
        return status;
    }
#endif
    LogCoprocessorInfo();
    return ESP_OK;
}

}  // namespace micropixel::platform::radio
