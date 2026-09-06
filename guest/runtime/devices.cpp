#include "sdk/devices.hpp"

#include "runtime/service_binding.hpp"
#include "sdk/gpio.hpp"
#include "sdk/haptics.hpp"
#include "sdk/power_info.hpp"
#include "sdk/sensors.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::OpenService;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache devices_service;
ServiceCache sensors_service;
ServiceCache gpio_service;
ServiceCache haptics_service;
ServiceCache power_info_service;
struct SensorHandleState final {
    uint32_t handle{};
    micropixel::DeviceId device{};
    micropixel::SensorKind kind{micropixel::SensorKind::kAcceleration};
};

SensorHandleState sensor_handles[MICROPIXEL_MAX_SENSOR_HANDLES]{};
SensorHandleState* FindSensorHandle(uint32_t handle) {
    const uint32_t encoded_index = handle & 0xffU;
    if (encoded_index == 0U || encoded_index > MICROPIXEL_MAX_SENSOR_HANDLES) {
        return nullptr;
    }
    SensorHandleState& state = sensor_handles[encoded_index - 1U];
    return state.handle == handle ? &state : nullptr;
}

}  // namespace

namespace micropixel {

Result<DeviceList> Devices::List(DeviceKind kind) const {
    int32_t status = OpenService(devices_service, MICROPIXEL_SERVICE_DEVICES, MICROPIXEL_DEVICES_INTERFACE_MAJOR,
                                 MICROPIXEL_DEVICES_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_devices_list_request_t request{};
    request.size = sizeof(request);
    request.kind = static_cast<uint16_t>(kind);
    micropixel_devices_list_response_t response{};
    uint32_t response_size = 0U;
    status = CallService(devices_service, MICROPIXEL_DEVICES_METHOD_LIST, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.count > MICROPIXEL_MAX_DEVICES) {
        runtime::Panic("devices.list.response", MICROPIXEL_STATUS_INTERNAL);
    }
    DeviceList list{};
    list.count_ = response.count;
    list.generation_ = response.generation;
    for (uint32_t index = 0U; index < response.count; ++index) {
        if (response.devices[index] == 0U) {
            runtime::Panic("devices.list.id", MICROPIXEL_STATUS_INTERNAL);
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (response.devices[previous] == response.devices[index]) {
                runtime::Panic("devices.list.duplicate", MICROPIXEL_STATUS_INTERNAL);
            }
        }
        list.devices_[index] = DeviceId{response.devices[index]};
    }
    return list;
}

Result<DeviceInfo> Devices::GetInfo(DeviceId device) const {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(devices_service, MICROPIXEL_SERVICE_DEVICES, MICROPIXEL_DEVICES_INTERFACE_MAJOR,
                                 MICROPIXEL_DEVICES_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_device_request_t request{sizeof(request), 0U, device.value()};
    micropixel_device_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(devices_service, MICROPIXEL_DEVICES_METHOD_GET_INFO, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.device != device.value() ||
        response.kind == MICROPIXEL_DEVICE_KIND_ANY || response.kind > MICROPIXEL_DEVICE_KIND_NETWORK ||
        response.reserved0 != 0U || response.reserved1 != 0U ||
        response.name_length > MICROPIXEL_DEVICE_NAME_MAX_BYTES || response.name[response.name_length] != '\0') {
        runtime::Panic("devices.info.response", MICROPIXEL_STATUS_INTERNAL);
    }
    DeviceInfo info{};
    info.id = device;
    info.parent = DeviceId{response.parent};
    info.kind = static_cast<DeviceKind>(response.kind);
    info.capabilities = response.capabilities;
    if (!info.name.Append(response.name)) {
        runtime::Panic("devices.info.name", MICROPIXEL_STATUS_INTERNAL);
    }
    return info;
}

Result<SensorInfo> Sensors::GetInfo(DeviceId device) const {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(sensors_service, MICROPIXEL_SERVICE_SENSORS, MICROPIXEL_SENSORS_INTERFACE_MAJOR,
                                 MICROPIXEL_SENSORS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_device_request_t request{sizeof(request), 0U, device.value()};
    micropixel_sensor_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(sensors_service, MICROPIXEL_SENSORS_METHOD_GET_INFO, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.device != device.value() ||
        response.kind < MICROPIXEL_SENSOR_ACCELERATION || response.kind > MICROPIXEL_SENSOR_ORIENTATION ||
        response.placement > MICROPIXEL_SENSOR_PLACEMENT_RIGHT || response.value_count == 0U ||
        response.value_count > 4U || response.minimum_interval_us == 0U ||
        response.maximum_interval_us < response.minimum_interval_us || response.reserved[0] != 0U ||
        response.reserved[1] != 0U) {
        runtime::Panic("sensors.info.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return SensorInfo{device,
                      DeviceId{response.parent},
                      static_cast<SensorKind>(response.kind),
                      static_cast<SensorPlacement>(response.placement),
                      response.value_count,
                      Duration::Microseconds(response.minimum_interval_us),
                      Duration::Microseconds(response.maximum_interval_us)};
}

namespace detail {

Result<SensorOpenResult> OpenSensor(DeviceId device, SensorKind expected_kind) {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(sensors_service, MICROPIXEL_SERVICE_SENSORS, MICROPIXEL_SENSORS_INTERFACE_MAJOR,
                                 MICROPIXEL_SENSORS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_sensor_open_request_t request{};
    request.size = sizeof(request);
    request.expected_kind = static_cast<uint16_t>(expected_kind);
    request.device = device.value();
    micropixel_sensor_open_response_t response{};
    uint32_t response_size = 0U;
    status = CallService(sensors_service, MICROPIXEL_SENSORS_METHOD_OPEN, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.sensor == 0U ||
        response.device != device.value() || response.kind != static_cast<uint16_t>(expected_kind) ||
        response.reserved0 != 0U) {
        runtime::Panic("sensors.open.response", MICROPIXEL_STATUS_INTERNAL);
    }
    const uint32_t encoded_index = response.sensor & 0xffU;
    if (encoded_index == 0U || encoded_index > MICROPIXEL_MAX_SENSOR_HANDLES) {
        runtime::Panic("sensors.open.handle", MICROPIXEL_STATUS_INTERNAL);
    }
    sensor_handles[encoded_index - 1U] = SensorHandleState{response.sensor, device, expected_kind};
    return SensorOpenResult{response.sensor, device, expected_kind};
}

Result<SensorReadResult> ReadSensor(uint32_t handle, SensorKind expected_kind) {
    SensorHandleState* state = FindSensorHandle(handle);
    if (state == nullptr || state->kind != expected_kind) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_handle_request_t request{sizeof(request), 0U, handle};
    micropixel_sensor_reading_t response{};
    uint32_t response_size = 0U;
    const int32_t status = CallService(sensors_service, MICROPIXEL_SENSORS_METHOD_READ, &request, sizeof(request),
                                       &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.sensor != handle ||
        response.device != state->device.value() || response.kind != static_cast<uint16_t>(expected_kind) ||
        response.reserved0 != 0U) {
        runtime::Panic("sensors.read.response", MICROPIXEL_STATUS_INTERNAL);
    }
    SensorReadResult result{};
    result.timestamp_us = response.timestamp_us;
    CopyBytes(result.values, response.values, sizeof(result.values));
    return result;
}

Result<Duration> SetSensorSampleInterval(uint32_t handle, Duration interval) {
    if (FindSensorHandle(handle) == nullptr || interval.count_microseconds() == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_sensor_sample_interval_request_t request{};
    request.size = sizeof(request);
    request.sensor = handle;
    request.interval_us = interval.count_microseconds();
    const int32_t status =
        CallVoid(sensors_service, MICROPIXEL_SENSORS_METHOD_SET_SAMPLE_INTERVAL, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<Duration>{interval} : unexpected(ErrorFromStatus(status));
}

void ReleaseSensor(uint32_t handle) {
    SensorHandleState* state = FindSensorHandle(handle);
    if (state == nullptr) {
        return;
    }
    micropixel_handle_request_t request{sizeof(request), 0U, handle};
    if (OpenService(sensors_service, MICROPIXEL_SERVICE_SENSORS, MICROPIXEL_SENSORS_INTERFACE_MAJOR,
                    MICROPIXEL_SENSORS_INTERFACE_MINOR) == MICROPIXEL_STATUS_OK) {
        (void)CallVoid(sensors_service, MICROPIXEL_SENSORS_METHOD_RELEASE, &request, sizeof(request));
    }
    *state = {};
}

Result<GpioOpenResult> OpenGpio(DeviceId device, uint16_t mode, uint16_t pull, uint16_t edge, uint32_t initial_value,
                                uint32_t pwm_frequency_hz) {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(gpio_service, MICROPIXEL_SERVICE_GPIO, MICROPIXEL_GPIO_INTERFACE_MAJOR,
                                 MICROPIXEL_GPIO_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_gpio_open_request_t request{};
    request.size = sizeof(request);
    request.mode = mode;
    request.device = device.value();
    request.pull = pull;
    request.edge = edge;
    request.initial_value = initial_value;
    request.pwm_frequency_hz = pwm_frequency_hz;
    micropixel_gpio_open_response_t response{};
    uint32_t response_size = 0U;
    status = CallService(gpio_service, MICROPIXEL_GPIO_METHOD_OPEN, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.gpio == 0U ||
        response.device != device.value() || response.mode != mode || response.reserved0 != 0U) {
        runtime::Panic("gpio.open.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return GpioOpenResult{response.gpio, device};
}

Result<bool> ReadGpio(uint32_t handle) {
    if (handle == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_handle_request_t request{sizeof(request), 0U, handle};
    micropixel_gpio_value_response_t response{};
    uint32_t response_size = 0U;
    const int32_t status = CallService(gpio_service, MICROPIXEL_GPIO_METHOD_READ, &request, sizeof(request), &response,
                                       sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.gpio != handle ||
        response.reserved0 != 0U || response.value > 1U) {
        runtime::Panic("gpio.read.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return response.value != 0U;
}

Result<void> WriteGpio(uint32_t handle, bool value) {
    micropixel_gpio_value_request_t request{sizeof(request), 0U, handle, value ? 1U : 0U};
    const int32_t status = CallVoid(gpio_service, MICROPIXEL_GPIO_METHOD_WRITE, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> SetGpioPwmDuty(uint32_t handle, uint16_t duty_per_mille) {
    micropixel_gpio_value_request_t request{sizeof(request), 0U, handle, duty_per_mille};
    const int32_t status = CallVoid(gpio_service, MICROPIXEL_GPIO_METHOD_SET_PWM_DUTY, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

void ReleaseGpio(uint32_t handle) {
    if (handle == 0U) {
        return;
    }
    micropixel_handle_request_t request{sizeof(request), 0U, handle};
    if (OpenService(gpio_service, MICROPIXEL_SERVICE_GPIO, MICROPIXEL_GPIO_INTERFACE_MAJOR,
                    MICROPIXEL_GPIO_INTERFACE_MINOR) == MICROPIXEL_STATUS_OK) {
        (void)CallVoid(gpio_service, MICROPIXEL_GPIO_METHOD_RELEASE, &request, sizeof(request));
    }
}

Result<uint32_t> OpenHaptic(DeviceId device) {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(haptics_service, MICROPIXEL_SERVICE_HAPTICS, MICROPIXEL_HAPTICS_INTERFACE_MAJOR,
                                 MICROPIXEL_HAPTICS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_device_request_t request{sizeof(request), 0U, device.value()};
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    status = CallService(haptics_service, MICROPIXEL_HAPTICS_METHOD_OPEN, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.reserved0 != 0U ||
        response.handle == 0U) {
        runtime::Panic("haptics.open.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return response.handle;
}

Result<void> PlayHaptic(uint32_t handle, Duration duration, uint16_t strength_per_mille) {
    const uint64_t duration_us = duration.count_microseconds();
    if (handle == 0U || strength_per_mille == 0U || strength_per_mille > 1000U || duration_us == 0U ||
        duration_us > static_cast<uint64_t>(UINT32_MAX) * 1000U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_haptics_play_request_t request{};
    request.size = sizeof(request);
    request.strength_per_mille = strength_per_mille;
    request.haptic = handle;
    request.duration_ms = static_cast<uint32_t>((duration_us + 999U) / 1000U);
    const int32_t status = CallVoid(haptics_service, MICROPIXEL_HAPTICS_METHOD_PLAY, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> StopHaptic(uint32_t handle) {
    micropixel_handle_request_t request{sizeof(request), 0U, handle};
    const int32_t status = CallVoid(haptics_service, MICROPIXEL_HAPTICS_METHOD_STOP, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

void ReleaseHaptic(uint32_t handle) {
    if (handle == 0U) {
        return;
    }
    micropixel_handle_request_t request{sizeof(request), 0U, handle};
    if (OpenService(haptics_service, MICROPIXEL_SERVICE_HAPTICS, MICROPIXEL_HAPTICS_INTERFACE_MAJOR,
                    MICROPIXEL_HAPTICS_INTERFACE_MINOR) == MICROPIXEL_STATUS_OK) {
        (void)CallVoid(haptics_service, MICROPIXEL_HAPTICS_METHOD_RELEASE, &request, sizeof(request));
    }
}

}  // namespace detail

Result<GpioInfo> Gpio::GetInfo(DeviceId device) const {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(gpio_service, MICROPIXEL_SERVICE_GPIO, MICROPIXEL_GPIO_INTERFACE_MAJOR,
                                 MICROPIXEL_GPIO_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_device_request_t request{sizeof(request), 0U, device.value()};
    micropixel_gpio_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(gpio_service, MICROPIXEL_GPIO_METHOD_GET_INFO, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.device != device.value() ||
        response.line_number == 0U || response.reserved[0] != 0U || response.reserved[1] != 0U ||
        response.reserved[2] != 0U) {
        runtime::Panic("gpio.info.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return GpioInfo{device, response.line_number, response.capabilities, response.maximum_pwm_frequency_hz};
}

Result<GpioInput> Gpio::OpenInput(DeviceId device, GpioInputOptions options) const {
    auto result = detail::OpenGpio(device, MICROPIXEL_GPIO_MODE_INPUT, static_cast<uint16_t>(options.pull),
                                   static_cast<uint16_t>(options.edge), 0U, 0U);
    return result ? Result<GpioInput>{GpioInput{result->handle, result->device}}
                  : Result<GpioInput>{unexpected(result.error())};
}

Result<GpioOutput> Gpio::OpenOutput(DeviceId device, bool initial_value) const {
    auto result = detail::OpenGpio(device, MICROPIXEL_GPIO_MODE_OUTPUT, MICROPIXEL_GPIO_PULL_NONE,
                                   MICROPIXEL_GPIO_EDGE_NONE, initial_value ? 1U : 0U, 0U);
    return result ? Result<GpioOutput>{GpioOutput{result->handle, result->device}}
                  : Result<GpioOutput>{unexpected(result.error())};
}

Result<GpioPwm> Gpio::OpenPwm(DeviceId device, uint32_t frequency_hz, uint16_t initial_duty_per_mille) const {
    auto result = detail::OpenGpio(device, MICROPIXEL_GPIO_MODE_PWM, MICROPIXEL_GPIO_PULL_NONE,
                                   MICROPIXEL_GPIO_EDGE_NONE, initial_duty_per_mille, frequency_hz);
    return result ? Result<GpioPwm>{GpioPwm{result->handle, result->device}}
                  : Result<GpioPwm>{unexpected(result.error())};
}

Result<HapticsInfo> Haptics::GetInfo(DeviceId device) const {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(haptics_service, MICROPIXEL_SERVICE_HAPTICS, MICROPIXEL_HAPTICS_INTERFACE_MAJOR,
                                 MICROPIXEL_HAPTICS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_device_request_t request{sizeof(request), 0U, device.value()};
    micropixel_haptics_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(haptics_service, MICROPIXEL_HAPTICS_METHOD_GET_INFO, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.device != device.value() ||
        response.maximum_duration_ms == 0U || response.reserved[0] != 0U || response.reserved[1] != 0U) {
        runtime::Panic("haptics.info.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return HapticsInfo{device, Duration::Milliseconds(response.maximum_duration_ms),
                       (response.capabilities & MICROPIXEL_HAPTICS_CAP_VARIABLE_STRENGTH) != 0U};
}

Result<Haptic> Haptics::Open(DeviceId device) const {
    auto result = detail::OpenHaptic(device);
    return result ? Result<Haptic>{Haptic{*result, device}} : Result<Haptic>{unexpected(result.error())};
}

Result<PowerState> PowerInfo::Get(DeviceId device) const {
    if (!device.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    int32_t status = OpenService(power_info_service, MICROPIXEL_SERVICE_POWER_INFO,
                                 MICROPIXEL_POWER_INFO_INTERFACE_MAJOR, MICROPIXEL_POWER_INFO_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_device_request_t request{sizeof(request), 0U, device.value()};
    micropixel_power_info_response_t response{};
    uint32_t response_size = 0U;
    status = CallService(power_info_service, MICROPIXEL_POWER_INFO_METHOD_GET, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    constexpr uint32_t kKnownFlags = MICROPIXEL_POWER_STATE_HAS_BATTERY | MICROPIXEL_POWER_STATE_CHARGING |
                                     MICROPIXEL_POWER_STATE_DISCHARGING | MICROPIXEL_POWER_STATE_EXTERNAL_CONNECTED;
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.device != device.value() ||
        response.source > MICROPIXEL_POWER_SOURCE_EXTERNAL || response.battery_percent > 100U ||
        (response.flags & ~kKnownFlags) != 0U || response.reserved0[0] != 0U || response.reserved0[1] != 0U ||
        response.reserved0[2] != 0U || response.reserved1[0] != 0U || response.reserved1[1] != 0U) {
        runtime::Panic("power.info.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return PowerState{device,
                      static_cast<PowerSource>(response.source),
                      response.battery_percent,
                      (response.flags & MICROPIXEL_POWER_STATE_HAS_BATTERY) != 0U,
                      (response.flags & MICROPIXEL_POWER_STATE_CHARGING) != 0U,
                      (response.flags & MICROPIXEL_POWER_STATE_DISCHARGING) != 0U,
                      (response.flags & MICROPIXEL_POWER_STATE_EXTERNAL_CONNECTED) != 0U};
}

}  // namespace micropixel
