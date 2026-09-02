#include "runtime/abi/service_endpoints.hpp"

#include <cinttypes>
#include <cstring>

#include "device/text.hpp"
#include "esp_log.h"
#include "runtime/guest_context.hpp"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_abi";

template <typename Value>
int32_t WriteValue(const Value& value, uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (response == nullptr || response_capacity < sizeof(Value)) {
        response_size_out = sizeof(Value);
        return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    }
    std::memcpy(response, &value, sizeof(Value));
    response_size_out = sizeof(Value);
    return MICROPIXEL_STATUS_OK;
}

template <typename Value, typename Result>
int32_t WriteResult(const Result& result, uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (!result) {
        return result.error().status;
    }
    return WriteValue<Value>(*result, response, response_capacity, response_size_out);
}

template <typename Result>
int32_t ResultStatus(const Result& result) {
    if (result) {
        return MICROPIXEL_STATUS_OK;
    }
    return result.error().status;
}

template <typename Value>
bool ReadRequest(const uint8_t* request, uint32_t request_size, Value& value_out) {
    if (request == nullptr || request_size < sizeof(Value)) {
        return false;
    }
    std::memcpy(&value_out, request, sizeof(Value));
    return value_out.size >= sizeof(Value) && value_out.size <= request_size;
}

bool EmptyRequest(uint32_t request_size) { return request_size == 0U; }

int32_t WriteHandle(uint32_t handle, uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    micropixel_handle_response_t wire{};
    wire.size = sizeof(wire);
    wire.handle = handle;
    if (response == nullptr || response_capacity < sizeof(wire)) {
        response_size_out = sizeof(wire);
        return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    }
    std::memcpy(response, &wire, sizeof(wire));
    response_size_out = sizeof(wire);
    return MICROPIXEL_STATUS_OK;
}

bool ReadHandle(const uint8_t* request, uint32_t request_size, uint32_t& handle_out) {
    micropixel_handle_request_t wire{};
    if (!ReadRequest(request, request_size, wire) || wire.handle == 0U) {
        return false;
    }
    handle_out = wire.handle;
    return true;
}

}  // namespace

ServiceDescriptor DevicesServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_DEVICES,
        .interface_major = MICROPIXEL_DEVICES_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_DEVICES_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_request_bytes = sizeof(micropixel_devices_list_request_t),
        .max_response_bytes = sizeof(micropixel_devices_list_response_t),
    };
}

int32_t DevicesServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                     uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_DEVICES_METHOD_LIST) {
        micropixel_devices_list_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_devices_list_response_t>(context_.DevicesList(wire.kind), response,
                                                               response_capacity, response_size_out);
    }
    if (method_id == MICROPIXEL_DEVICES_METHOD_GET_INFO) {
        micropixel_device_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.device == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_device_info_t>(context_.DeviceInfo(wire.device), response, response_capacity,
                                                     response_size_out);
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor SensorsServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_SENSORS,
        .interface_major = MICROPIXEL_SENSORS_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_SENSORS_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_request_bytes = sizeof(micropixel_sensor_sample_interval_request_t),
        .max_response_bytes = sizeof(micropixel_sensor_reading_t),
    };
}

int32_t SensorsServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                     uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_SENSORS_METHOD_GET_INFO) {
        micropixel_device_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.device == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_sensor_info_t>(context_.SensorInfo(wire.device), response, response_capacity,
                                                     response_size_out);
    }
    if (method_id == MICROPIXEL_SENSORS_METHOD_OPEN) {
        micropixel_sensor_open_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.device == 0U ||
            wire.expected_kind == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_sensor_open_response_t>(context_.SensorOpen(wire.device, wire.expected_kind),
                                                              response, response_capacity, response_size_out);
    }
    if (method_id == MICROPIXEL_SENSORS_METHOD_SET_SAMPLE_INTERVAL) {
        micropixel_sensor_sample_interval_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.sensor == 0U || wire.interval_us == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.SensorSetSampleInterval(wire.sensor, wire.interval_us));
    }
    uint32_t handle = 0U;
    if (!ReadHandle(request, request_size, handle)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_SENSORS_METHOD_READ) {
        return WriteResult<micropixel_sensor_reading_t>(context_.SensorRead(handle), response, response_capacity,
                                                        response_size_out);
    }
    if (method_id == MICROPIXEL_SENSORS_METHOD_RELEASE) {
        return ResultStatus(context_.SensorRelease(handle));
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor GpioServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_GPIO,
        .interface_major = MICROPIXEL_GPIO_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_GPIO_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
        .max_request_bytes = sizeof(micropixel_gpio_open_request_t),
        .max_response_bytes = sizeof(micropixel_gpio_info_t),
    };
}

