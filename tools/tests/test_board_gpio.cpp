// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "device/device_registry.hpp"
#include "platform/boards/metalio-claw4/board_config.hpp"
#include "platform/gpio/esp_gpio_peripheral.hpp"

namespace {
namespace board = micropixel::platform::metalio_claw4::board;
using micropixel::platform::gpio::EspGpioPeripheral;

int32_t Open(EspGpioPeripheral& gpio, uint32_t pin, uint16_t mode, uint32_t value = 0U, uint32_t frequency = 0U) {
    return gpio.Open(pin, mode, MICROPIXEL_GPIO_PULL_NONE, MICROPIXEL_GPIO_EDGE_NONE, value, frequency, nullptr,
                     nullptr);
}

void TestWhitelistAndPwm() {
    EspGpioPeripheral gpio(board::kApplicationGpioLines);
    // Claw4's power-key initialization already installed the shared ISR service.
    gpio_isr_installed = true;
    assert(gpio.Initialize() == ESP_OK);
    assert(gpio.Initialize() == ESP_OK && gpio_install_count == 1);
    micropixel_gpio_info_t info{};
    for (uint32_t pin : board::kApplicationGpioLines) {
        assert(gpio.GetInfo(pin, info) == MICROPIXEL_STATUS_OK);
        assert(info.line_number == pin && (info.capabilities & MICROPIXEL_GPIO_CAP_PWM) != 0);
    }
    for (int pin :
         {board::kTouchInterrupt, board::kI2cData, board::kI2cClock, board::kHapticMotor, board::kDisplayBacklight}) {
        assert(gpio.GetInfo(pin, info) == MICROPIXEL_STATUS_NOT_FOUND);
        assert(Open(gpio, pin, MICROPIXEL_GPIO_MODE_OUTPUT) == MICROPIXEL_STATUS_NOT_FOUND);
    }
    assert(Open(gpio, 5, MICROPIXEL_GPIO_MODE_OUTPUT, 1) == MICROPIXEL_STATUS_OK);
    bool level{};
    assert(gpio.Read(5, level) == MICROPIXEL_STATUS_OK && level);
    assert(gpio.Write(5, false) == MICROPIXEL_STATUS_OK);
    assert(gpio.Read(5, level) == MICROPIXEL_STATUS_OK && !level);
    gpio.Close(5);
    assert(gpio_pins[5].mode == GPIO_MODE_DISABLE);
    assert(gpio.Read(5, level) == MICROPIXEL_STATUS_CLOSED);
    assert(Open(gpio, 5, MICROPIXEL_GPIO_MODE_PWM, 500, 1000) == MICROPIXEL_STATUS_OK);
    assert(Open(gpio, 14, MICROPIXEL_GPIO_MODE_PWM, 1000, 2000) == MICROPIXEL_STATUS_OK);
    assert(!gpio_pwm_active[board::kBacklightLedcChannel] && !gpio_pwm_active[board::kHapticLedcChannel]);
    assert(gpio_pwm_channels[2].gpio_num == 5 && gpio_pwm_channels[2].timer_sel == LEDC_TIMER_2);
    assert(gpio_pwm_channels[3].gpio_num == 14 && gpio_pwm_channels[3].timer_sel == LEDC_TIMER_3);
    assert(Open(gpio, 15, MICROPIXEL_GPIO_MODE_PWM, 0, 1000) == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    assert(gpio.SetPwmDuty(5, 1000) == MICROPIXEL_STATUS_OK && gpio_pwm_channels[2].duty == 1023);
    gpio.Close(5);
    assert(!gpio_pwm_active[2]);
    assert(Open(gpio, 15, MICROPIXEL_GPIO_MODE_PWM, 0, 1000) == MICROPIXEL_STATUS_OK);
    assert(gpio_pwm_channels[2].gpio_num == 15);
}

void Edge(void* context, micropixel::device::GpioPeripheral&, micropixel::device::PeripheralChannelId pin, bool level,
          uint64_t timestamp) {
    assert(pin == 5 && level && timestamp != 0);
    static_cast<std::atomic<uint32_t>*>(context)->fetch_add(1);
}

void FireEdgeAndWait(std::atomic<uint32_t>& count, uint32_t expected) {
    gpio_pins[5].level = 1;
    assert(gpio_pins[5].handler != nullptr && gpio_pins[5].interrupt_enabled);
    gpio_pins[5].handler(gpio_pins[5].context);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (count.load() != expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(count.load() == expected);
}

void TestSuspendResumeAndRollback() {
    std::atomic<uint32_t> count{};
    EspGpioPeripheral gpio(board::kApplicationGpioLines);
    assert(gpio.Initialize() == ESP_OK);
    assert(gpio.Open(5, MICROPIXEL_GPIO_MODE_INPUT, MICROPIXEL_GPIO_PULL_UP, MICROPIXEL_GPIO_EDGE_RISING, 0, 0, Edge,
                     &count) == MICROPIXEL_STATUS_OK);
    FireEdgeAndWait(count, 1);
    gpio.SuspendEvents();
    assert(!gpio_pins[5].interrupt_enabled && gpio_pins[5].handler == nullptr);
    gpio_fail_handler_pin = 5;
    assert(gpio.ResumeEvents() == MICROPIXEL_STATUS_INTERNAL);
    assert(gpio_pins[5].handler == nullptr);
    gpio_fail_handler_pin = -1;
    assert(gpio.ResumeEvents() == MICROPIXEL_STATUS_OK);
    FireEdgeAndWait(count, 2);
    gpio.Close(5);
    assert(gpio_pins[5].handler == nullptr && gpio_pins[5].mode == GPIO_MODE_DISABLE);
}

void TestRegistryInputRouting() {
    EspGpioPeripheral gpio(board::kApplicationGpioLines);
    assert(gpio.Initialize() == ESP_OK);
    micropixel::device::DeviceRegistry registry;
    assert(registry.RegisterGpio(gpio, 5, "P5"));
    micropixel_device_info_t info{};
    assert(registry.GetByIndex(0, info) == MICROPIXEL_STATUS_OK);
    assert(info.device != 5);
    assert(registry.Open(info.device, MICROPIXEL_GPIO_MODE_INPUT, MICROPIXEL_GPIO_PULL_NONE, MICROPIXEL_GPIO_EDGE_NONE,
                         0, 0, nullptr, nullptr) == MICROPIXEL_STATUS_OK);
    bool level{};
    assert(registry.Read(info.device, level) == MICROPIXEL_STATUS_OK);
    assert(gpio_pins[5].handler == nullptr);
    registry.Close(info.device);

    struct EdgeState {
        micropixel_device_id_t device;
        std::atomic<uint32_t> count{};
    } state{info.device};
    const auto on_edge = [](void* context, micropixel_device_id_t device, bool value, uint64_t timestamp) {
        auto& state = *static_cast<EdgeState*>(context);
        assert(device == state.device && value && timestamp != 0);
        state.count.fetch_add(1);
    };
    assert(registry.Open(info.device, MICROPIXEL_GPIO_MODE_INPUT, MICROPIXEL_GPIO_PULL_UP, MICROPIXEL_GPIO_EDGE_RISING,
                         0, 0, on_edge, &state) == MICROPIXEL_STATUS_OK);
    FireEdgeAndWait(state.count, 1);
    registry.Close(info.device);
}
}  // namespace

int main() {
    TestWhitelistAndPwm();
    TestSuspendResumeAndRollback();
    TestRegistryInputRouting();
    std::puts("Board GPIO: whitelist, reserved PWM channels, close, suspend/resume, rollback and routing passed.");
}
