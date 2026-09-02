#include "platform/gpio/gpio_isr_service.hpp"

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::gpio {
namespace {

constexpr uint8_t kNotInstalled = 0U;
constexpr uint8_t kInstalling = 1U;
constexpr uint8_t kInstalled = 2U;
std::atomic<uint8_t> isr_service_state{kNotInstalled};

}  // namespace

esp_err_t EnsureIsrServiceInstalled() {
    for (;;) {
        uint8_t state = isr_service_state.load(std::memory_order_acquire);
        if (state == kInstalled) {
            return ESP_OK;
        }
        if (state == kNotInstalled &&
            isr_service_state.compare_exchange_strong(state, kInstalling, std::memory_order_acq_rel)) {
            const esp_err_t status = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
            const bool available = status == ESP_OK || status == ESP_ERR_INVALID_STATE;
            isr_service_state.store(available ? kInstalled : kNotInstalled, std::memory_order_release);
            return available ? ESP_OK : status;
        }
        taskYIELD();
    }
}

}  // namespace micropixel::platform::gpio