int32_t GpioServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                                  uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_GPIO_METHOD_GET_INFO) {
        micropixel_device_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.device == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_gpio_info_t>(context_.GpioInfo(wire.device), response, response_capacity,
                                                   response_size_out);
    }
    if (method_id == MICROPIXEL_GPIO_METHOD_OPEN) {
        micropixel_gpio_open_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.device == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_gpio_open_response_t>(context_.GpioOpen(wire), response, response_capacity,
                                                            response_size_out);
    }
    if (method_id == MICROPIXEL_GPIO_METHOD_WRITE || method_id == MICROPIXEL_GPIO_METHOD_SET_PWM_DUTY) {
        micropixel_gpio_value_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.gpio == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        if (method_id == MICROPIXEL_GPIO_METHOD_WRITE) {
            if (wire.value > 1U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            return ResultStatus(context_.GpioWrite(wire.gpio, wire.value != 0U));
        }
        if (wire.value > 1000U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.GpioSetPwmDuty(wire.gpio, static_cast<uint16_t>(wire.value)));
    }
    uint32_t handle = 0U;
    if (!ReadHandle(request, request_size, handle)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_GPIO_METHOD_READ) {
        return WriteResult<micropixel_gpio_value_response_t>(context_.GpioRead(handle), response, response_capacity,
                                                             response_size_out);
    }
    if (method_id == MICROPIXEL_GPIO_METHOD_RELEASE) {
        return ResultStatus(context_.GpioRelease(handle));
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor HapticsServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_HAPTICS,
        .interface_major = MICROPIXEL_HAPTICS_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_HAPTICS_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
        .max_request_bytes = sizeof(micropixel_haptics_play_request_t),
        .max_response_bytes = sizeof(micropixel_haptics_info_t),
    };
}

int32_t HapticsServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                     uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_HAPTICS_METHOD_GET_INFO || method_id == MICROPIXEL_HAPTICS_METHOD_OPEN) {
        micropixel_device_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.device == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        if (method_id == MICROPIXEL_HAPTICS_METHOD_GET_INFO) {
            return WriteResult<micropixel_haptics_info_t>(context_.HapticsInfo(wire.device), response,
                                                          response_capacity, response_size_out);
        }
        return WriteResult<micropixel_handle_response_t>(context_.HapticsOpen(wire.device), response, response_capacity,
                                                         response_size_out);
    }
    if (method_id == MICROPIXEL_HAPTICS_METHOD_PLAY) {
        micropixel_haptics_play_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.haptic == 0U ||
            wire.reserved0 != 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.HapticsPlay(wire));
    }
    uint32_t handle = 0U;
    if (!ReadHandle(request, request_size, handle)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_HAPTICS_METHOD_STOP) {
        return ResultStatus(context_.HapticsStop(handle));
    }
    if (method_id == MICROPIXEL_HAPTICS_METHOD_RELEASE) {
        return ResultStatus(context_.HapticsRelease(handle));
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor PowerInfoServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_POWER_INFO,
        .interface_major = MICROPIXEL_POWER_INFO_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_POWER_INFO_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_request_bytes = sizeof(micropixel_device_request_t),
        .max_response_bytes = sizeof(micropixel_power_info_response_t),
    };
}

int32_t PowerInfoServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                       uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    micropixel_device_request_t wire{};
    if (method_id != MICROPIXEL_POWER_INFO_METHOD_GET) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
        wire.device == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return WriteResult<micropixel_power_info_response_t>(context_.PowerInfo(wire.device), response, response_capacity,
                                                         response_size_out);
}

