#include <stdint.h>

#include "abi/micropixel_abi.h"
#include "runtime/panic.hpp"
#include "sdk/application.hpp"
#include "sdk/panic.hpp"
#include "sdk/resources.hpp"
#include "sdk/storage.hpp"

namespace {

uint32_t BoundedLength(const char* message) {
    uint32_t length = 0U;
    while (length < MICROPIXEL_ABI_MAX_LOG_BYTES && message[length] != '\0') {
        ++length;
    }
    return length;
}

const char* StatusName(int32_t status) {
    switch (status) {
        case MICROPIXEL_STATUS_INVALID_ARGUMENT:
            return "invalid_argument";
        case MICROPIXEL_STATUS_INVALID_MEMORY:
            return "invalid_memory";
        case MICROPIXEL_STATUS_UNSUPPORTED:
            return "unsupported";
        case MICROPIXEL_STATUS_RESOURCE_EXHAUSTED:
            return "resource_exhausted";
        case MICROPIXEL_STATUS_INTERNAL:
            return "internal";
        case MICROPIXEL_STATUS_NOT_FOUND:
            return "not_found";
        case MICROPIXEL_STATUS_PERMISSION_DENIED:
            return "permission_denied";
        case MICROPIXEL_STATUS_BUFFER_TOO_SMALL:
            return "buffer_too_small";
        case MICROPIXEL_STATUS_RATE_LIMITED:
            return "rate_limited";
        case MICROPIXEL_STATUS_WOULD_BLOCK:
            return "would_block";
        case MICROPIXEL_STATUS_TIMEOUT:
            return "timeout";
        case MICROPIXEL_STATUS_CANCELLED:
            return "cancelled";
        case MICROPIXEL_STATUS_CLOSED:
            return "closed";
        case MICROPIXEL_STATUS_VERSION_MISMATCH:
            return "version_mismatch";
        default:
            return "internal";
    }
}

void DiagnosticLine(const char* message) {
    uint32_t length = BoundedLength(message);
    if (length < MICROPIXEL_ABI_MAX_LOG_BYTES) {
        (void)micropixel_log_write(MICROPIXEL_LOG_ERROR, reinterpret_cast<const uint8_t*>(message), length);
    }
}

void RequireOk(int32_t status, const char* operation) {
    if (status != MICROPIXEL_STATUS_OK) {
        micropixel::runtime::Panic(operation, status);
    }
}

void CopyBytes(void* destination, const void* source, uint32_t length) {
    auto* output = static_cast<uint8_t*>(destination);
    const auto* input = static_cast<const uint8_t*>(source);
    for (uint32_t index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

void ClearBytes(void* destination, uint32_t length) {
    auto* output = static_cast<uint8_t*>(destination);
    for (uint32_t index = 0U; index < length; ++index) {
        output[index] = 0U;
    }
}

micropixel::Error ErrorFromStatus(int32_t status) {
    using micropixel::Error;
    using micropixel::ErrorCode;
    switch (status) {
        case MICROPIXEL_STATUS_INVALID_ARGUMENT:
        case MICROPIXEL_STATUS_INVALID_MEMORY:
            return Error{ErrorCode::kInvalidArgument};
        case MICROPIXEL_STATUS_UNSUPPORTED:
            return Error{ErrorCode::kUnsupported};
        case MICROPIXEL_STATUS_RESOURCE_EXHAUSTED:
            return Error{ErrorCode::kResourceExhausted};
        case MICROPIXEL_STATUS_NOT_FOUND:
            return Error{ErrorCode::kNotFound};
        case MICROPIXEL_STATUS_PERMISSION_DENIED:
            return Error{ErrorCode::kPermissionDenied};
        case MICROPIXEL_STATUS_BUFFER_TOO_SMALL:
            return Error{ErrorCode::kBufferTooSmall};
        case MICROPIXEL_STATUS_RATE_LIMITED:
            return Error{ErrorCode::kRateLimited};
        default:
            return Error{ErrorCode::kInternal};
    }
}

struct ServiceCache final {
    micropixel_service_info_t info{};
    int32_t status{MICROPIXEL_STATUS_INTERNAL};
    bool attempted{};
};

ServiceCache timer_service;
ServiceCache storage_service;
ServiceCache resource_service;
ServiceCache random_service;
ServiceCache graphics_service;
ServiceCache input_service;
ServiceCache audio_service;
micropixel_graphics_info_t cached_graphics_info{};
bool graphics_info_loaded{};

int32_t OpenService(ServiceCache& cache, uint32_t service_id, uint16_t interface_major, uint16_t minimum_minor) {
    if (!cache.attempted) {
        cache.attempted = true;
        cache.status = micropixel_service_open(service_id, MICROPIXEL_INTERFACE_VERSION(interface_major, minimum_minor),
                                               &cache.info, sizeof(cache.info));
        if (cache.status == MICROPIXEL_STATUS_OK &&
            (cache.info.size < sizeof(cache.info) || cache.info.service_id != service_id || cache.info.handle == 0U ||
             cache.info.interface_major != interface_major || cache.info.interface_minor < minimum_minor)) {
            cache.status = MICROPIXEL_STATUS_VERSION_MISMATCH;
        }
    }
    return cache.status;
}

int32_t CallService(ServiceCache& cache, uint32_t method_id, const void* request, uint32_t request_size, void* response,
                    uint32_t response_capacity, uint32_t& response_size_out) {
    if (cache.status != MICROPIXEL_STATUS_OK || !cache.attempted) {
        return cache.attempted ? cache.status : MICROPIXEL_STATUS_INTERNAL;
    }
    return micropixel_service_call(cache.info.handle, method_id, static_cast<const uint8_t*>(request), request_size,
                                   static_cast<uint8_t*>(response), response_capacity, &response_size_out);
}

int32_t CallVoid(ServiceCache& cache, uint32_t method_id, const void* request, uint32_t request_size) {
    uint32_t response_size = 0U;
    int32_t status = CallService(cache, method_id, request, request_size, nullptr, 0U, response_size);
    return status == MICROPIXEL_STATUS_OK && response_size != 0U ? MICROPIXEL_STATUS_INTERNAL : status;
}

int32_t OpenOffscreenSurfaceService() {
    const int32_t status = OpenService(resource_service, MICROPIXEL_SERVICE_RESOURCE,
                                       MICROPIXEL_RESOURCE_INTERFACE_MAJOR, MICROPIXEL_RESOURCE_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return status;
    }
    return resource_service.info.interface_minor >= MICROPIXEL_RESOURCE_INTERFACE_MINOR
               ? MICROPIXEL_STATUS_OK
               : MICROPIXEL_STATUS_VERSION_MISMATCH;
}

bool StorageKeyLength(const char* key, uint32_t& length_out);

bool FillStorageKeyRequest(const char* key, micropixel_storage_key_request_t& request_out) {
    uint32_t key_length = 0U;
    if (!StorageKeyLength(key, key_length)) {
        return false;
    }
    request_out = {};
    request_out.size = sizeof(request_out);
    request_out.key_length = static_cast<uint16_t>(key_length);
    CopyBytes(request_out.key, key, key_length);
    return true;
}

int32_t GetStorageValue(const char* key, uint8_t* bytes, uint32_t capacity, uint32_t& size_out) {
    micropixel_storage_key_request_t request{};
    if (!FillStorageKeyRequest(key, request) || (bytes == nullptr && capacity != 0U) ||
        capacity > MICROPIXEL_STORAGE_MAX_VALUE_BYTES) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    int32_t status = OpenService(storage_service, MICROPIXEL_SERVICE_STORAGE, 1U, 0U);
    return status == MICROPIXEL_STATUS_OK ? CallService(storage_service, MICROPIXEL_STORAGE_METHOD_GET, &request,
                                                        sizeof(request), bytes, capacity, size_out)
                                          : status;
}

template <uint32_t ValueCapacity>
int32_t SetStorageValue(const char* key, const uint8_t* bytes, uint32_t length) {
    uint32_t key_length = 0U;
    if (!StorageKeyLength(key, key_length) || (bytes == nullptr && length != 0U) || length > ValueCapacity) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    uint8_t request[sizeof(micropixel_storage_set_request_t) + MICROPIXEL_STORAGE_MAX_KEY_BYTES + ValueCapacity]{};
    const uint32_t request_size = sizeof(micropixel_storage_set_request_t) + key_length + length;
    micropixel_storage_set_request_t header{};
    header.size = static_cast<uint16_t>(request_size);
    header.key_length = static_cast<uint16_t>(key_length);
    header.value_length = length;
    CopyBytes(request, &header, sizeof(header));
    CopyBytes(request + sizeof(header), key, key_length);
    CopyBytes(request + sizeof(header) + key_length, bytes, length);
    int32_t status = OpenService(storage_service, MICROPIXEL_SERVICE_STORAGE, 1U, 0U);
    return status == MICROPIXEL_STATUS_OK
               ? CallVoid(storage_service, MICROPIXEL_STORAGE_METHOD_SET, request, request_size)
               : status;
}

bool StorageKeyLength(const char* key, uint32_t& length_out) {
    if (key == nullptr) {
        return false;
    }
    uint32_t length = 0U;
    while (length < 16U && key[length] != '\0') {
        ++length;
    }
    if (length == 0U || length >= 16U) {
        return false;
    }
    length_out = length;
    return true;
}

}  // namespace

namespace micropixel::runtime {

[[noreturn]] void Panic(const char* operation, int32_t status) {
    DiagnosticLine("guest panic");
    DiagnosticLine(operation);
    DiagnosticLine(StatusName(status));
    __builtin_trap();
}

}  // namespace micropixel::runtime

namespace micropixel {

static_assert(CommandBuffer::kCapacityBytes == MICROPIXEL_GRAPHICS_MAX_COMMAND_BYTES,
              "SDK/ABI graphics command byte capacity drifted");
static_assert(CommandBuffer::kCapacityCommands == MICROPIXEL_GRAPHICS_MAX_COMMANDS,
              "SDK/ABI graphics command count capacity drifted");

Result<uint32_t> KVStore::GetU32(const char* key) const {
    uint8_t wire[4]{};
    uint32_t size = 0U;
    int32_t status = GetStorageValue(key, wire, sizeof(wire), size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (size != sizeof(wire)) {
        return unexpected(Error{ErrorCode::kInternal});
    }
    return static_cast<uint32_t>(wire[0]) | (static_cast<uint32_t>(wire[1]) << 8U) |
           (static_cast<uint32_t>(wire[2]) << 16U) | (static_cast<uint32_t>(wire[3]) << 24U);
}

Result<void> KVStore::SetU32(const char* key, uint32_t value) const {
    const uint8_t wire[4]{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U),
                          static_cast<uint8_t>(value >> 16U), static_cast<uint8_t>(value >> 24U)};
    int32_t status = SetStorageValue<sizeof(wire)>(key, wire, sizeof(wire));
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

Result<bool> KVStore::GetBool(const char* key) const {
    uint8_t value = 0U;
    uint32_t size = 0U;
    int32_t status = GetStorageValue(key, &value, sizeof(value), size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (size != sizeof(value) || value > 1U) {
        return unexpected(Error{ErrorCode::kInternal});
    }
    return value == 1U;
}

Result<void> KVStore::SetBool(const char* key, bool value) const {
    const uint8_t wire = value ? 1U : 0U;
    int32_t status = SetStorageValue<sizeof(wire)>(key, &wire, sizeof(wire));
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

Result<uint32_t> KVStore::GetBytes(const char* key, uint8_t* bytes, uint32_t capacity) const {
    uint32_t size = 0U;
    int32_t status = GetStorageValue(key, bytes, capacity, size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return size;
}

Result<void> KVStore::SetBytes(const char* key, const uint8_t* bytes, uint32_t length) const {
    int32_t status = SetStorageValue<MICROPIXEL_STORAGE_MAX_VALUE_BYTES>(key, bytes, length);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

Result<void> KVStore::Remove(const char* key) const {
    micropixel_storage_key_request_t request{};
    if (!FillStorageKeyRequest(key, request)) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    int32_t status = OpenService(storage_service, MICROPIXEL_SERVICE_STORAGE, 1U, 0U);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(storage_service, MICROPIXEL_STORAGE_METHOD_REMOVE, &request, sizeof(request));
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

[[noreturn]] void Panic(const char* reason) {
    if (reason == nullptr || BoundedLength(reason) == MICROPIXEL_ABI_MAX_LOG_BYTES) {
        runtime::Panic("panic.reason", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    DiagnosticLine("guest panic");
    DiagnosticLine(reason);
    __builtin_trap();
}

void Log::Info(const char* message) const {
    if (message == nullptr) {
        runtime::Panic("log.info", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    uint32_t length = BoundedLength(message);
    if (length == MICROPIXEL_ABI_MAX_LOG_BYTES) {
        runtime::Panic("log.info", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    RequireOk(micropixel_log_write(MICROPIXEL_LOG_INFO, reinterpret_cast<const uint8_t*>(message), length), "log.info");
}

TimePoint Clock::Now() const { return TimePoint{micropixel_clock_now()}; }

uint32_t Random::U32() const {
    RequireOk(OpenService(random_service, MICROPIXEL_SERVICE_RANDOM, MICROPIXEL_RANDOM_INTERFACE_MAJOR,
                          MICROPIXEL_RANDOM_INTERFACE_MINOR),
              "random.open");
    micropixel_random_u32_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(random_service, MICROPIXEL_RANDOM_METHOD_GET_U32, nullptr, 0U, &response, sizeof(response),
                          response_size),
              "random.u32");
    if (response_size < sizeof(response) || response.size < sizeof(response)) {
        runtime::Panic("random.u32.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return response.value;
}

Result<AudioInfo> Audio::info() const {
    micropixel_audio_info_t raw{};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    uint32_t response_size = 0U;
    if (status == MICROPIXEL_STATUS_OK) {
        status =
            CallService(audio_service, MICROPIXEL_AUDIO_METHOD_GET_INFO, nullptr, 0U, &raw, sizeof(raw), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(raw) || raw.size < sizeof(raw) ||
        raw.interface_major != MICROPIXEL_AUDIO_INTERFACE_MAJOR ||
        raw.interface_minor < MICROPIXEL_AUDIO_INTERFACE_MINOR) {
        return unexpected(Error{ErrorCode::kUnsupported});
    }
    return AudioInfo{
        raw.sample_rate,
        raw.max_voices,
        raw.supported_waveforms,
        Duration::Milliseconds(raw.max_tone_duration_ms),
    };
}

Result<void> Audio::Play(const Tone& tone) const {
    const uint64_t duration_us = tone.duration.count_microseconds();
    const uint64_t attack_us = tone.attack.count_microseconds();
    const uint64_t release_us = tone.release.count_microseconds();
    const uint32_t waveform = static_cast<uint32_t>(tone.waveform);
    if (duration_us == 0U || duration_us > static_cast<uint64_t>(MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS) * 1000U ||
        attack_us > duration_us || release_us > duration_us || tone.volume_per_mille > 1000U ||
        waveform < MICROPIXEL_AUDIO_WAVE_SINE || waveform > MICROPIXEL_AUDIO_WAVE_NOISE ||
        (tone.waveform != Waveform::kNoise && (tone.frequency_hz < 20U || tone.frequency_hz > 20000U))) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_audio_tone_t raw{};
    raw.size = sizeof(raw);
    raw.interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR;
    raw.waveform = static_cast<uint16_t>(tone.waveform);
    raw.volume_per_mille = tone.volume_per_mille;
    raw.frequency_millihz = tone.frequency_hz * 1000U;
    raw.duration_ms = static_cast<uint32_t>((duration_us + 999U) / 1000U);
    raw.attack_ms = static_cast<uint16_t>((attack_us + 999U) / 1000U);
    raw.release_ms = static_cast<uint16_t>((release_us + 999U) / 1000U);
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAY_TONE, &raw, sizeof(raw));
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

Result<void> Audio::StopAll() const {
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_STOP_ALL, nullptr, 0U);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    return {};
}

Timer::~Timer() { Release(); }

void Timer::Cancel() {
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    RequireOk(OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U), "timer.cancel.open");
    RequireOk(CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_CANCEL, &request, sizeof(request)), "timer.cancel");
}

void Timer::Release() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        int32_t status = OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U);
        if (status == MICROPIXEL_STATUS_OK) {
            status = CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_RELEASE, &request, sizeof(request));
        }
        handle_ = 0U;
        RequireOk(status, "timer.release");
    }
}

Timer Timers::After(Duration delay) const {
    RequireOk(OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U), "timers.after.open");
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(timer_service, MICROPIXEL_TIMER_METHOD_CREATE, nullptr, 0U, &response, sizeof(response),
                          response_size),
              "timers.after.create");
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("timers.after.response", MICROPIXEL_STATUS_INTERNAL);
    }
    Timer timer{response.handle};
    micropixel_timer_start_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, response.handle,
                                             delay.count_microseconds(), 0U};
    RequireOk(CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_START, &request, sizeof(request)), "timers.after.start");
    return timer;
}

Timer Timers::Every(Duration period) const {
    RequireOk(OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U), "timers.every.open");
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(timer_service, MICROPIXEL_TIMER_METHOD_CREATE, nullptr, 0U, &response, sizeof(response),
                          response_size),
              "timers.every.create");
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("timers.every.response", MICROPIXEL_STATUS_INTERNAL);
    }
    Timer timer{response.handle};
    micropixel_timer_start_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, response.handle,
                                             period.count_microseconds(), period.count_microseconds()};
    RequireOk(CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_START, &request, sizeof(request)), "timers.every.start");
    return timer;
}

