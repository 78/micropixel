#include "platform/gpio/esp_gpio_peripheral.hpp"

#include <cstdint>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "platform/gpio/gpio_isr_service.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform::gpio {
namespace {

constexpr char kTag[] = "micropixel_gpio";
constexpr uint32_t kMaximumPwmFrequencyHz = 40000U;
constexpr ledc_mode_t kPwmSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_bit_t kPwmResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kPwmMaximumDuty = (1U << 10U) - 1U;
constexpr uint32_t kEdgeQueueCapacity = 32U;
constexpr uint32_t kWorkerStackBytes = 4096U;
constexpr BaseType_t kWorkerCore = 0;

gpio_pullup_t PullUp(uint16_t pull) {
    return pull == MICROPIXEL_GPIO_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
}

gpio_pulldown_t PullDown(uint16_t pull) {
    return pull == MICROPIXEL_GPIO_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
}

gpio_int_type_t InterruptType(uint16_t edge) {
    switch (edge) {
        case MICROPIXEL_GPIO_EDGE_RISING:
            return GPIO_INTR_POSEDGE;
        case MICROPIXEL_GPIO_EDGE_FALLING:
            return GPIO_INTR_NEGEDGE;
        case MICROPIXEL_GPIO_EDGE_BOTH:
            return GPIO_INTR_ANYEDGE;
        default:
            return GPIO_INTR_DISABLE;
    }
}

}  // namespace

EspGpioPeripheral::EspGpioPeripheral(std::span<const device::PeripheralChannelId> application_lines)
    : line_count_(application_lines.size() <= lines_.size() ? static_cast<uint32_t>(application_lines.size()) : 0U),
      valid_configuration_(line_count_ != 0U) {
    for (uint32_t index = 0U; index < line_count_; ++index) {
        const auto channel = application_lines[index];
        if (channel >= GPIO_NUM_MAX) {
            valid_configuration_ = false;
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (application_lines[previous] == channel) {
                valid_configuration_ = false;
            }
        }
        lines_[index].owner = this;
        lines_[index].channel = channel;
        lines_[index].line_number = channel;
        lines_[index].pin = static_cast<gpio_num_t>(channel);
    }
    pwm_slots_[0].timer = LEDC_TIMER_2;
    pwm_slots_[0].channel = LEDC_CHANNEL_2;
    pwm_slots_[1].timer = LEDC_TIMER_3;
    pwm_slots_[1].channel = LEDC_CHANNEL_3;
}

EspGpioPeripheral::~EspGpioPeripheral() {
    for (uint32_t index = 0U; index < line_count_; ++index) {
        Line& line = lines_[index];
        if (line.active) {
            Close(line.channel);
        }
    }
    StopEdgeWorker();
}

esp_err_t EspGpioPeripheral::Initialize() {
    if (!valid_configuration_) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t isr_status = EnsureIsrServiceInstalled();
    if (isr_status != ESP_OK) {
        return isr_status;
    }
    ESP_LOGI(kTag, "%u application GPIO lines ready; edge resources are lazy", static_cast<unsigned>(line_count_));
    return ESP_OK;
}