SystemServiceEndpoint::SystemServiceEndpoint(std::string_view effective_locale,
                                             const micropixel_system_launch_arguments_response_t& launch_arguments)
    : launch_arguments_(launch_arguments) {
    if (effective_locale.empty() || effective_locale.size() > MICROPIXEL_LOCALE_TAG_MAX_BYTES) {
        effective_locale = "en";
    }
    effective_locale_length_ = static_cast<uint16_t>(effective_locale.size());
    std::memcpy(effective_locale_.data(), effective_locale.data(), effective_locale.size());
}

ServiceDescriptor SystemServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_SYSTEM,
        .interface_major = MICROPIXEL_SYSTEM_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_SYSTEM_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_response_bytes = sizeof(micropixel_system_launch_arguments_response_t),
    };
}

int32_t SystemServiceEndpoint::Call(uint32_t method_id, const uint8_t*, uint32_t request_size, uint8_t* response,
                                    uint32_t response_capacity, uint32_t& response_size_out) {
    if (!EmptyRequest(request_size)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_SYSTEM_METHOD_GET_LOCALE) {
        micropixel_system_locale_response_t locale{};
        locale.size = sizeof(locale);
        locale.tag_length = effective_locale_length_;
        std::memcpy(locale.tag, effective_locale_.data(), effective_locale_length_);
        return WriteValue(locale, response, response_capacity, response_size_out);
    }
    if (method_id == MICROPIXEL_SYSTEM_METHOD_GET_LAUNCH_ARGUMENTS) {
        return WriteValue(launch_arguments_, response, response_capacity, response_size_out);
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor TimerServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_TIMER,
        .interface_major = 1U,
        .interface_minor = 0U,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
        .max_request_bytes = sizeof(micropixel_timer_start_request_t),
        .max_response_bytes = sizeof(micropixel_handle_response_t),
    };
}

int32_t TimerServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                                   uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_TIMER_METHOD_CREATE) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        auto result = context_.TimerCreate();
        return result ? WriteHandle(*result, response, response_capacity, response_size_out) : result.error().status;
    }
    if (method_id == MICROPIXEL_TIMER_METHOD_START) {
        micropixel_timer_start_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.timer == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.TimerStart(wire.timer, wire.initial_delay_us, wire.period_us));
    }
    uint32_t handle = 0U;
    if (!ReadHandle(request, request_size, handle)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_TIMER_METHOD_CANCEL) {
        return ResultStatus(context_.TimerCancel(handle));
    }
    if (method_id == MICROPIXEL_TIMER_METHOD_RELEASE) {
        return ResultStatus(context_.TimerRelease(handle));
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor StorageServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_STORAGE,
        .interface_major = 1U,
        .interface_minor = 0U,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_request_bytes = sizeof(micropixel_storage_set_request_t) + MICROPIXEL_STORAGE_MAX_KEY_BYTES +
                             CONFIG_MICROPIXEL_KV_MAX_VALUE_BYTES,
        .max_response_bytes = CONFIG_MICROPIXEL_KV_MAX_VALUE_BYTES,
    };
}

int32_t StorageServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                     uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_STORAGE_METHOD_SET) {
        micropixel_storage_set_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.key_length == 0U ||
            wire.key_length > MICROPIXEL_STORAGE_MAX_KEY_BYTES ||
            wire.value_length > CONFIG_MICROPIXEL_KV_MAX_VALUE_BYTES ||
            wire.size != sizeof(wire) + wire.key_length + wire.value_length || wire.size != request_size) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        const char* key = reinterpret_cast<const char*>(request + sizeof(wire));
        const uint8_t* value = request + sizeof(wire) + wire.key_length;
        return ResultStatus(context_.KvSetBytes(key, wire.key_length, value, wire.value_length));
    }

    micropixel_storage_key_request_t wire{};
    if (!ReadRequest(request, request_size, wire) || wire.key_length == 0U ||
        wire.key_length > MICROPIXEL_STORAGE_MAX_KEY_BYTES) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_STORAGE_METHOD_GET) {
        auto result = context_.KvGetBytes(wire.key, wire.key_length, response, response_capacity);
        if (!result) {
            if (result.error().status == MICROPIXEL_STATUS_BUFFER_TOO_SMALL) {
                response_size_out = result.error().detail;
            }
            return result.error().status;
        }
        response_size_out = *result;
        return MICROPIXEL_STATUS_OK;
    }
    if (method_id == MICROPIXEL_STORAGE_METHOD_REMOVE) {
        return ResultStatus(context_.KvRemove(wire.key, wire.key_length));
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor ResourceServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_RESOURCE,
        .interface_major = MICROPIXEL_RESOURCE_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_RESOURCE_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_request_bytes = MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES,
        .max_response_bytes = sizeof(micropixel_adaptive_texture_info_t),
    };
}

