#pragma once
#include "esp_timer.h"
#include "host/network/network_controller.hpp"

namespace micropixel::host::network {
// Owns only the process-lifetime maintenance timer, not a worker task.
class NetworkMaintenance final {
   public:
    explicit NetworkMaintenance(NetworkController& controller) : controller_(controller) {}
    ~NetworkMaintenance();
    NetworkMaintenance(const NetworkMaintenance&) = delete;
    NetworkMaintenance& operator=(const NetworkMaintenance&) = delete;
    [[nodiscard]] bool Start();

   private:
    static void Tick(void* context);
    NetworkController& controller_;
    esp_timer_handle_t timer_{};
};
}  // namespace micropixel::host::network
