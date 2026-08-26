#include "runtime/services/gpio_service.hpp"

#include <cstring>

#include "runtime/services/timer_service.hpp"

namespace micropixel::runtime {

GpioService::GpioService(device::GpioService& gpio, EventQueue& events, TimerService& clock)
    : gpio_(gpio), events_(events), clock_(clock), mutex_(xSemaphoreCreateMutex()) {}

GpioService::~GpioService() {
    Shutdown();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

bool GpioService::TakeLock() { return mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE; }

void GpioService::GiveLock() { (void)xSemaphoreGive(mutex_); }

GpioService::Slot* GpioService::Find(micropixel_gpio_handle_t handle) {
    const uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > limits::kMaxGpioHandles) {
        return nullptr;
    }
    Slot& slot = slots_[encoded_index - 1U];
    return slot.handle == handle ? &slot : nullptr;
}

ServiceResult<micropixel_gpio_open_response_t> GpioService::Open(const micropixel_gpio_open_request_t& request) {
    if (!TakeLock()) {
        return FailService<micropixel_gpio_open_response_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    for (const Slot& existing : slots_) {
        if (existing.handle != 0U && existing.device == request.device) {
            GiveLock();
            return FailService<micropixel_gpio_open_response_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
    }
    uint32_t selected_index = limits::kMaxGpioHandles;
    for (uint32_t index = 0U; index < limits::kMaxGpioHandles; ++index) {
        if (slots_[index].handle == 0U) {
            selected_index = index;
            break;
        }
    }
    if (selected_index == limits::kMaxGpioHandles) {
        GiveLock();
        return FailService<micropixel_gpio_open_response_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    Slot& slot = slots_[selected_index];
    ++slot.generation;
    if (slot.generation == 0U) {
        ++slot.generation;
    }
    slot.handle = (slot.generation << 8U) | (selected_index + 1U);
    slot.device = request.device;
    slot.mode = request.mode;
    slot.edge = request.edge;
    const micropixel_gpio_handle_t handle = slot.handle;
    GiveLock();

    const device::GpioEdgeSink sink = request.edge == MICROPIXEL_GPIO_EDGE_NONE ? nullptr : OnEdge;
    auto opened = gpio_.Open(request.device, request.mode, request.pull, request.edge, request.initial_value,
                             request.pwm_frequency_hz, sink, this);
    if (!opened) {
        if (TakeLock()) {
            Slot* failed = Find(handle);
            if (failed != nullptr) {
                const uint32_t generation = failed->generation;
                *failed = {};
                failed->generation = generation;
            }
            GiveLock();
        }
        return FailService<micropixel_gpio_open_response_t>(opened.error().status);
    }

    micropixel_gpio_open_response_t response{};
    response.size = sizeof(response);
    response.mode = request.mode;
    response.gpio = handle;
    response.device = request.device;
    return response;
}

ServiceResult<micropixel_gpio_value_response_t> GpioService::Read(micropixel_gpio_handle_t gpio) {
    if (!TakeLock()) {
        return FailService<micropixel_gpio_value_response_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(gpio);
    if (slot == nullptr) {
        GiveLock();
        return FailService<micropixel_gpio_value_response_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    GiveLock();
    auto value = gpio_.Read(device);
    if (!value) {
        return FailService<micropixel_gpio_value_response_t>(value.error().status);
    }
    micropixel_gpio_value_response_t response{};
    response.size = sizeof(response);
    response.gpio = gpio;
    response.value = *value ? 1U : 0U;
    return response;
}

ServiceResult<void> GpioService::Write(micropixel_gpio_handle_t gpio, bool value) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(gpio);
    if (slot == nullptr || slot->mode != MICROPIXEL_GPIO_MODE_OUTPUT) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    GiveLock();
    auto result = gpio_.Write(device, value);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> GpioService::SetPwmDuty(micropixel_gpio_handle_t gpio, uint16_t duty_per_mille) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(gpio);
    if (slot == nullptr || slot->mode != MICROPIXEL_GPIO_MODE_PWM) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    GiveLock();
    auto result = gpio_.SetPwmDuty(device, duty_per_mille);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<void> GpioService::Release(micropixel_gpio_handle_t gpio) {
    if (!TakeLock()) {
        return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    }
    Slot* slot = Find(gpio);
    if (slot == nullptr) {
        GiveLock();
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_device_id_t device = slot->device;
    const uint32_t generation = slot->generation;
    *slot = {};
    slot->generation = generation;
    GiveLock();
    gpio_.Close(device);
    return {};
}

void GpioService::Suspend() {
    if (suspended_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    gpio_.SuspendEvents();
}

bool GpioService::Resume() {
    if (!suspended_.load(std::memory_order_acquire)) {
        return true;
    }
    auto resumed = gpio_.ResumeEvents();
    if (!resumed) {
        return false;
    }
    suspended_.store(false, std::memory_order_release);
    return true;
}

void GpioService::Shutdown() {
    if (shut_down_) {
        return;
    }
    shut_down_ = true;
    if (!TakeLock()) {
        return;
    }
    micropixel_device_id_t devices[limits::kMaxGpioHandles]{};
    uint32_t device_count = 0U;
    for (Slot& slot : slots_) {
        if (slot.handle != 0U) {
            devices[device_count++] = slot.device;
            const uint32_t generation = slot.generation;
            slot = {};
            slot.generation = generation;
        }
    }
    GiveLock();
    for (uint32_t index = 0U; index < device_count; ++index) {
        gpio_.Close(devices[index]);
    }
    // The Platform backend outlives this AppSession. RequestStop() suspends
    // edge delivery before Shutdown(), so restore the no-owner baseline after
    // all handles are closed; otherwise the next Guest cannot subscribe.
    (void)gpio_.ResumeEvents();
    suspended_.store(false, std::memory_order_release);
}

void GpioService::OnEdge(void* context, micropixel_device_id_t device, bool value, uint64_t timestamp_us) {
    (void)timestamp_us;
    if (context != nullptr) {
        static_cast<GpioService*>(context)->HandleEdge(device, value);
    }
}

void GpioService::HandleEdge(micropixel_device_id_t device, bool value) {
    if (suspended_.load(std::memory_order_acquire) || !TakeLock()) {
        return;
    }
    micropixel_gpio_handle_t handle = 0U;
    uint32_t sequence = 0U;
    uint16_t configured_edge = MICROPIXEL_GPIO_EDGE_NONE;
    for (Slot& slot : slots_) {
        if (slot.handle != 0U && slot.device == device && slot.mode == MICROPIXEL_GPIO_MODE_INPUT) {
            handle = slot.handle;
            sequence = ++slot.sequence;
            configured_edge = slot.edge;
            break;
        }
    }
    GiveLock();
    if (handle == 0U) {
        return;
    }
    const uint32_t observed_edge = value ? MICROPIXEL_GPIO_EDGE_RISING : MICROPIXEL_GPIO_EDGE_FALLING;
    if (configured_edge != MICROPIXEL_GPIO_EDGE_BOTH && configured_edge != observed_edge) {
        return;
    }
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_GPIO_EVENT_EDGE;
    event.service_id = MICROPIXEL_SERVICE_GPIO;
    event.source = handle;
    event.timestamp_us = clock_.Now();
    event.sequence = sequence;
    event.status = MICROPIXEL_STATUS_OK;
    micropixel_gpio_event_payload_t payload{};
    payload.device = device;
    payload.value = value ? 1U : 0U;
    payload.edge = observed_edge;
    std::memcpy(event.payload, &payload, sizeof(payload));
    (void)events_.PushGpioCoalesced(event);
}

}  // namespace micropixel::runtime