int32_t ResourceServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                      uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_RESOURCE_METHOD_LOAD_TEXTURE) {
        micropixel_resource_load_texture_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.asset_id == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_texture_info_t>(context_.LoadTexture(wire.asset_id), response, response_capacity,
                                                      response_size_out);
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_LOAD_ADAPTIVE_TEXTURE) {
        micropixel_resource_load_adaptive_texture_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.asset_id == 0U || wire.scale_numerator == 0U || wire.scale_denominator == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_adaptive_texture_info_t>(
            context_.LoadAdaptiveTexture(wire.asset_id, wire.scale_numerator, wire.scale_denominator), response,
            response_capacity, response_size_out);
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_LOAD_FONT) {
        micropixel_resource_load_font_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U ||
            wire.resource_id == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_font_info_t>(context_.LoadFont(wire.resource_id), response, response_capacity,
                                                   response_size_out);
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_CREATE) {
        micropixel_streaming_texture_create_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != sizeof(wire) || wire.reserved0 != 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_texture_info_t>(
            context_.CreateStreamingTexture(wire.width, wire.height, wire.pixel_format), response, response_capacity,
            response_size_out);
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_UPDATE) {
        micropixel_streaming_texture_update_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != request_size || wire.reserved0 != 0U ||
            wire.reserved1 != 0U || wire.texture == 0U || wire.width == 0U || wire.height == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        const uint64_t pixel_bytes = static_cast<uint64_t>(wire.pitch) * wire.height;
        if (pixel_bytes > UINT32_MAX || sizeof(wire) + pixel_bytes != request_size) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.UpdateStreamingTexture(wire, request + sizeof(wire)));
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_BEGIN) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.BeginTextureUpdateBatch());
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_FINISH) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.FinishTextureUpdateBatch());
    }

    uint32_t handle = 0U;
    if (!ReadHandle(request, request_size, handle)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_TEXTURE_RELEASE) {
        return ResultStatus(context_.ReleaseTexture(handle));
    }
    if (method_id == MICROPIXEL_RESOURCE_METHOD_FONT_RELEASE && handle <= UINT16_MAX) {
        return ResultStatus(context_.ReleaseFont(static_cast<micropixel_font_handle_t>(handle)));
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

ServiceDescriptor RandomServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_RANDOM,
        .interface_major = MICROPIXEL_RANDOM_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_RANDOM_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_response_bytes = sizeof(micropixel_random_u32_response_t),
    };
}

int32_t RandomServiceEndpoint::Call(uint32_t method_id, const uint8_t*, uint32_t request_size, uint8_t* response,
                                    uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id != MICROPIXEL_RANDOM_METHOD_GET_U32) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (!EmptyRequest(request_size)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    auto result = context_.RandomU32();
    if (!result) {
        return result.error().status;
    }
    micropixel_random_u32_response_t wire{};
    wire.size = sizeof(wire);
    wire.value = *result;
    return WriteValue(wire, response, response_capacity, response_size_out);
}

ServiceDescriptor GraphicsServiceEndpoint::Describe() const {
    auto result = context_.GraphicsInfo();
    const uint32_t max_submit_bytes = result ? result->max_scene_bytes : MICROPIXEL_GRAPHICS_MAX_SCENE_BYTES;
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_GRAPHICS,
        .interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_SUBMIT,
        .capabilities = 0U,
        .max_request_bytes = sizeof(micropixel_graphics_measure_text_request_t) + MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES,
        .max_response_bytes = sizeof(micropixel_graphics_info_t),
        .max_submit_bytes = max_submit_bytes,
    };
}

