#include "runtime/abi/service_endpoints.hpp"

#include <cinttypes>
#include <cstring>

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
    return result ? static_cast<int32_t>(MICROPIXEL_STATUS_OK) : result.error().status;
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

ServiceDescriptor SystemServiceEndpoint::Describe() const {
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_SYSTEM,
        .interface_major = MICROPIXEL_SYSTEM_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_SYSTEM_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
        .max_response_bytes = sizeof(micropixel_system_locale_response_t),
    };
}

int32_t SystemServiceEndpoint::Call(uint32_t method_id, const uint8_t*, uint32_t request_size, uint8_t* response,
                                    uint32_t response_capacity, uint32_t& response_size_out) {
    if (method_id != MICROPIXEL_SYSTEM_METHOD_GET_LOCALE) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    if (!EmptyRequest(request_size)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    micropixel_system_locale_response_t locale{};
    locale.size = sizeof(locale);
    locale.tag_length = 2U;
    locale.tag[0] = 'e';
    locale.tag[1] = 'n';
    return WriteValue(locale, response, response_capacity, response_size_out);
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
        .max_response_bytes = sizeof(micropixel_texture_info_t),
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
    const uint64_t capabilities = result ? result->capabilities : 0U;
    const uint32_t max_submit_bytes = result ? result->max_command_bytes : MICROPIXEL_GRAPHICS_MAX_COMMAND_BYTES;
    return ServiceDescriptor{
        .service_id = MICROPIXEL_SERVICE_GRAPHICS,
        .interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_SUBMIT,
        .capabilities = capabilities,
        .max_request_bytes = 0U,
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
    if (method_id == MICROPIXEL_GRAPHICS_METHOD_FRAME_BEGIN) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.GraphicsBeginFrame());
    }
    if (method_id == MICROPIXEL_GRAPHICS_METHOD_FRAME_COMMIT) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.GraphicsCommitFrame());
    }
    if (method_id == MICROPIXEL_GRAPHICS_METHOD_FRAME_CANCEL) {
        if (!EmptyRequest(request_size)) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return ResultStatus(context_.GraphicsCancelFrame());
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

int32_t GraphicsServiceEndpoint::Submit(uint32_t channel_id, const uint8_t* bytes, uint32_t length) {
    if (channel_id != MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS) {
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
        .flags = MICROPIXEL_SERVICE_FLAG_CALL,
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
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

}  // namespace micropixel::runtime
