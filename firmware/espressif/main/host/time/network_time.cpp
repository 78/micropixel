#include "host/time/network_time.hpp"

#include <cinttypes>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

namespace micropixel::firmware::network_time {
namespace {

constexpr char kTag[] = "network_time";

void TimeSynchronized(timeval* time) {
    if (time != nullptr) {
        ESP_LOGI(kTag, "system UTC synchronized by SNTP: seconds=%" PRId64, static_cast<int64_t>(time->tv_sec));
    }
}

}  // namespace

esp_err_t Initialize() {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_MICROPIXEL_NTP_SERVER);
    config.wait_for_sync = false;
    config.sync_cb = TimeSynchronized;
    const esp_err_t error = esp_netif_sntp_init(&config);
    if (error == ESP_OK) {
        ESP_LOGI(kTag, "Host SNTP started: server=%s", CONFIG_MICROPIXEL_NTP_SERVER);
    }
    return error;
}

}  // namespace micropixel::firmware::network_time