int32_t GraphicsServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size,
                                      uint8_t* response, uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_GRAPHICS_METHOD_GET_INFO) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_graphics_info_t>(context_.GraphicsInfo(), response, response_capacity,
                                                       response_size_out);
    }
    if (method_id == MICROPIXEL_GRAPHICS_METHOD_MEASURE_TEXT) {
        micropixel_graphics_measure_text_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.size != request_size || wire.font == 0U ||
            wire.reserved0 != 0U || wire.text_length == 0U || wire.text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES ||
            sizeof(wire) + wire.text_length != request_size ||
            !device::IsValidUtf8(request + sizeof(wire), wire.text_length)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_text_metrics_t>(
            context_.MeasureText(wire.font, reinterpret_cast<const char*>(request + sizeof(wire)), wire.text_length),
            response, response_capacity, response_size_out);
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

int32_t GraphicsServiceEndpoint::Submit(uint32_t channel_id, const uint8_t* bytes, uint32_t length) {
    if (channel_id != MICROPIXEL_GRAPHICS_CHANNEL_SCENE) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    return ResultStatus(context_.GraphicsSubmit(bytes, length));
}

ServiceDescriptor InputServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_INPUT,
        .interface_major = MICROPIXEL_INPUT_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_INPUT_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
        .max_response_bytes = sizeof(micropixel_input_info_t),
    };
}

int32_t InputServiceEndpoint::Call(uint32_t method_id, const uint8_t*, uint32_t request_size, uint8_t* response,
                                   uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id != MICROPIXEL_INPUT_METHOD_GET_INFO) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (!EmptyRequest(request_size)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return WriteResult<micropixel_input_info_t>(context_.InputInfo(), response, response_capacity, response_size_out);
}

ServiceDescriptor AudioServiceEndpoint::Describe() const {
    auto result = context_.AudioInfo();
    const uint64_t capabilities = result ? result->capabilities : 0U;
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_AUDIO,
        .interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_AUDIO_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
        .capabilities = capabilities,
        .max_request_bytes = sizeof(micropixel_audio_tone_t),
        .max_response_bytes = sizeof(micropixel_audio_info_t),
    };
}

int32_t AudioServiceEndpoint::Call(uint32_t method_id, const uint8_t* request, uint32_t request_size, uint8_t* response,
                                   uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id == MICROPIXEL_AUDIO_METHOD_GET_INFO) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_audio_info_t>(context_.AudioInfo(), response, response_capacity,
                                                    response_size_out);
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_PLAY_TONE) {
        micropixel_audio_tone_t wire{};
        if (!ReadRequest(request, request_size, wire)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        wire.size = sizeof(wire);
        return ResultStatus(context_.AudioPlayTone(wire));
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_STOP_ALL) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.AudioStopAll());
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_CLIP_LOAD) {
        micropixel_audio_clip_load_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.asset_id == 0U || wire.reserved0 != 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return WriteResult<micropixel_audio_clip_info_t>(context_.AudioLoadClip(wire.asset_id), response,
                                                         response_capacity, response_size_out);
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_PLAYBACK_START) {
        micropixel_audio_playback_start_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.clip == 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        auto result = context_.AudioStartPlayback(wire);
        return result ? WriteHandle(*result, response, response_capacity, response_size_out) : result.error().status;
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_PLAYBACK_SET_VOLUME) {
        micropixel_audio_playback_volume_request_t wire{};
        if (!ReadRequest(request, request_size, wire) || wire.playback == 0U || wire.reserved0 != 0U ||
            wire.reserved1 != 0U) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.AudioSetPlaybackVolume(wire.playback, wire.volume_per_mille));
    }
    uint32_t handle = 0U;
    if (!ReadHandle(request, request_size, handle)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_CLIP_RELEASE) {
        return ResultStatus(context_.AudioReleaseClip(handle));
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_PLAYBACK_PAUSE) {
        return ResultStatus(context_.AudioPausePlayback(handle));
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_PLAYBACK_RESUME) {
        return ResultStatus(context_.AudioResumePlayback(handle));
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_PLAYBACK_STOP) {
        return ResultStatus(context_.AudioStopPlayback(handle));
    }
    if (method_id == MICROPIXEL_AUDIO_METHOD_PLAYBACK_GET_STATE) {
        return WriteResult<micropixel_audio_playback_state_response_t>(context_.AudioPlaybackState(handle), response,
                                                                       response_capacity, response_size_out);
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

}  // namespace micropixel::runtime
