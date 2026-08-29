#ifndef MICROPIXEL_PLATFORM_GPIO_ESP_GPIO_PERIPHERAL_HPP
#define MICROPIXEL_PLATFORM_GPIO_ESP_GPIO_PERIPHERAL_HPP

#include <array>
#include <atomic>
#include <span>

#include "device/contracts/gpio.hpp"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace micropixel::platform::gpio {

class EspGpioPeripheral final : public device::GpioPeripheral {
   public:
    static constexpr uint32_t kMaximumLineCount = 24U;

    explicit EspGpioPeripheral(std::span<const device::PeripheralChannelId> application_lines);
    ~EspGpioPeripheral() override;

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel, micropixel_gpio_info_t& info_out) const override;
    [[nodiscard]] int32_t Open(device::PeripheralChannelId channel, uint16_t mode, uint16_t pull, uint16_t edge,
                               uint32_t initial_value, uint32_t pwm_frequency_hz,
                               device::GpioPeripheralEdgeSink edge_sink, void* edge_context) override;
    [[nodiscard]] int32_t Read(device::PeripheralChannelId channel, bool& value_out) const override;
    [[nodiscard]] int32_t Write(device::PeripheralChannelId channel, bool value) override;
    [[nodiscard]] int32_t SetPwmDuty(device::PeripheralChannelId channel, uint16_t duty_per_mille) override;
    void SuspendEvents() override;
    [[nodiscard]] int32_t ResumeEvents() override;
    void Close(device::PeripheralChannelId channel) override;

   private:
    struct Line final {
        EspGpioPeripheral* owner{};
        device::PeripheralChannelId channel{};
        gpio_num_t pin{GPIO_NUM_NC};
        uint16_t line_number{};
        uint16_t mode{};
        uint16_t edge{};
        int8_t pwm_slot{-1};
        device::GpioPeripheralEdgeSink edge_sink{};
        void* edge_context{};
        bool active{};
        bool isr_registered{};
    };

    struct PwmSlot final {
        ledc_timer_t timer{};
        ledc_channel_t channel{};
        Line* line{};
    };

    struct EdgeRecord final {
        uint32_t line_index{};
        uint32_t value{};
        uint64_t timestamp_us{};
    };

    [[nodiscard]] Line* Find(device::PeripheralChannelId channel);
    [[nodiscard]] const Line* Find(device::PeripheralChannelId channel) const;
    [[nodiscard]] int32_t ConfigureInput(Line& line, uint16_t pull, uint16_t edge,
                                         device::GpioPeripheralEdgeSink edge_sink, void* edge_context);
    [[nodiscard]] int32_t ConfigureOutput(Line& line, bool initial_value);
    [[nodiscard]] int32_t ConfigurePwm(Line& line, uint32_t frequency_hz, uint16_t initial_duty_per_mille);
    [[nodiscard]] bool StartEdgeWorker();
    void StopEdgeWorker();
    void ReleaseEdgeResources();
    static void IRAM_ATTR OnEdge(void* context);
    static void WorkerEntry(void* context);
    void Worker();

    static constexpr uint32_t kPwmSlotCount = 2U;

    std::array<Line, kMaximumLineCount> lines_{};
    std::array<PwmSlot, kPwmSlotCount> pwm_slots_{};
    uint32_t line_count_{};
    bool valid_configuration_{};
    QueueHandle_t edge_queue_{};
    SemaphoreHandle_t worker_stopped_{};
    SemaphoreHandle_t callback_mutex_{};
    TaskHandle_t worker_{};
    std::atomic<bool> stopping_{};
    uint32_t edge_line_count_{};
    bool events_suspended_{};
    portMUX_TYPE state_lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace micropixel::platform::gpio

#endif