int32_t EspGpioPeripheral::GetInfo(device::PeripheralChannelId channel, micropixel_gpio_info_t& info_out) const {
    const Line* line = Find(channel);
    if (line == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.line_number = line->line_number;
    info_out.capabilities = MICROPIXEL_GPIO_CAP_INPUT | MICROPIXEL_GPIO_CAP_OUTPUT | MICROPIXEL_GPIO_CAP_PULL_UP |
                            MICROPIXEL_GPIO_CAP_PULL_DOWN | MICROPIXEL_GPIO_CAP_EDGE_EVENTS | MICROPIXEL_GPIO_CAP_PWM;
    info_out.maximum_pwm_frequency_hz = kMaximumPwmFrequencyHz;
    return MICROPIXEL_STATUS_OK;
}

int32_t EspGpioPeripheral::Open(device::PeripheralChannelId channel, uint16_t mode, uint16_t pull, uint16_t edge,
                                uint32_t initial_value, uint32_t pwm_frequency_hz,
                                device::GpioPeripheralEdgeSink edge_sink, void* edge_context) {
    Line* line = Find(channel);
    if (line == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (line->active || initial_value > 1000U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    const bool needs_edge_worker = mode == MICROPIXEL_GPIO_MODE_INPUT && edge != MICROPIXEL_GPIO_EDGE_NONE;
    if (needs_edge_worker && events_suspended_) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    const bool started_worker = needs_edge_worker && edge_line_count_ == 0U;
    if (started_worker && !StartEdgeWorker()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }

    int32_t status = MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (mode == MICROPIXEL_GPIO_MODE_INPUT) {
        if (initial_value != 0U || pwm_frequency_hz != 0U) {
            if (started_worker) {
                StopEdgeWorker();
            }
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        status = ConfigureInput(*line, pull, edge, edge_sink, edge_context);
    } else if (mode == MICROPIXEL_GPIO_MODE_OUTPUT) {
        if (pull != MICROPIXEL_GPIO_PULL_NONE || edge != MICROPIXEL_GPIO_EDGE_NONE || pwm_frequency_hz != 0U ||
            initial_value > 1U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        status = ConfigureOutput(*line, initial_value != 0U);
    } else if (mode == MICROPIXEL_GPIO_MODE_PWM) {
        if (pull != MICROPIXEL_GPIO_PULL_NONE || edge != MICROPIXEL_GPIO_EDGE_NONE) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        status = ConfigurePwm(*line, pwm_frequency_hz, static_cast<uint16_t>(initial_value));
    }
    if (status == MICROPIXEL_STATUS_OK && needs_edge_worker) {
        ++edge_line_count_;
    } else if (status != MICROPIXEL_STATUS_OK && started_worker) {
        StopEdgeWorker();
    }
    return status;
}

int32_t EspGpioPeripheral::ConfigureInput(Line& line, uint16_t pull, uint16_t edge,
                                          device::GpioPeripheralEdgeSink edge_sink, void* edge_context) {
    if (pull > MICROPIXEL_GPIO_PULL_DOWN || edge > MICROPIXEL_GPIO_EDGE_BOTH ||
        ((edge == MICROPIXEL_GPIO_EDGE_NONE) != (edge_sink == nullptr))) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<uint32_t>(line.pin);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = PullUp(pull);
    config.pull_down_en = PullDown(pull);
    config.intr_type = InterruptType(edge);
    if (gpio_config(&config) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }

    portENTER_CRITICAL(&state_lock_);
    line.mode = MICROPIXEL_GPIO_MODE_INPUT;
    line.edge = edge;
    line.edge_sink = edge_sink;
    line.edge_context = edge_context;
    line.active = true;
    portEXIT_CRITICAL(&state_lock_);
    if (edge != MICROPIXEL_GPIO_EDGE_NONE) {
        esp_err_t status = gpio_isr_handler_add(line.pin, OnEdge, &line);
        if (status == ESP_OK) {
            status = gpio_intr_enable(line.pin);
        }
        if (status != ESP_OK) {
            (void)gpio_isr_handler_remove(line.pin);
            portENTER_CRITICAL(&state_lock_);
            line = Line{.owner = this, .channel = line.channel, .pin = line.pin, .line_number = line.line_number};
            portEXIT_CRITICAL(&state_lock_);
            (void)gpio_reset_pin(line.pin);
            return MICROPIXEL_STATUS_INTERNAL;
        }
        line.isr_registered = true;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t EspGpioPeripheral::ConfigureOutput(Line& line, bool initial_value) {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<uint32_t>(line.pin);
    config.mode = GPIO_MODE_OUTPUT;
    config.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&config) != ESP_OK || gpio_set_level(line.pin, initial_value ? 1U : 0U) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    line.mode = MICROPIXEL_GPIO_MODE_OUTPUT;
    line.active = true;
    return MICROPIXEL_STATUS_OK;
}

int32_t EspGpioPeripheral::ConfigurePwm(Line& line, uint32_t frequency_hz, uint16_t initial_duty_per_mille) {
    if (frequency_hz == 0U || frequency_hz > kMaximumPwmFrequencyHz || initial_duty_per_mille > 1000U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    int32_t slot_index = -1;
    for (uint32_t index = 0U; index < pwm_slots_.size(); ++index) {
        if (pwm_slots_[index].line == nullptr) {
            slot_index = static_cast<int32_t>(index);
            break;
        }
    }
    if (slot_index < 0) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    PwmSlot& slot = pwm_slots_[static_cast<uint32_t>(slot_index)];
    ledc_timer_config_t timer{};
    timer.speed_mode = kPwmSpeedMode;
    timer.duty_resolution = kPwmResolution;
    timer.timer_num = slot.timer;
    timer.freq_hz = frequency_hz;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    ledc_channel_config_t channel{};
    channel.gpio_num = line.pin;
    channel.speed_mode = kPwmSpeedMode;
    channel.channel = slot.channel;
    channel.timer_sel = slot.timer;
    channel.duty = kPwmMaximumDuty * initial_duty_per_mille / 1000U;
    if (ledc_channel_config(&channel) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    slot.line = &line;
    line.mode = MICROPIXEL_GPIO_MODE_PWM;
    line.pwm_slot = static_cast<int8_t>(slot_index);
    line.active = true;
    return MICROPIXEL_STATUS_OK;
}

int32_t EspGpioPeripheral::Read(device::PeripheralChannelId channel, bool& value_out) const {
    const Line* line = Find(channel);
    if (line == nullptr || !line->active) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    value_out = gpio_get_level(line->pin) != 0;
    return MICROPIXEL_STATUS_OK;
}

int32_t EspGpioPeripheral::Write(device::PeripheralChannelId channel, bool value) {
    Line* line = Find(channel);
    if (line == nullptr || !line->active || line->mode != MICROPIXEL_GPIO_MODE_OUTPUT) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    return gpio_set_level(line->pin, value ? 1U : 0U) == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

int32_t EspGpioPeripheral::SetPwmDuty(device::PeripheralChannelId channel, uint16_t duty_per_mille) {
    Line* line = Find(channel);
    if (line == nullptr || !line->active || line->mode != MICROPIXEL_GPIO_MODE_PWM || line->pwm_slot < 0) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    if (duty_per_mille > 1000U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const PwmSlot& slot = pwm_slots_[static_cast<uint32_t>(line->pwm_slot)];
    const uint32_t duty = kPwmMaximumDuty * duty_per_mille / 1000U;
    const esp_err_t status = ledc_set_duty(kPwmSpeedMode, slot.channel, duty);
    return status == ESP_OK && ledc_update_duty(kPwmSpeedMode, slot.channel) == ESP_OK ? MICROPIXEL_STATUS_OK
                                                                                       : MICROPIXEL_STATUS_INTERNAL;
}

void EspGpioPeripheral::Close(device::PeripheralChannelId channel) {
    Line* line = Find(channel);
    if (line == nullptr || !line->active) {
        return;
    }
    const bool had_edge = line->edge != MICROPIXEL_GPIO_EDGE_NONE;
    portENTER_CRITICAL(&state_lock_);
    line->active = false;
    line->edge_sink = nullptr;
    line->edge_context = nullptr;
    portEXIT_CRITICAL(&state_lock_);
    if (line->isr_registered) {
        (void)gpio_intr_disable(line->pin);
        (void)gpio_isr_handler_remove(line->pin);
        line->isr_registered = false;
    }
    if (callback_mutex_ != nullptr && xSemaphoreTake(callback_mutex_, portMAX_DELAY) == pdTRUE) {
        (void)xSemaphoreGive(callback_mutex_);
    }
    if (line->pwm_slot >= 0) {
        PwmSlot& slot = pwm_slots_[static_cast<uint32_t>(line->pwm_slot)];
        (void)ledc_stop(kPwmSpeedMode, slot.channel, 0U);
        slot.line = nullptr;
    }
    const device::PeripheralChannelId line_endpoint = line->channel;
    const gpio_num_t pin = line->pin;
    const uint16_t number = line->line_number;
    *line = Line{.owner = this, .channel = line_endpoint, .pin = pin, .line_number = number};
    (void)gpio_reset_pin(pin);
    if (had_edge && edge_line_count_ != 0U) {
        --edge_line_count_;
        if (edge_line_count_ == 0U) {
            StopEdgeWorker();
        }
    }
}

void EspGpioPeripheral::SuspendEvents() {
    if (events_suspended_) {
        return;
    }
    events_suspended_ = true;
    for (uint32_t index = 0U; index < line_count_; ++index) {
        Line& line = lines_[index];
        if (line.active && line.edge != MICROPIXEL_GPIO_EDGE_NONE && line.isr_registered) {
            (void)gpio_intr_disable(line.pin);
            (void)gpio_isr_handler_remove(line.pin);
            line.isr_registered = false;
        }
    }
    StopEdgeWorker();
}

int32_t EspGpioPeripheral::ResumeEvents() {
    if (!events_suspended_) {
        return MICROPIXEL_STATUS_OK;
    }
    if (edge_line_count_ == 0U) {
        events_suspended_ = false;
        return MICROPIXEL_STATUS_OK;
    }
    if (!StartEdgeWorker()) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    for (uint32_t index = 0U; index < line_count_; ++index) {
        Line& line = lines_[index];
        if (!line.active || line.edge == MICROPIXEL_GPIO_EDGE_NONE) {
            continue;
        }
        esp_err_t status = gpio_isr_handler_add(line.pin, OnEdge, &line);
        if (status == ESP_OK) {
            status = gpio_intr_enable(line.pin);
        }
        if (status != ESP_OK) {
            (void)gpio_isr_handler_remove(line.pin);
            for (uint32_t rollback_index = 0U; rollback_index < line_count_; ++rollback_index) {
                Line& rollback = lines_[rollback_index];
                if (rollback.isr_registered) {
                    (void)gpio_intr_disable(rollback.pin);
                    (void)gpio_isr_handler_remove(rollback.pin);
                    rollback.isr_registered = false;
                }
            }
            StopEdgeWorker();
            return MICROPIXEL_STATUS_INTERNAL;
        }
        line.isr_registered = true;
    }
    events_suspended_ = false;
    return MICROPIXEL_STATUS_OK;
}

EspGpioPeripheral::Line* EspGpioPeripheral::Find(device::PeripheralChannelId channel) {
    for (uint32_t index = 0U; index < line_count_; ++index) {
        Line& line = lines_[index];
        if (line.channel == channel) {
            return &line;
        }
    }
    return nullptr;
}

const EspGpioPeripheral::Line* EspGpioPeripheral::Find(device::PeripheralChannelId channel) const {
    for (uint32_t index = 0U; index < line_count_; ++index) {
        const Line& line = lines_[index];
        if (line.channel == channel) {
            return &line;
        }
    }
    return nullptr;
}

bool EspGpioPeripheral::StartEdgeWorker() {
    if (worker_ != nullptr) {
        return true;
    }
    ReleaseEdgeResources();
    edge_queue_ = xQueueCreate(kEdgeQueueCapacity, sizeof(EdgeRecord));
    worker_stopped_ = xSemaphoreCreateBinary();
    callback_mutex_ = xSemaphoreCreateMutex();
    if (edge_queue_ == nullptr || worker_stopped_ == nullptr || callback_mutex_ == nullptr) {
        ReleaseEdgeResources();
        return false;
    }
    stopping_.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(WorkerEntry, "micropixel_gpio", kWorkerStackBytes, this,
                                task_policy::kRemoteControlPriority, &worker_, kWorkerCore) != pdPASS) {
        worker_ = nullptr;
        ReleaseEdgeResources();
        return false;
    }
    return true;
}

void EspGpioPeripheral::StopEdgeWorker() {
    if (worker_ != nullptr) {
        stopping_.store(true, std::memory_order_release);
        const EdgeRecord stop{UINT32_MAX, 0U, 0U};
        (void)xQueueSend(edge_queue_, &stop, 0U);
        (void)xSemaphoreTake(worker_stopped_, portMAX_DELAY);
        worker_ = nullptr;
    }
    ReleaseEdgeResources();
}

void EspGpioPeripheral::ReleaseEdgeResources() {
    if (edge_queue_ != nullptr) {
        vQueueDelete(edge_queue_);
        edge_queue_ = nullptr;
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
        worker_stopped_ = nullptr;
    }
    if (callback_mutex_ != nullptr) {
        vSemaphoreDelete(callback_mutex_);
        callback_mutex_ = nullptr;
    }
}

void IRAM_ATTR EspGpioPeripheral::OnEdge(void* context) {
    auto& line = *static_cast<Line*>(context);
    EspGpioPeripheral& peripheral = *line.owner;
    const uint32_t index = static_cast<uint32_t>(&line - peripheral.lines_.data());
    const EdgeRecord record{index, static_cast<uint32_t>(gpio_get_level(line.pin)),
                            static_cast<uint64_t>(esp_timer_get_time())};
    BaseType_t higher_priority_woken = pdFALSE;
    if (peripheral.edge_queue_ != nullptr) {
        (void)xQueueSendFromISR(peripheral.edge_queue_, &record, &higher_priority_woken);
    }
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void EspGpioPeripheral::WorkerEntry(void* context) {
    static_cast<EspGpioPeripheral*>(context)->Worker();
    vTaskDelete(nullptr);
}

void EspGpioPeripheral::Worker() {
    while (!stopping_.load(std::memory_order_acquire)) {
        EdgeRecord record{};
        if (xQueueReceive(edge_queue_, &record, portMAX_DELAY) != pdTRUE || record.line_index >= line_count_) {
            continue;
        }
        if (callback_mutex_ == nullptr || xSemaphoreTake(callback_mutex_, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        device::GpioPeripheralEdgeSink sink = nullptr;
        void* sink_context = nullptr;
        device::PeripheralChannelId channel = 0U;
        portENTER_CRITICAL(&state_lock_);
        const Line& line = lines_[record.line_index];
        if (line.active && line.mode == MICROPIXEL_GPIO_MODE_INPUT) {
            sink = line.edge_sink;
            sink_context = line.edge_context;
            channel = line.channel;
        }
        portEXIT_CRITICAL(&state_lock_);
        if (sink != nullptr) {
            sink(sink_context, *this, channel, record.value != 0U, record.timestamp_us);
        }
        (void)xSemaphoreGive(callback_mutex_);
    }
    if (worker_stopped_ != nullptr) {
        (void)xSemaphoreGive(worker_stopped_);
    }
}

}  // namespace micropixel::platform::gpio