GraphicsInfo Graphics::info() const {
    RequireOk(OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                          MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
              "graphics.open");
    if (!graphics_info_loaded) {
        uint32_t response_size = 0U;
        RequireOk(CallService(graphics_service, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U, &cached_graphics_info,
                              sizeof(cached_graphics_info), response_size),
                  "graphics.info");
        if (response_size < sizeof(cached_graphics_info) || cached_graphics_info.size < sizeof(cached_graphics_info) ||
            cached_graphics_info.interface_major != MICROPIXEL_GRAPHICS_INTERFACE_MAJOR ||
            cached_graphics_info.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB888 ||
            cached_graphics_info.max_command_bytes < CommandBuffer::kCapacityBytes ||
            cached_graphics_info.max_commands == 0U) {
            runtime::Panic("graphics.info.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        graphics_info_loaded = true;
    }
    const micropixel_graphics_info_t& raw = cached_graphics_info;
    return GraphicsInfo{raw.width,        raw.height,         raw.capabilities,      raw.max_command_bytes,
                        raw.max_commands, raw.max_text_bytes, raw.max_frame_commands};
}

CommandBuffer Graphics::CreateCommandBuffer() const {
    const GraphicsInfo graphics_info = info();
    return CreateCommandBuffer(graphics_info);
}

CommandBuffer Graphics::CreateCommandBuffer(const GraphicsInfo& graphics_info) const {
    return CommandBuffer{CommandBuffer::CapabilityToken{}, graphics_info.max_commands(), graphics_info.max_commands(),
                         false};
}

void CommandBuffer::Reset() {
    if (frame_command_count_ != 0U && auto_submit_) {
        runtime::Panic("graphics.frame_buffer.reset", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    logical_command_count_ = 0U;
    frame_command_count_ = 0U;
    surface_active_ = false;
    submitted_ = false;
    ResetBatch();
}

void CommandBuffer::ResetBatch() {
    ClearBytes(bytes_, sizeof(micropixel_graphics_command_header_t));
    micropixel_graphics_command_header_t header{};
    header.magic = MICROPIXEL_GRAPHICS_COMMAND_MAGIC;
    header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    header.total_size = sizeof(header);
    CopyBytes(bytes_, &header, sizeof(header));
    size_ = sizeof(header);
    batch_command_count_ = 0U;
}

uint8_t* CommandBuffer::AppendUnchecked(uint32_t bytes) {
    if (bytes == 0U || bytes > kCapacityBytes - size_ || batch_command_count_ >= max_commands_ ||
        frame_command_count_ >= max_frame_commands_) {
        runtime::Panic("graphics.command_buffer.capacity", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    uint8_t* record = bytes_ + size_;
    ClearBytes(record, bytes);
    size_ += bytes;
    ++batch_command_count_;
    ++frame_command_count_;
    return record;
}

void CommandBuffer::SubmitBatch() {
    if (batch_command_count_ == 0U) {
        return;
    }
    micropixel_graphics_command_header_t header{};
    CopyBytes(&header, bytes_, sizeof(header));
    header.total_size = size_;
    header.command_count = batch_command_count_;
    CopyBytes(bytes_, &header, sizeof(header));
    RequireOk(OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                          MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
              "graphics.submit.open");
    RequireOk(
        micropixel_service_submit(graphics_service.info.handle, MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS, bytes_, size_),
        "graphics.submit");
}

void CommandBuffer::ContinueSurfaceInNewBatch() {
    micropixel_graphics_end_surface_command_t end{};
    end.record.opcode = MICROPIXEL_GRAPHICS_OP_END_SURFACE;
    end.record.size = sizeof(end);
    CopyBytes(AppendUnchecked(sizeof(end)), &end, sizeof(end));
    SubmitBatch();
    ResetBatch();

    micropixel_graphics_begin_surface_command_t begin{};
    begin.record.opcode = MICROPIXEL_GRAPHICS_OP_BEGIN_SURFACE;
    begin.record.size = sizeof(begin);
    begin.x = surface_bounds_.x;
    begin.y = surface_bounds_.y;
    begin.width = surface_bounds_.width;
    begin.height = surface_bounds_.height;
    begin.translate_x = surface_translation_.x;
    begin.translate_y = surface_translation_.y;
    begin.flags = surface_translation_active_ ? MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION_ACTIVE : 0U;
    CopyBytes(AppendUnchecked(sizeof(begin)), &begin, sizeof(begin));
}

uint8_t* CommandBuffer::Append(uint32_t bytes) {
    if (submitted_ || bytes == 0U) {
        runtime::Panic("graphics.command_buffer.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t reserved_commands = surface_active_ ? 1U : 0U;
    const uint32_t reserved_bytes = surface_active_ ? sizeof(micropixel_graphics_end_surface_command_t) : 0U;
    if (bytes > kCapacityBytes - size_ - reserved_bytes ||
        batch_command_count_ + 1U + reserved_commands > max_commands_) {
        if (!auto_submit_ || batch_command_count_ == 0U) {
            runtime::Panic("graphics.command_buffer.capacity", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
        if (surface_active_) {
            ContinueSurfaceInNewBatch();
        } else {
            SubmitBatch();
            ResetBatch();
        }
    }
    uint8_t* record = AppendUnchecked(bytes);
    ++logical_command_count_;
    return record;
}

void CommandBuffer::Clear(Color color) {
    micropixel_graphics_clear_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_CLEAR;
    command.record.size = sizeof(command);
    command.rgb888 = color.rgb888_;
    CopyBytes(Append(sizeof(command)), &command, sizeof(command));
}

void CommandBuffer::FillRect(Rect rect, Color color) {
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0) {
        runtime::Panic("graphics.fill_rect.rect", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_graphics_fill_rect_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_FILL_RECT;
    command.record.size = sizeof(command);
    command.x = rect.x;
    command.y = rect.y;
    command.width = rect.width;
    command.height = rect.height;
    command.rgb888 = color.rgb888_;
    CopyBytes(Append(sizeof(command)), &command, sizeof(command));
}

void CommandBuffer::BlendRect(Rect rect, Color color, uint8_t opacity) {
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0) {
        runtime::Panic("graphics.blend_rect.rect", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_graphics_blend_rect_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_BLEND_RECT;
    command.record.size = sizeof(command);
    command.x = rect.x;
    command.y = rect.y;
    command.width = rect.width;
    command.height = rect.height;
    command.rgb888 = color.rgb888_;
    command.opacity = opacity;
    CopyBytes(Append(sizeof(command)), &command, sizeof(command));
}

void CommandBuffer::DrawText(int32_t x, int32_t y, const char* text, Color color, uint16_t font_size_px) {
    if (x < 0 || y < 0 || text == nullptr || font_size_px < 8U || font_size_px > 48U) {
        runtime::Panic("graphics.draw_text.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    uint32_t text_length = 0U;
    while (text_length <= MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES && text[text_length] != '\0') {
        ++text_length;
    }
    if (text_length == 0U || text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES) {
        runtime::Panic("graphics.draw_text.length", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    uint32_t raw_size = sizeof(micropixel_graphics_draw_text_command_t) + text_length;
    uint32_t record_size = (raw_size + 3U) & ~3U;
    uint8_t* record = Append(record_size);
    micropixel_graphics_draw_text_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_DRAW_TEXT;
    command.record.size = static_cast<uint16_t>(record_size);
    command.x = x;
    command.y = y;
    command.rgb888 = color.rgb888_;
    command.font_size_px = font_size_px;
    command.text_length = static_cast<uint16_t>(text_length);
    CopyBytes(record, &command, sizeof(command));
    CopyBytes(record + sizeof(command), text, text_length);
}

void CommandBuffer::DrawTextCentered(int32_t center_x, int32_t y, const char* text, Color color,
                                     uint16_t font_size_px) {
    if (center_x < 0 || y < 0 || text == nullptr || font_size_px < 8U || font_size_px > 48U) {
        runtime::Panic("graphics.draw_text_centered.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    uint32_t text_length = 0U;
    while (text_length <= MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES && text[text_length] != '\0') {
        ++text_length;
    }
    if (text_length == 0U || text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES) {
        runtime::Panic("graphics.draw_text_centered.length", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    uint32_t raw_size = sizeof(micropixel_graphics_draw_text_command_t) + text_length;
    uint32_t record_size = (raw_size + 3U) & ~3U;
    uint8_t* record = Append(record_size);
    micropixel_graphics_draw_text_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_DRAW_TEXT_CENTERED;
    command.record.size = static_cast<uint16_t>(record_size);
    command.x = center_x;
    command.y = y;
    command.rgb888 = color.rgb888_;
    command.font_size_px = font_size_px;
    command.text_length = static_cast<uint16_t>(text_length);
    CopyBytes(record, &command, sizeof(command));
    CopyBytes(record + sizeof(command), text, text_length);
}

void CommandBuffer::DrawBitmap(int32_t x, int32_t y, const Bitmap& bitmap) {
    DrawBitmapRegion(x, y, bitmap,
                     Rect{0, 0, static_cast<int32_t>(bitmap.width()), static_cast<int32_t>(bitmap.height())});
}

void CommandBuffer::DrawBitmapRegion(int32_t x, int32_t y, const Bitmap& bitmap, Rect source) {
    if (x < 0 || y < 0 || !bitmap.valid() || source.x < 0 || source.y < 0 || source.width <= 0 || source.height <= 0 ||
        static_cast<uint64_t>(source.x) + static_cast<uint32_t>(source.width) > bitmap.width() ||
        static_cast<uint64_t>(source.y) + static_cast<uint32_t>(source.height) > bitmap.height()) {
        runtime::Panic("graphics.draw_bitmap.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_graphics_draw_bitmap_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_DRAW_BITMAP;
    command.record.size = sizeof(command);
    command.bitmap = bitmap.handle_;
    command.x = x;
    command.y = y;
    command.source_x = source.x;
    command.source_y = source.y;
    command.width = source.width;
    command.height = source.height;
    CopyBytes(Append(sizeof(command)), &command, sizeof(command));
}

void CommandBuffer::BlendBitmap(int32_t x, int32_t y, const Bitmap& bitmap, uint8_t opacity) {
    BlendBitmapRegion(x, y, bitmap,
                      Rect{0, 0, static_cast<int32_t>(bitmap.width()), static_cast<int32_t>(bitmap.height())}, opacity);
}

void CommandBuffer::BlendBitmapRegion(int32_t x, int32_t y, const Bitmap& bitmap, Rect source, uint8_t opacity) {
    if (x < 0 || y < 0 || !bitmap.valid() || source.x < 0 || source.y < 0 || source.width <= 0 || source.height <= 0 ||
        static_cast<uint64_t>(source.x) + static_cast<uint32_t>(source.width) > bitmap.width() ||
        static_cast<uint64_t>(source.y) + static_cast<uint32_t>(source.height) > bitmap.height()) {
        runtime::Panic("graphics.blend_bitmap.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_graphics_blend_bitmap_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_BLEND_BITMAP;
    command.record.size = sizeof(command);
    command.bitmap = bitmap.handle_;
    command.x = x;
    command.y = y;
    command.source_x = source.x;
    command.source_y = source.y;
    command.width = source.width;
    command.height = source.height;
    command.opacity = opacity;
    CopyBytes(Append(sizeof(command)), &command, sizeof(command));
}

void CommandBuffer::BeginSurface(Rect bounds, Point translation, bool translation_active) {
    if (surface_active_ || bounds.x < 0 || bounds.y < 0 || bounds.width <= 0 || bounds.height <= 0 ||
        translation.x < -32 || translation.x > 32 || translation.y < -32 || translation.y > 32) {
        runtime::Panic("graphics.begin_surface.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_graphics_begin_surface_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_BEGIN_SURFACE;
    command.record.size = sizeof(command);
    command.x = bounds.x;
    command.y = bounds.y;
    command.width = bounds.width;
    command.height = bounds.height;
    command.translate_x = translation.x;
    command.translate_y = translation.y;
    command.flags = translation_active ? MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION_ACTIVE : 0U;
    CopyBytes(Append(sizeof(command)), &command, sizeof(command));
    surface_bounds_ = bounds;
    surface_translation_ = translation;
    surface_translation_active_ = translation_active;
    surface_active_ = true;
}

void CommandBuffer::EndSurface() {
    if (!surface_active_ || submitted_) {
        runtime::Panic("graphics.end_surface.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_graphics_end_surface_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_END_SURFACE;
    command.record.size = sizeof(command);
    CopyBytes(AppendUnchecked(sizeof(command)), &command, sizeof(command));
    ++logical_command_count_;
    surface_active_ = false;
}

void CommandBuffer::Submit() {
    if (submitted_ || surface_active_) {
        runtime::Panic("graphics.submit.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SubmitBatch();
    submitted_ = true;
}

GraphicsFrame::GraphicsFrame(GraphicsFrame&& other) noexcept : active_(other.active_) {
    other.active_ = false;
}

GraphicsFrame::~GraphicsFrame() {
    if (active_) {
        Commit();
    }
}

CommandBuffer GraphicsFrame::CreateCommandBuffer(const GraphicsInfo& graphics_info) const {
    if (!active_ || !graphics_info.supports_multi_submit_frames()) {
        runtime::Panic("graphics.frame.buffer", MICROPIXEL_STATUS_UNSUPPORTED);
    }
    return CommandBuffer{CommandBuffer::CapabilityToken{}, graphics_info.max_commands(),
                         graphics_info.max_frame_commands(), true};
}

void GraphicsFrame::Commit() {
    if (!active_) {
        return;
    }
    active_ = false;
    RequireOk(OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                          MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
              "graphics.frame.commit.open");
    RequireOk(CallVoid(graphics_service, MICROPIXEL_GRAPHICS_METHOD_FRAME_COMMIT, nullptr, 0U),
              "graphics.frame.commit");
}

GraphicsFrame Graphics::BeginFrame() const {
    const GraphicsInfo graphics_info = info();
    if (!graphics_info.supports_multi_submit_frames()) {
        runtime::Panic("graphics.frame.unsupported", MICROPIXEL_STATUS_UNSUPPORTED);
    }
    RequireOk(CallVoid(graphics_service, MICROPIXEL_GRAPHICS_METHOD_FRAME_BEGIN, nullptr, 0U),
              "graphics.frame.begin");
    return GraphicsFrame{GraphicsFrame::CapabilityToken{}};
}

InputInfo Input::info() const {
    micropixel_input_info_t raw{};
    RequireOk(OpenService(input_service, MICROPIXEL_SERVICE_INPUT, 1U, 0U), "input.open");
    uint32_t response_size = 0U;
    RequireOk(
        CallService(input_service, MICROPIXEL_INPUT_METHOD_GET_INFO, nullptr, 0U, &raw, sizeof(raw), response_size),
        "input.info");
    if (response_size < sizeof(raw) || raw.size < sizeof(raw) || raw.interface_major != 1U ||
        raw.max_touch_points == 0U || raw.max_touch_points > MICROPIXEL_MAX_TOUCH_POINTS) {
        runtime::Panic("input.info.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
    }
    return InputInfo{raw.logical_width, raw.logical_height, raw.max_touch_points};
}

Bitmap::Bitmap(Bitmap&& other) noexcept : handle_(other.handle_), width_(other.width_), height_(other.height_) {
    other.handle_ = 0U;
}

Bitmap& Bitmap::operator=(Bitmap&& other) noexcept {
    if (this != &other) {
        Release();
        handle_ = other.handle_;
        width_ = other.width_;
        height_ = other.height_;
        other.handle_ = 0U;
    }
    return *this;
}

Bitmap::~Bitmap() { Release(); }

void Bitmap::Release() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        if (OpenService(resource_service, MICROPIXEL_SERVICE_RESOURCE, 1U, 0U) == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_BITMAP_RELEASE, &request, sizeof(request));
        }
        handle_ = 0U;
        width_ = 0U;
        height_ = 0U;
    }
}

void OffscreenSurface::Update(Rect dirty, const uint8_t* pixels, uint32_t stride) {
    const uint32_t bytes_per_pixel =
        pixel_format_ == SurfacePixelFormat::kRgb888 ? 3U : (pixel_format_ == SurfacePixelFormat::kArgb8888 ? 4U : 0U);
    if (!valid() || dirty.x < 0 || dirty.y < 0 || dirty.width <= 0 || dirty.height <= 0 || pixels == nullptr ||
        bytes_per_pixel == 0U || static_cast<int64_t>(dirty.x) + dirty.width > static_cast<int64_t>(width()) ||
        static_cast<int64_t>(dirty.y) + dirty.height > static_cast<int64_t>(height())) {
        runtime::Panic("surface.update.arguments", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t row_bytes = static_cast<uint32_t>(dirty.width) * bytes_per_pixel;
    if (stride < row_bytes) {
        runtime::Panic("surface.update.stride", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    constexpr uint32_t kHeaderBytes = sizeof(micropixel_offscreen_surface_update_request_t);
    static_assert(kHeaderBytes < MICROPIXEL_OFFSCREEN_SURFACE_MAX_UPDATE_BYTES,
                  "offscreen surface update header exceeds ABI request");
    const uint32_t rows_per_request = (MICROPIXEL_OFFSCREEN_SURFACE_MAX_UPDATE_BYTES - kHeaderBytes) / row_bytes;
    if (rows_per_request == 0U) {
        runtime::Panic("surface.update.row_too_wide", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    RequireOk(OpenOffscreenSurfaceService(), "surface.update.open");
    alignas(4) uint8_t request[MICROPIXEL_OFFSCREEN_SURFACE_MAX_UPDATE_BYTES]{};
    uint32_t row = 0U;
    while (row < static_cast<uint32_t>(dirty.height)) {
        uint32_t row_count = static_cast<uint32_t>(dirty.height) - row;
        if (row_count > rows_per_request) {
            row_count = rows_per_request;
        }
        const uint32_t pixel_bytes = row_count * row_bytes;
        const uint32_t request_size = kHeaderBytes + pixel_bytes;
        micropixel_offscreen_surface_update_request_t header{};
        header.size = static_cast<uint16_t>(request_size);
        header.bitmap = bitmap_.handle_;
        header.x = static_cast<uint32_t>(dirty.x);
        header.y = static_cast<uint32_t>(dirty.y) + row;
        header.width = static_cast<uint32_t>(dirty.width);
        header.height = row_count;
        header.stride = row_bytes;
        CopyBytes(request, &header, sizeof(header));
        for (uint32_t source_row = 0U; source_row < row_count; ++source_row) {
            CopyBytes(request + kHeaderBytes + source_row * row_bytes, pixels + (row + source_row) * stride, row_bytes);
        }
        RequireOk(
            CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_SURFACE_UPDATE, request, request_size),
            "surface.update");
        row += row_count;
    }
}

OffscreenUpdateFrame::OffscreenUpdateFrame(OffscreenUpdateFrame&& other) noexcept : active_(other.active_) {
    other.active_ = false;
}

OffscreenUpdateFrame::~OffscreenUpdateFrame() {
    if (active_) {
        Commit();
    }
}

void OffscreenUpdateFrame::Commit() {
    if (!active_) {
        return;
    }
    active_ = false;
    RequireOk(OpenOffscreenSurfaceService(), "surface.frame.commit.open");
    RequireOk(CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_FRAME_COMMIT, nullptr, 0U),
              "surface.frame.commit");
}

LoadRequest::LoadRequest(LoadRequest&& other) noexcept : handle_(other.handle_) { other.handle_ = 0U; }

LoadRequest& LoadRequest::operator=(LoadRequest&& other) noexcept {
    if (this != &other) {
        Cancel();
        handle_ = other.handle_;
        other.handle_ = 0U;
    }
    return *this;
}

LoadRequest::~LoadRequest() { Cancel(); }

void LoadRequest::Cancel() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        if (OpenService(resource_service, MICROPIXEL_SERVICE_RESOURCE, 1U, 0U) == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_CANCEL, &request, sizeof(request));
        }
        handle_ = 0U;
    }
}

LoadRequest Resources::Load(ResourceRef resource) const {
    RequireOk(OpenService(resource_service, MICROPIXEL_SERVICE_RESOURCE, 1U, 0U), "resources.open");
    micropixel_resource_load_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, resource.asset().value()};
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_LOAD, &request, sizeof(request), &response,
                          sizeof(response), response_size),
              "resources.load");
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("resources.load.handle", MICROPIXEL_STATUS_INTERNAL);
    }
    return LoadRequest{response.handle};
}

OffscreenSurface Resources::CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                   SurfacePixelFormat pixel_format) const {
    RequireOk(OpenOffscreenSurfaceService(), "surface.create.open");
    micropixel_offscreen_surface_create_request_t request{};
    request.size = sizeof(request);
    request.width = width;
    request.height = height;
    request.pixel_format = static_cast<uint32_t>(pixel_format);
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_SURFACE_CREATE, &request,
                          sizeof(request), &response, sizeof(response), response_size),
              "surface.create");
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("surface.create.handle", MICROPIXEL_STATUS_INTERNAL);
    }
    return OffscreenSurface{Bitmap{response.handle, width, height}, pixel_format};
}

OffscreenUpdateFrame Resources::BeginOffscreenUpdateFrame() const {
    RequireOk(OpenOffscreenSurfaceService(), "surface.frame.begin.open");
    RequireOk(CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_OFFSCREEN_FRAME_BEGIN, nullptr, 0U),
              "surface.frame.begin");
    return OffscreenUpdateFrame{OffscreenUpdateFrame::CapabilityToken{}};
}

Bitmap ResourceReadyEvent::TakeBitmap() {
    if (!succeeded() || bitmap_ == 0U) {
        runtime::Panic("resource_ready.take_bitmap", status_);
    }
    micropixel_bitmap_info_t info{};
    RequireOk(OpenService(resource_service, MICROPIXEL_SERVICE_RESOURCE, 1U, 0U), "resource_ready.open");
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, bitmap_};
    uint32_t response_size = 0U;
    RequireOk(CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_BITMAP_GET_INFO, &request, sizeof(request),
                          &info, sizeof(info), response_size),
              "resource_ready.bitmap_info");
    if (response_size < sizeof(info) || info.size < sizeof(info) || info.interface_major != 1U ||
        (info.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB888 &&
         info.pixel_format != MICROPIXEL_PIXEL_FORMAT_ARGB8888) ||
        info.width == 0U || info.height == 0U) {
        runtime::Panic("resource_ready.bitmap_info.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
    }
    Bitmap bitmap{bitmap_, info.width, info.height};
    bitmap_ = 0U;
    return bitmap;
}

ResourceReadyEvent* Event::ResourceFrom(LoadRequest& request) {
    if (type_ != EventType::kResourceReady || request.handle_ == 0U || resource_.request_ != request.handle_) {
        return nullptr;
    }
    request.MarkComplete();
    return &resource_;
}

Event Application::WaitEvent() const {
    micropixel_event_t raw{};
    RequireOk(micropixel_event_wait(&raw, sizeof(raw), UINT64_MAX), "application.WaitEvent");
    if (raw.size != sizeof(raw)) {
        runtime::Panic("application.wait_event.size", MICROPIXEL_STATUS_INTERNAL);
    }

    TimePoint timestamp{raw.timestamp_us};
    if (raw.service_id == MICROPIXEL_SERVICE_TIMER && raw.event_id == MICROPIXEL_TIMER_EVENT_EXPIRED) {
        micropixel_timer_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        return Event{TimerEvent{timestamp, Duration::Microseconds(payload.elapsed_us), raw.source}};
    }

    if (raw.service_id == MICROPIXEL_SERVICE_INPUT && raw.event_id == MICROPIXEL_INPUT_EVENT_TOUCH) {
        micropixel_touch_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        TouchPhase phase = TouchPhase::kCancel;
        switch (payload.phase) {
            case MICROPIXEL_TOUCH_DOWN:
                phase = TouchPhase::kDown;
                break;
            case MICROPIXEL_TOUCH_MOVE:
                phase = TouchPhase::kMove;
                break;
            case MICROPIXEL_TOUCH_UP:
                phase = TouchPhase::kUp;
                break;
            case MICROPIXEL_TOUCH_CANCEL:
                phase = TouchPhase::kCancel;
                break;
            default:
                runtime::Panic("application.wait_event.touch_phase", MICROPIXEL_STATUS_INTERNAL);
        }
        return Event{TouchEvent{timestamp, phase, raw.source, static_cast<uint16_t>(payload.x),
                                static_cast<uint16_t>(payload.y), payload.pressure}};
    }

    if (raw.service_id == MICROPIXEL_SERVICE_RESOURCE && raw.event_id == MICROPIXEL_RESOURCE_EVENT_READY) {
        micropixel_resource_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        return Event{ResourceReadyEvent{raw.source, payload.bitmap, raw.status}};
    }

    return Event{timestamp};
}

}  // namespace micropixel
