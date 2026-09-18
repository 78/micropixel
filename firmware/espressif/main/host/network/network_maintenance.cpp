#include "host/network/network_maintenance.hpp"

namespace micropixel::host::network {
bool NetworkMaintenance::Start() {
    if (timer_) return true;
    const esp_timer_create_args_t args{.callback = Tick,
                                       .arg = this,
                                       .dispatch_method = ESP_TIMER_TASK,
                                       .name = "network_poll",
                                       .skip_unhandled_events = true};
    if (esp_timer_create(&args, &timer_) != ESP_OK) return false;
    if (esp_timer_start_periodic(timer_, 5000000) != ESP_OK) {
        (void)esp_timer_delete(timer_);
        timer_ = nullptr;
        return false;
    }
    return true;
}
NetworkMaintenance::~NetworkMaintenance() {
    if (!timer_) return;
    (void)esp_timer_stop_blocking(timer_, UINT32_MAX);
    (void)esp_timer_delete(timer_);
}
void NetworkMaintenance::Tick(void* context) { static_cast<NetworkMaintenance*>(context)->controller_.Poll(); }
}  // namespace micropixel::host::network
