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
        case MICROPIXEL_STATUS_CLOSED:
            return Error{ErrorCode::kInvalidState};
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
        case MICROPIXEL_STATUS_CANCELLED:
            return Error{ErrorCode::kCancelled};
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
ServiceCache system_service;
ServiceCache graphics_service;
ServiceCache input_service;
ServiceCache audio_service;
micropixel_graphics_info_t cached_graphics_info{};
bool graphics_info_loaded{};
micropixel_input_info_t cached_input_info{};
bool input_info_loaded{};

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

const micropixel_input_info_t& LoadInputInfo() {
    if (!input_info_loaded) {
        RequireOk(OpenService(input_service, MICROPIXEL_SERVICE_INPUT, MICROPIXEL_INPUT_INTERFACE_MAJOR, 0U),
                  "input.open");
        uint32_t response_size = 0U;
        RequireOk(CallService(input_service, MICROPIXEL_INPUT_METHOD_GET_INFO, nullptr, 0U, &cached_input_info,
                              sizeof(cached_input_info), response_size),
                  "input.info");
        if (response_size < sizeof(cached_input_info) || cached_input_info.size < sizeof(cached_input_info) ||
            cached_input_info.interface_major != MICROPIXEL_INPUT_INTERFACE_MAJOR ||
            cached_input_info.logical_width == 0U || cached_input_info.logical_height == 0U ||
            cached_input_info.max_touch_points == 0U ||
            cached_input_info.max_touch_points > MICROPIXEL_MAX_TOUCH_POINTS) {
            micropixel::runtime::Panic("input.info.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        if (graphics_info_loaded && (cached_input_info.logical_width != cached_graphics_info.width ||
                                     cached_input_info.logical_height != cached_graphics_info.height)) {
            micropixel::runtime::Panic("input.info.coordinate_space", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        input_info_loaded = true;
    }
    return cached_input_info;
}

void WriteLog(uint32_t level, const char* message, const char* operation) {
    if (message == nullptr) {
        micropixel::runtime::Panic(operation, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t length = BoundedLength(message);
    if (length == MICROPIXEL_ABI_MAX_LOG_BYTES) {
        micropixel::runtime::Panic(operation, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    RequireOk(micropixel_log_write(level, reinterpret_cast<const uint8_t*>(message), length), operation);
}

int32_t OpenResourceService() {
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
    while (length <= micropixel::KVStore::kMaximumKeyBytes && key[length] != '\0') {
        ++length;
    }
    if (length == 0U || length > micropixel::KVStore::kMaximumKeyBytes) {
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

static_assert(Frame::kCapacityBytes == MICROPIXEL_GRAPHICS_MAX_COMMAND_BYTES,
              "SDK/ABI graphics command byte capacity drifted");
static_assert(Frame::kCapacityCommands == MICROPIXEL_GRAPHICS_MAX_COMMANDS,
              "SDK/ABI graphics command count capacity drifted");
static_assert(KVStore::kMaximumKeyBytes == MICROPIXEL_STORAGE_MAX_KEY_BYTES, "SDK/ABI storage key limit drifted");
static_assert(KVStore::kMaximumValueBytes == MICROPIXEL_STORAGE_MAX_VALUE_BYTES, "SDK/ABI storage value limit drifted");
static_assert(Log::kMaximumMessageBytes + 1U == MICROPIXEL_ABI_MAX_LOG_BYTES, "SDK/ABI log message limit drifted");

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

Result<uint32_t> KVStore::GetBytesSize(const char* key) const {
    uint32_t size = 0U;
    const int32_t status = GetStorageValue(key, nullptr, 0U, size);
    if (status == MICROPIXEL_STATUS_OK || status == MICROPIXEL_STATUS_BUFFER_TOO_SMALL) {
        return size;
    }
    return unexpected(ErrorFromStatus(status));
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

void Log::Debug(const char* message) const { WriteLog(MICROPIXEL_LOG_DEBUG, message, "log.debug"); }

void Log::Info(const char* message) const { WriteLog(MICROPIXEL_LOG_INFO, message, "log.info"); }

void Log::Warning(const char* message) const { WriteLog(MICROPIXEL_LOG_WARNING, message, "log.warning"); }

void Log::Error(const char* message) const { WriteLog(MICROPIXEL_LOG_ERROR, message, "log.error"); }

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

uint32_t Random::Below(uint32_t upper_bound) const {
    if (upper_bound == 0U) {
        runtime::Panic("random.below.upper_bound", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    // Reject the short prefix that would make `% upper_bound` favor some
    // outcomes when 2^32 is not evenly divisible by upper_bound.
    const uint32_t rejection_threshold = static_cast<uint32_t>(0U - upper_bound) % upper_bound;
    uint32_t value = 0U;
    do {
        value = U32();
    } while (value < rejection_threshold);
    return value % upper_bound;
}

Locale Localization::CurrentLocale() const {
    Locale locale{};
    RequireOk(OpenService(system_service, MICROPIXEL_SERVICE_SYSTEM, MICROPIXEL_SYSTEM_INTERFACE_MAJOR,
                          MICROPIXEL_SYSTEM_INTERFACE_MINOR),
              "system.open");
    micropixel_system_locale_response_t wire{};
    uint32_t response_size = 0U;
    RequireOk(CallService(system_service, MICROPIXEL_SYSTEM_METHOD_GET_LOCALE, nullptr, 0U, &wire, sizeof(wire),
                          response_size),
              "system.locale");
    if (response_size < sizeof(wire) || wire.size < sizeof(wire) || wire.tag_length == 0U ||
        wire.tag_length > MICROPIXEL_LOCALE_TAG_MAX_BYTES || wire.tag[wire.tag_length] != '\0') {
        runtime::Panic("system.locale.invalid", MICROPIXEL_STATUS_INTERNAL);
    }
    CopyBytes(locale.tag_, wire.tag, wire.tag_length + 1U);
    return locale;
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
        raw.max_clips,
        raw.max_playbacks,
        (raw.capabilities & MICROPIXEL_AUDIO_CAPABILITY_OGG_OPUS) != 0U,
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

AudioClip::AudioClip(AudioClip&& other) noexcept : handle_(other.handle_) { other.handle_ = 0U; }

AudioClip& AudioClip::operator=(AudioClip&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        other.handle_ = 0U;
    }
    return *this;
}

AudioClip::~AudioClip() { Reset(); }

void AudioClip::Reset() {
    if (handle_ == 0U) {
        return;
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    if (OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                    MICROPIXEL_AUDIO_INTERFACE_MINOR) == MICROPIXEL_STATUS_OK) {
        (void)CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_CLIP_RELEASE, &request, sizeof(request));
    }
    handle_ = 0U;
}

Playback::Playback(Playback&& other) noexcept : handle_(other.handle_) { other.handle_ = 0U; }

Playback& Playback::operator=(Playback&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        other.handle_ = 0U;
    }
    return *this;
}

Playback::~Playback() { Reset(); }

Result<void> Playback::Pause() {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_PAUSE, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> Playback::Resume() {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_RESUME, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> Playback::SetVolume(uint16_t volume_per_mille) {
    if (handle_ == 0U || volume_per_mille > 1000U) {
        return unexpected(Error{handle_ == 0U ? ErrorCode::kInvalidState : ErrorCode::kInvalidArgument});
    }
    micropixel_audio_playback_volume_request_t request{};
    request.size = sizeof(request);
    request.playback = handle_;
    request.volume_per_mille = volume_per_mille;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_SET_VOLUME, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<PlaybackState> Playback::state() const {
    if (handle_ == 0U) {
        return unexpected(Error{ErrorCode::kInvalidState});
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    micropixel_audio_playback_state_response_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_GET_STATE, &request, sizeof(request),
                             &response, sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.playback != handle_ ||
        response.state < MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING ||
        response.state > MICROPIXEL_AUDIO_PLAYBACK_STATE_FAILED) {
        runtime::Panic("audio.playback.state", MICROPIXEL_STATUS_INTERNAL);
    }
    return static_cast<PlaybackState>(response.state - MICROPIXEL_AUDIO_PLAYBACK_STATE_PLAYING);
}

Result<void> Playback::Stop() {
    if (handle_ == 0U) {
        return {};
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_STOP, &request, sizeof(request));
    }
    if (status == MICROPIXEL_STATUS_OK || status == MICROPIXEL_STATUS_NOT_FOUND) {
        handle_ = 0U;
        return {};
    }
    return unexpected(ErrorFromStatus(status));
}

void Playback::Reset() {
    if (handle_ != 0U) {
        (void)Stop();
        handle_ = 0U;
    }
}

Result<AudioClip> Audio::Load(AssetId asset) const {
    micropixel_audio_clip_load_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, asset.value()};
    micropixel_audio_clip_info_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_CLIP_LOAD, &request, sizeof(request), &response,
                             sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_AUDIO_INTERFACE_MAJOR || response.clip == 0U ||
        response.reserved0 != 0U || response.format != MICROPIXEL_AUDIO_FORMAT_OGG_OPUS) {
        runtime::Panic("audio.clip.load", MICROPIXEL_STATUS_INTERNAL);
    }
    return AudioClip{response.clip};
}

Result<Playback> Audio::Play(const AudioClip& clip, PlaybackOptions options) const {
    if (!clip.valid() || options.volume_per_mille > 1000U) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_audio_playback_start_request_t request{};
    request.size = sizeof(request);
    request.flags = options.loop ? MICROPIXEL_AUDIO_PLAYBACK_LOOP : 0U;
    request.clip = clip.handle_;
    request.volume_per_mille = options.volume_per_mille;
    micropixel_handle_response_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(audio_service, MICROPIXEL_SERVICE_AUDIO, MICROPIXEL_AUDIO_INTERFACE_MAJOR,
                                 MICROPIXEL_AUDIO_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(audio_service, MICROPIXEL_AUDIO_METHOD_PLAYBACK_START, &request, sizeof(request),
                             &response, sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.handle == 0U) {
        runtime::Panic("audio.playback.start", MICROPIXEL_STATUS_INTERNAL);
    }
    return Playback{response.handle};
}

Result<Playback> Audio::Play(AssetId asset, PlaybackOptions options) const {
    auto clip = Load(asset);
    if (!clip) {
        return unexpected(clip.error());
    }
    return Play(*clip, options);
}

Timer::~Timer() { Reset(); }

void Timer::Cancel() {
    if (handle_ == 0U) {
        return;
    }
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    RequireOk(OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U), "timer.cancel.open");
    RequireOk(CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_RELEASE, &request, sizeof(request)), "timer.cancel");
    handle_ = 0U;
}

void Timer::Reset() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        int32_t status = OpenService(timer_service, MICROPIXEL_SERVICE_TIMER, 1U, 0U);
        if (status == MICROPIXEL_STATUS_OK) {
            status = CallVoid(timer_service, MICROPIXEL_TIMER_METHOD_RELEASE, &request, sizeof(request));
        }
        handle_ = 0U;
        (void)status;
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

RendererInfo Renderer::info() const {
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
            cached_graphics_info.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 || cached_graphics_info.width == 0U ||
            cached_graphics_info.height == 0U || cached_graphics_info.max_command_bytes < Frame::kCapacityBytes ||
            cached_graphics_info.max_commands == 0U || cached_graphics_info.max_draw_operations == 0U ||
            cached_graphics_info.max_frame_commands < cached_graphics_info.max_draw_operations) {
            runtime::Panic("graphics.info.incompatible", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        if (input_info_loaded && (cached_graphics_info.width != cached_input_info.logical_width ||
                                  cached_graphics_info.height != cached_input_info.logical_height)) {
            runtime::Panic("graphics.info.coordinate_space", MICROPIXEL_STATUS_UNSUPPORTED);
        }
        graphics_info_loaded = true;
    }
    const micropixel_graphics_info_t& raw = cached_graphics_info;
    return RendererInfo{
        raw.width, raw.height, raw.capabilities, raw.max_commands, raw.max_draw_operations, raw.max_frame_commands};
}

Frame::Frame(CapabilityToken, const RendererInfo& info)
    : max_batch_commands_(info.max_batch_commands() < kCapacityCommands ? info.max_batch_commands()
                                                                        : kCapacityCommands),
      max_draw_operations_(info.max_draw_operations()),
      max_frame_commands_(info.max_frame_commands()),
      display_bounds_{0, 0, static_cast<int32_t>(info.width()), static_cast<int32_t>(info.height())},
      retained_translation_available_(info.retained_translation_available()),
      multi_submit_available_(info.multi_submit_available()) {
    states_[0].clip = display_bounds_;
    states_[0].clip_limit = display_bounds_;
    ResetBatch();
}

Frame::Frame(Frame&& other) noexcept
    : size_(other.size_),
      batch_command_count_(other.batch_command_count_),
      draw_operation_count_(other.draw_operation_count_),
      frame_command_count_(other.frame_command_count_),
      max_batch_commands_(other.max_batch_commands_),
      max_draw_operations_(other.max_draw_operations_),
      max_frame_commands_(other.max_frame_commands_),
      display_bounds_(other.display_bounds_),
      retained_clip_(other.retained_clip_),
      retained_translation_(other.retained_translation_),
      state_depth_(other.state_depth_),
      encoded_state_depth_(other.encoded_state_depth_),
      retained_scope_count_(other.retained_scope_count_),
      failure_status_(other.failure_status_),
      retained_translation_available_(other.retained_translation_available_),
      multi_submit_available_(other.multi_submit_available_),
      retained_scope_selected_(other.retained_scope_selected_),
      state_encoded_(other.state_encoded_),
      host_frame_active_(other.host_frame_active_),
      presented_(other.presented_) {
    CopyBytes(bytes_, other.bytes_, sizeof(bytes_));
    CopyBytes(states_, other.states_, sizeof(states_));
    other.host_frame_active_ = false;
    other.presented_ = true;
}

Frame::~Frame() { Cancel(); }

void Frame::ResetBatch() {
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

uint8_t* Frame::AppendUnchecked(uint32_t bytes) {
    if (failure_status_ != MICROPIXEL_STATUS_OK) {
        return DiscardRecord(bytes);
    }
    if (bytes == 0U || bytes > kCapacityBytes - size_ || batch_command_count_ >= max_batch_commands_ ||
        frame_command_count_ >= max_frame_commands_) {
        Fail(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        return DiscardRecord(bytes);
    }
    uint8_t* record = bytes_ + size_;
    ClearBytes(record, bytes);
    size_ += bytes;
    ++batch_command_count_;
    ++frame_command_count_;
    return record;
}

uint8_t* Frame::DiscardRecord(uint32_t bytes) {
    if (bytes == 0U || bytes > sizeof(discard_record_)) {
        runtime::Panic("graphics.frame.record_size", MICROPIXEL_STATUS_INTERNAL);
    }
    ClearBytes(discard_record_, bytes);
    return discard_record_;
}

void Frame::Fail(int32_t status) {
    if (failure_status_ == MICROPIXEL_STATUS_OK) {
        failure_status_ = status == MICROPIXEL_STATUS_OK ? MICROPIXEL_STATUS_INTERNAL : status;
    }
}

bool Frame::StartHostFrame() {
    if (host_frame_active_) {
        return true;
    }
    if (!multi_submit_available_) {
        Fail(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        return false;
    }
    int32_t status = OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                                 MICROPIXEL_GRAPHICS_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(graphics_service, MICROPIXEL_GRAPHICS_METHOD_FRAME_BEGIN, nullptr, 0U);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        Fail(status);
        return false;
    }
    host_frame_active_ = true;
    return true;
}

bool Frame::SubmitBatch() {
    if (batch_command_count_ == 0U) {
        return true;
    }
    micropixel_graphics_command_header_t header{};
    CopyBytes(&header, bytes_, sizeof(header));
    header.total_size = size_;
    header.command_count = batch_command_count_;
    CopyBytes(bytes_, &header, sizeof(header));
    int32_t status = OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                                 MICROPIXEL_GRAPHICS_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = micropixel_service_submit(graphics_service.info.handle, MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS, bytes_,
                                           size_);
    }
    if (status != MICROPIXEL_STATUS_OK) {
        Fail(status);
        return false;
    }
    return true;
}

void Frame::CloseEncodedState() {
    micropixel_graphics_pop_state_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_POP_STATE;
    command.record.size = sizeof(command);
    CopyBytes(AppendUnchecked(sizeof(command)), &command, sizeof(command));
    state_encoded_ = false;
    encoded_state_depth_ = 0U;
}

void Frame::ContinueStateInNewBatch() {
    const uint32_t continued_depth = encoded_state_depth_;
    CloseEncodedState();
    if (!SubmitBatch()) {
        return;
    }
    ResetBatch();

    micropixel_graphics_push_state_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_PUSH_STATE;
    command.record.size = sizeof(command);
    command.clip_x = retained_clip_.x;
    command.clip_y = retained_clip_.y;
    command.width = retained_clip_.width;
    command.height = retained_clip_.height;
    command.translate_x = retained_translation_.x;
    command.translate_y = retained_translation_.y;
    command.flags = (retained_translation_.x != 0 || retained_translation_.y != 0)
                        ? MICROPIXEL_GRAPHICS_STATE_RETAINED_TRANSLATION_ACTIVE
                        : 0U;
    CopyBytes(AppendUnchecked(sizeof(command)), &command, sizeof(command));
    if (failure_status_ == MICROPIXEL_STATUS_OK) {
        state_encoded_ = true;
        encoded_state_depth_ = continued_depth;
    }
}

uint8_t* Frame::Append(uint32_t bytes) {
    if (presented_ || bytes == 0U) {
        runtime::Panic("graphics.frame.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (failure_status_ != MICROPIXEL_STATUS_OK) {
        return DiscardRecord(bytes);
    }
    if (draw_operation_count_ >= max_draw_operations_) {
        Fail(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        return DiscardRecord(bytes);
    }
    const uint32_t reserved_commands = state_encoded_ ? 1U : 0U;
    const uint32_t reserved_bytes = state_encoded_ ? sizeof(micropixel_graphics_pop_state_command_t) : 0U;
    if (bytes > kCapacityBytes - size_ - reserved_bytes ||
        batch_command_count_ + 1U + reserved_commands > max_batch_commands_) {
        if (batch_command_count_ == 0U) {
            Fail(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
            return DiscardRecord(bytes);
        }
        if (!StartHostFrame()) {
            return DiscardRecord(bytes);
        }
        if (state_encoded_) {
            ContinueStateInNewBatch();
        } else {
            if (!SubmitBatch()) {
                return DiscardRecord(bytes);
            }
            ResetBatch();
        }
    }
    uint8_t* record = AppendUnchecked(bytes);
    if (failure_status_ == MICROPIXEL_STATUS_OK) {
        ++draw_operation_count_;
    }
    return record;
}

void Frame::Clear(Color color) {
    if (state_depth_ != 0U) {
        runtime::Panic("graphics.clear.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_graphics_clear_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_CLEAR;
    command.record.size = sizeof(command);
    command.rgb888 = color.rgb888_;
    CopyBytes(Append(sizeof(command)), &command, sizeof(command));
}

void Frame::EnsureStateEncoded() {
    constexpr uint32_t kMaxRetainedScopesPerFrame = 4U;
    if (state_depth_ == 0U || state_encoded_ || failure_status_ != MICROPIXEL_STATUS_OK ||
        retained_scope_count_ >= kMaxRetainedScopesPerFrame) {
        return;
    }
    const State& state = states_[state_depth_];
    const Rect state_clip = StateClip();
    if (state_clip.empty()) {
        return;
    }
    const Rect wire_clip = state_clip.translated(-state.translation.x, -state.translation.y);
    const int64_t translated_left = static_cast<int64_t>(wire_clip.x) + state.translation.x;
    const int64_t translated_top = static_cast<int64_t>(wire_clip.y) + state.translation.y;
    const int64_t translated_right = translated_left + wire_clip.width;
    const int64_t translated_bottom = translated_top + wire_clip.height;
    bool use_retained_translation =
        retained_translation_available_ && state.translation.x >= -32 && state.translation.x <= 32 &&
        state.translation.y >= -32 && state.translation.y <= 32 && translated_left >= display_bounds_.x &&
        translated_top >= display_bounds_.y &&
        translated_right <= static_cast<int64_t>(display_bounds_.x) + display_bounds_.width &&
        translated_bottom <= static_cast<int64_t>(display_bounds_.y) + display_bounds_.height;
    if (use_retained_translation && retained_scope_selected_ &&
        (wire_clip != retained_clip_ || state.translation != retained_translation_)) {
        use_retained_translation = false;
    }
    if (!use_retained_translation) {
        return;
    }
    if (!retained_scope_selected_) {
        retained_scope_selected_ = true;
        retained_clip_ = wire_clip;
        retained_translation_ = state.translation;
    }

    micropixel_graphics_push_state_command_t command{};
    command.record.opcode = MICROPIXEL_GRAPHICS_OP_PUSH_STATE;
    command.record.size = sizeof(command);
    command.clip_x = wire_clip.x;
    command.clip_y = wire_clip.y;
    command.width = wire_clip.width;
    command.height = wire_clip.height;
    command.translate_x = state.translation.x;
    command.translate_y = state.translation.y;
    command.flags = (state.translation.x != 0 || state.translation.y != 0)
                        ? MICROPIXEL_GRAPHICS_STATE_RETAINED_TRANSLATION_ACTIVE
                        : 0U;
    const uint32_t pop_size = sizeof(micropixel_graphics_pop_state_command_t);
    if (sizeof(command) > kCapacityBytes - size_ - pop_size || batch_command_count_ + 2U > max_batch_commands_) {
        if (batch_command_count_ == 0U) {
            Fail(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
            return;
        }
        if (!StartHostFrame() || !SubmitBatch()) {
            return;
        }
        ResetBatch();
    }
    CopyBytes(AppendUnchecked(sizeof(command)), &command, sizeof(command));
    if (failure_status_ == MICROPIXEL_STATUS_OK) {
        state_encoded_ = true;
        encoded_state_depth_ = state_depth_;
        ++retained_scope_count_;
    }
}

Rect Frame::StateClip() const {
    if (state_depth_ == 0U) {
        return display_bounds_;
    }
    const State& state = states_[state_depth_];
    return state.clip.translated(state.translation.x, state.translation.y)
        .intersection(state.clip_limit)
        .intersection(display_bounds_);
}

Rect Frame::EffectiveClip() const {
    const Rect clip = StateClip();
    return state_encoded_
               ? clip.translated(-retained_translation_.x, -retained_translation_.y).intersection(retained_clip_)
               : clip;
}

Point Frame::EffectiveTranslation() const {
    if (state_depth_ == 0U) {
        return {};
    }
    const Point translation = states_[state_depth_].translation;
    return state_encoded_ ? Point{translation.x - retained_translation_.x, translation.y - retained_translation_.y}
                          : translation;
}

void Frame::FillRect(Rect rect, Color color, uint8_t opacity) {
    if (rect.empty()) {
        runtime::Panic("graphics.fill_rect.rect", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (state_depth_ != 0U) {
        states_[state_depth_].draw_started = true;
    }
    EnsureStateEncoded();
    const Point translation = EffectiveTranslation();
    rect = rect.translated(translation.x, translation.y).intersection(EffectiveClip());
    if (rect.empty()) {
        return;
    }
    if (opacity == 255U) {
        micropixel_graphics_fill_rect_command_t command{};
        command.record.opcode = MICROPIXEL_GRAPHICS_OP_FILL_RECT;
        command.record.size = sizeof(command);
        command.x = rect.x;
        command.y = rect.y;
        command.width = rect.width;
        command.height = rect.height;
        command.rgb888 = color.rgb888_;
        CopyBytes(Append(sizeof(command)), &command, sizeof(command));
    } else {
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
}

void Frame::DrawText(Point position, const char* text, Color color, SystemFont font) {
    const uint16_t font_handle = static_cast<uint16_t>(font);
    if (font_handle < MICROPIXEL_SYSTEM_FONT_SMALL || font_handle > MICROPIXEL_SYSTEM_FONT_TITLE) {
        runtime::Panic("graphics.draw_text.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    DrawTextWithHandle(position, text, color, font_handle);
}

void Frame::DrawText(Point position, const char* text, Color color, const Font& font) {
    if (!font.valid()) {
        runtime::Panic("graphics.draw_text.font", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    DrawTextWithHandle(position, text, color, font.handle_);
}

void Frame::DrawTextWithHandle(Point position, const char* text, Color color, uint16_t font_handle) {
    if (text == nullptr || font_handle == 0U) {
        runtime::Panic("graphics.draw_text.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (state_depth_ != 0U) {
        states_[state_depth_].draw_started = true;
    }
    EnsureStateEncoded();
    const Point translation = EffectiveTranslation();
    position.x += translation.x;
    position.y += translation.y;
    const Rect text_clip = EffectiveClip();
    if (!text_clip.contains(position)) {
        return;
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
    command.x = position.x;
    command.y = position.y;
    command.rgb888 = color.rgb888_;
    command.font_handle = font_handle;
    command.text_length = static_cast<uint16_t>(text_length);
    CopyBytes(record, &command, sizeof(command));
    CopyBytes(record + sizeof(command), text, text_length);
}

void Frame::DrawTextCentered(int32_t center_x, int32_t y, const char* text, Color color, SystemFont font) {
    const uint16_t font_handle = static_cast<uint16_t>(font);
    if (font_handle < MICROPIXEL_SYSTEM_FONT_SMALL || font_handle > MICROPIXEL_SYSTEM_FONT_TITLE) {
        runtime::Panic("graphics.draw_text_centered.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    DrawTextCenteredWithHandle(center_x, y, text, color, font_handle);
}

void Frame::DrawTextCentered(int32_t center_x, int32_t y, const char* text, Color color, const Font& font) {
    if (!font.valid()) {
        runtime::Panic("graphics.draw_text_centered.font", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    DrawTextCenteredWithHandle(center_x, y, text, color, font.handle_);
}

void Frame::DrawTextCenteredWithHandle(int32_t center_x, int32_t y, const char* text, Color color,
                                       uint16_t font_handle) {
    if (text == nullptr || font_handle == 0U) {
        runtime::Panic("graphics.draw_text_centered.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (state_depth_ != 0U) {
        states_[state_depth_].draw_started = true;
    }
    EnsureStateEncoded();
    const Point translation = EffectiveTranslation();
    center_x += translation.x;
    y += translation.y;
    const Rect text_clip = EffectiveClip();
    if (!text_clip.contains(center_x, y)) {
        return;
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
    command.font_handle = font_handle;
    command.text_length = static_cast<uint16_t>(text_length);
    CopyBytes(record, &command, sizeof(command));
    CopyBytes(record + sizeof(command), text, text_length);
}

void Frame::DrawTexture(Point position, const Texture& texture, uint8_t opacity) {
    const Rect source{0, 0, static_cast<int32_t>(texture.width()), static_cast<int32_t>(texture.height())};
    DrawTexture(Rect{position.x, position.y, source.width, source.height}, texture, source, opacity);
}

void Frame::DrawTexture(Point position, const Texture& texture, Rect source, uint8_t opacity) {
    DrawTexture(Rect{position.x, position.y, source.width, source.height}, texture, source, opacity);
}

void Frame::DrawTexture(Rect destination, const Texture& texture, uint8_t opacity) {
    DrawTexture(destination, texture,
                Rect{0, 0, static_cast<int32_t>(texture.width()), static_cast<int32_t>(texture.height())}, opacity);
}

void Frame::DrawTexture(Rect destination, const Texture& texture, Rect source, uint8_t opacity) {
    if (!texture.valid() || destination.empty() || source.x < 0 || source.y < 0 || source.empty() ||
        static_cast<uint64_t>(source.x) + static_cast<uint32_t>(source.width) > texture.width() ||
        static_cast<uint64_t>(source.y) + static_cast<uint32_t>(source.height) > texture.height()) {
        runtime::Panic("graphics.draw_texture.argument", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (state_depth_ != 0U) {
        states_[state_depth_].draw_started = true;
    }
    EnsureStateEncoded();
    const Point translation = EffectiveTranslation();
    destination = destination.translated(translation.x, translation.y);
    const Rect clipped = destination.intersection(EffectiveClip());
    if (clipped.empty()) {
        return;
    }
    const int64_t left_offset = static_cast<int64_t>(clipped.x) - destination.x;
    const int64_t top_offset = static_cast<int64_t>(clipped.y) - destination.y;
    const int64_t right_offset = left_offset + clipped.width;
    const int64_t bottom_offset = top_offset + clipped.height;
    const int32_t source_left = source.x + static_cast<int32_t>(left_offset * source.width / destination.width);
    const int32_t source_top = source.y + static_cast<int32_t>(top_offset * source.height / destination.height);
    const int32_t source_right =
        source.x + static_cast<int32_t>((right_offset * source.width + destination.width - 1) / destination.width);
    const int32_t source_bottom =
        source.y + static_cast<int32_t>((bottom_offset * source.height + destination.height - 1) / destination.height);
    source = Rect{source_left, source_top, source_right - source_left, source_bottom - source_top};
    if (opacity == 255U) {
        micropixel_graphics_draw_texture_command_t command{};
        command.record.opcode = MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE;
        command.record.size = sizeof(command);
        command.texture = texture.handle_;
        command.x = clipped.x;
        command.y = clipped.y;
        command.width = clipped.width;
        command.height = clipped.height;
        command.source_x = source.x;
        command.source_y = source.y;
        command.source_width = source.width;
        command.source_height = source.height;
        CopyBytes(Append(sizeof(command)), &command, sizeof(command));
    } else {
        micropixel_graphics_blend_texture_command_t command{};
        command.record.opcode = MICROPIXEL_GRAPHICS_OP_BLEND_TEXTURE;
        command.record.size = sizeof(command);
        command.texture = texture.handle_;
        command.x = clipped.x;
        command.y = clipped.y;
        command.width = clipped.width;
        command.height = clipped.height;
        command.source_x = source.x;
        command.source_y = source.y;
        command.source_width = source.width;
        command.source_height = source.height;
        command.opacity = opacity;
        CopyBytes(Append(sizeof(command)), &command, sizeof(command));
    }
}

void Frame::DrawTexture(Point position, const StreamingTexture& texture, uint8_t opacity) {
    DrawTexture(position, texture.texture_, opacity);
}

void Frame::DrawTexture(Point position, const StreamingTexture& texture, Rect source, uint8_t opacity) {
    DrawTexture(position, texture.texture_, source, opacity);
}

void Frame::DrawTexture(Rect destination, const StreamingTexture& texture, uint8_t opacity) {
    DrawTexture(destination, texture.texture_, opacity);
}

void Frame::DrawTexture(Rect destination, const StreamingTexture& texture, Rect source, uint8_t opacity) {
    DrawTexture(destination, texture.texture_, source, opacity);
}

void Frame::Save() {
    if (presented_ || state_depth_ >= kMaxStateDepth) {
        runtime::Panic("graphics.save.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    states_[state_depth_ + 1U] = states_[state_depth_];
    states_[state_depth_ + 1U].clip_limit = StateClip();
    states_[state_depth_ + 1U].draw_started = false;
    ++state_depth_;
}

void Frame::SetClipRect(Rect clip) {
    if (state_depth_ == 0U || states_[state_depth_].draw_started || clip.empty()) {
        runtime::Panic("graphics.clip.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    states_[state_depth_].clip = clip;
}

void Frame::Translate(Point offset) {
    if (state_depth_ == 0U || states_[state_depth_].draw_started) {
        runtime::Panic("graphics.translate.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const int64_t x = static_cast<int64_t>(states_[state_depth_].translation.x) + offset.x;
    const int64_t y = static_cast<int64_t>(states_[state_depth_].translation.y) + offset.y;
    if (x <= INT32_MIN || x > INT32_MAX || y <= INT32_MIN || y > INT32_MAX) {
        runtime::Panic("graphics.translate.range", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    states_[state_depth_].translation = Point{static_cast<int32_t>(x), static_cast<int32_t>(y)};
}

void Frame::Restore() {
    if (state_depth_ == 0U || presented_) {
        runtime::Panic("graphics.restore.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (state_encoded_ && encoded_state_depth_ == state_depth_) {
        CloseEncodedState();
    }
    --state_depth_;
}

Result<void> Frame::Present() {
    if (presented_ || state_depth_ != 0U) {
        runtime::Panic("graphics.present.state", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    presented_ = true;
    if (failure_status_ != MICROPIXEL_STATUS_OK) {
        const int32_t status = failure_status_;
        Cancel();
        return unexpected(ErrorFromStatus(status));
    }
    if (draw_operation_count_ == 0U) {
        return {};
    }
    if (!SubmitBatch()) {
        const int32_t status = failure_status_;
        Cancel();
        return unexpected(ErrorFromStatus(status));
    }
    if (host_frame_active_) {
        const int32_t status = CallVoid(graphics_service, MICROPIXEL_GRAPHICS_METHOD_FRAME_COMMIT, nullptr, 0U);
        if (status != MICROPIXEL_STATUS_OK) {
            Cancel();
            return unexpected(ErrorFromStatus(status));
        }
        host_frame_active_ = false;
    }
    return {};
}

void Frame::Cancel() {
    if (host_frame_active_) {
        if (OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                        MICROPIXEL_GRAPHICS_INTERFACE_MINOR) == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(graphics_service, MICROPIXEL_GRAPHICS_METHOD_FRAME_CANCEL, nullptr, 0U);
        }
        host_frame_active_ = false;
    }
    presented_ = true;
}

Frame Renderer::BeginFrame() const { return Frame{Frame::CapabilityToken{}, info()}; }

namespace {

Result<TextMetrics> MeasureTextWithHandle(const char* text, uint16_t font_handle) {
    if (text == nullptr || font_handle == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    uint32_t text_length = 0U;
    while (text_length <= MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES && text[text_length] != '\0') {
        ++text_length;
    }
    if (text_length == 0U || text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }

    int32_t status = OpenService(graphics_service, MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                                 MICROPIXEL_GRAPHICS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    alignas(4)
        uint8_t request[sizeof(micropixel_graphics_measure_text_request_t) + MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES]{};
    const uint32_t request_size = sizeof(micropixel_graphics_measure_text_request_t) + text_length;
    micropixel_graphics_measure_text_request_t header{};
    header.size = static_cast<uint16_t>(request_size);
    header.font = font_handle;
    header.text_length = static_cast<uint16_t>(text_length);
    CopyBytes(request, &header, sizeof(header));
    CopyBytes(request + sizeof(header), text, text_length);

    micropixel_text_metrics_t response{};
    uint32_t response_size = 0U;
    status = CallService(graphics_service, MICROPIXEL_GRAPHICS_METHOD_MEASURE_TEXT, request, request_size, &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.reserved0 != 0U) {
        runtime::Panic("graphics.measure_text.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return TextMetrics{response.width, response.height, response.baseline};
}

}  // namespace

Result<TextMetrics> Renderer::MeasureText(const char* text, SystemFont font) const {
    const uint16_t font_handle = static_cast<uint16_t>(font);
    if (font_handle < MICROPIXEL_SYSTEM_FONT_SMALL || font_handle > MICROPIXEL_SYSTEM_FONT_TITLE) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    return MeasureTextWithHandle(text, font_handle);
}

Result<TextMetrics> Renderer::MeasureText(const char* text, const Font& font) const {
    if (!font.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    return MeasureTextWithHandle(text, font.handle_);
}

InputInfo Input::info() const {
    const micropixel_input_info_t& raw = LoadInputInfo();
    return InputInfo{raw.max_touch_points, raw.capabilities};
}

Texture::Texture(Texture&& other) noexcept : handle_(other.handle_), width_(other.width_), height_(other.height_) {
    other.handle_ = 0U;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        width_ = other.width_;
        height_ = other.height_;
        other.handle_ = 0U;
    }
    return *this;
}

Texture::~Texture() { Reset(); }

void Texture::Reset() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        if (OpenResourceService() == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_RELEASE, &request, sizeof(request));
        }
        handle_ = 0U;
        width_ = 0U;
        height_ = 0U;
    }
}

Font::Font(Font&& other) noexcept
    : handle_(other.handle_),
      size_(other.size_),
      line_height_(other.line_height_),
      ascent_(other.ascent_),
      descent_(other.descent_) {
    other.handle_ = 0U;
}

Font& Font::operator=(Font&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        size_ = other.size_;
        line_height_ = other.line_height_;
        ascent_ = other.ascent_;
        descent_ = other.descent_;
        other.handle_ = 0U;
    }
    return *this;
}

Font::~Font() { Reset(); }

void Font::Reset() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        if (OpenResourceService() == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_FONT_RELEASE, &request, sizeof(request));
        }
        handle_ = 0U;
        size_ = 0U;
        line_height_ = 0U;
        ascent_ = 0;
        descent_ = 0;
    }
}

Result<void> StreamingTexture::Update(Rect dirty, const uint8_t* pixels, uint32_t byte_length, uint32_t pitch) {
    const uint32_t bytes_per_pixel =
        pixel_format_ == PixelFormat::kBgr888 ? 3U : (pixel_format_ == PixelFormat::kBgra8888 ? 4U : 0U);
    if (!valid() || dirty.x < 0 || dirty.y < 0 || dirty.width <= 0 || dirty.height <= 0 || pixels == nullptr ||
        bytes_per_pixel == 0U || static_cast<int64_t>(dirty.x) + dirty.width > static_cast<int64_t>(width()) ||
        static_cast<int64_t>(dirty.y) + dirty.height > static_cast<int64_t>(height())) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    const uint32_t row_bytes = static_cast<uint32_t>(dirty.width) * bytes_per_pixel;
    const uint64_t required_bytes = static_cast<uint64_t>(dirty.height - 1) * pitch + row_bytes;
    if (pitch < row_bytes || required_bytes > byte_length) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    constexpr uint32_t kHeaderBytes = sizeof(micropixel_streaming_texture_update_request_t);
    static_assert(kHeaderBytes < MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES,
                  "streaming texture update header exceeds ABI request");
    const uint32_t rows_per_request = (MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES - kHeaderBytes) / row_bytes;
    if (rows_per_request == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }

    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    alignas(4) uint8_t request[MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES]{};
    uint32_t row = 0U;
    while (row < static_cast<uint32_t>(dirty.height)) {
        uint32_t row_count = static_cast<uint32_t>(dirty.height) - row;
        if (row_count > rows_per_request) {
            row_count = rows_per_request;
        }
        const uint32_t pixel_bytes = row_count * row_bytes;
        const uint32_t request_size = kHeaderBytes + pixel_bytes;
        micropixel_streaming_texture_update_request_t header{};
        header.size = static_cast<uint16_t>(request_size);
        header.texture = texture_.handle_;
        header.x = static_cast<uint32_t>(dirty.x);
        header.y = static_cast<uint32_t>(dirty.y) + row;
        header.width = static_cast<uint32_t>(dirty.width);
        header.height = row_count;
        header.pitch = row_bytes;
        CopyBytes(request, &header, sizeof(header));
        for (uint32_t source_row = 0U; source_row < row_count; ++source_row) {
            CopyBytes(request + kHeaderBytes + source_row * row_bytes, pixels + (row + source_row) * pitch, row_bytes);
        }
        status = CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_UPDATE, request, request_size);
        if (status != MICROPIXEL_STATUS_OK) {
            return unexpected(ErrorFromStatus(status));
        }
        row += row_count;
    }
    return {};
}

TextureUpdateBatch::TextureUpdateBatch(TextureUpdateBatch&& other) noexcept : active_(other.active_) {
    other.active_ = false;
}

TextureUpdateBatch::~TextureUpdateBatch() {
    if (active_) {
        (void)Finish();
    }
}

Result<void> TextureUpdateBatch::Finish() {
    if (!active_) {
        return {};
    }
    active_ = false;
    int32_t status = OpenResourceService();
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_FINISH, nullptr, 0U);
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<Texture> Resources::LoadTexture(AssetId asset) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_resource_load_texture_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, asset.value()};
    micropixel_texture_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_LOAD_TEXTURE, &request, sizeof(request),
                         &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_RESOURCE_INTERFACE_MAJOR || response.texture == 0U ||
        response.width == 0U || response.height == 0U ||
        (response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
         response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888) ||
        (response.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) != 0U) {
        runtime::Panic("resources.load_texture.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return Texture{response.texture, response.width, response.height};
}

Result<Font> Resources::LoadFont(AssetId asset) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_resource_load_font_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, asset.value()};
    micropixel_font_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_LOAD_FONT, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_RESOURCE_INTERFACE_MAJOR || response.font == 0U ||
        response.font_size == 0U || response.line_height == 0U || response.ascent <= 0 || response.descent < 0 ||
        response.reserved[0] != 0U || response.reserved[1] != 0U) {
        runtime::Panic("resources.load_font.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return Font{response.font, response.font_size, response.line_height, response.ascent, response.descent};
}

Result<StreamingTexture> Renderer::CreateStreamingTexture(Size size, PixelFormat pixel_format) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (size.width == 0U || size.height == 0U ||
        (pixel_format != PixelFormat::kBgr888 && pixel_format != PixelFormat::kBgra8888)) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_streaming_texture_create_request_t request{};
    request.size = sizeof(request);
    request.width = size.width;
    request.height = size.height;
    request.pixel_format = static_cast<uint32_t>(pixel_format);
    micropixel_texture_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_CREATE, &request,
                         sizeof(request), &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_RESOURCE_INTERFACE_MAJOR || response.texture == 0U ||
        response.width != size.width || response.height != size.height ||
        response.pixel_format != static_cast<uint32_t>(pixel_format) ||
        (response.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) == 0U) {
        runtime::Panic("texture.create.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return StreamingTexture{Texture{response.texture, response.width, response.height}, pixel_format};
}

TextureUpdateBatch Renderer::BeginTextureUpdateBatch() const {
    RequireOk(OpenResourceService(), "texture.batch.begin.open");
    RequireOk(CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_BEGIN, nullptr, 0U),
              "texture.batch.begin");
    return TextureUpdateBatch{TextureUpdateBatch::CapabilityToken{}};
}

void Application::BeginRun() const {
    if (running_) {
        runtime::Panic("application.run.reentrant", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    running_ = true;
}

void Application::EndRun() const { running_ = false; }

Event Application::WaitEvent() const {
    Event event;
    if (!WaitEventInternal(event, UINT64_MAX)) {
        runtime::Panic("application.wait_event.timeout", MICROPIXEL_STATUS_INTERNAL);
    }
    return event;
}

bool Application::WaitEventFor(Event& event, Duration timeout) const {
    if (timeout.count_microseconds() == UINT64_MAX) {
        runtime::Panic("application.wait_event_for.timeout", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return WaitEventInternal(event, timeout.count_microseconds());
}

bool Application::PollEvent(Event& event) const { return WaitEventInternal(event, 0U); }

bool Application::WaitEventInternal(Event& event, uint64_t timeout_us) const {
    micropixel_event_t raw{};
    const int32_t status = micropixel_event_wait(&raw, sizeof(raw), timeout_us);
    if (status == MICROPIXEL_STATUS_TIMEOUT) {
        return false;
    }
    RequireOk(status, "application.wait_event");
    if (raw.size != sizeof(raw)) {
        runtime::Panic("application.wait_event.size", MICROPIXEL_STATUS_INTERNAL);
    }

    TimePoint timestamp{raw.timestamp_us};
    if (raw.service_id == 0U && raw.event_id == MICROPIXEL_CORE_EVENT_RESUME) {
        event = Event{EventType::kResume, timestamp};
        return true;
    }
    if (raw.service_id == 0U && raw.event_id == MICROPIXEL_CORE_EVENT_STOP) {
        event = Event{EventType::kStop, timestamp};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_TIMER && raw.event_id == MICROPIXEL_TIMER_EVENT_EXPIRED) {
        micropixel_timer_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        event =
            Event{TimerEvent{timestamp, Duration::Microseconds(payload.elapsed_us), payload.missed_count, raw.source}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_AUDIO && raw.event_id == MICROPIXEL_AUDIO_EVENT_PLAYBACK_FINISHED) {
        micropixel_audio_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (payload.playback == 0U || payload.playback != raw.source || payload.reserved[0] != 0U ||
            payload.reserved[1] != 0U || payload.reserved[2] != 0U || raw.status > MICROPIXEL_STATUS_OK) {
            runtime::Panic("application.wait_event.audio_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{AudioPlaybackEvent{timestamp, raw.status == MICROPIXEL_STATUS_OK, raw.source}};
        return true;
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
        const micropixel_input_info_t& input = LoadInputInfo();
        const bool has_pressure = (input.capabilities & MICROPIXEL_INPUT_CAP_PRESSURE) != 0U;
        if (has_pressure && payload.pressure_per_mille > 1000U) {
            runtime::Panic("application.wait_event.touch_pressure", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{TouchEvent{timestamp, phase, raw.source, payload.x, payload.y, has_pressure,
                                 static_cast<uint16_t>(has_pressure ? payload.pressure_per_mille : 0U)}};
        return true;
    }

    if (raw.service_id == MICROPIXEL_SERVICE_INPUT && raw.event_id == MICROPIXEL_INPUT_EVENT_KEY) {
        micropixel_key_event_payload_t payload{};
        CopyBytes(&payload, raw.payload, sizeof(payload));
        if (payload.code < MICROPIXEL_KEY_UP || payload.code > MICROPIXEL_KEY_GAMEPAD_NORTH ||
            payload.modifiers != 0U || payload.reserved0 != 0U) {
            runtime::Panic("application.wait_event.key_payload", MICROPIXEL_STATUS_INTERNAL);
        }
        KeyPhase phase = KeyPhase::kCancel;
        switch (payload.phase) {
            case MICROPIXEL_KEY_DOWN_PHASE:
                phase = KeyPhase::kDown;
                break;
            case MICROPIXEL_KEY_UP_PHASE:
                phase = KeyPhase::kUp;
                break;
            case MICROPIXEL_KEY_REPEAT_PHASE:
                phase = KeyPhase::kRepeat;
                break;
            case MICROPIXEL_KEY_CANCEL_PHASE:
                phase = KeyPhase::kCancel;
                break;
            default:
                runtime::Panic("application.wait_event.key_phase", MICROPIXEL_STATUS_INTERNAL);
        }
        if ((phase == KeyPhase::kRepeat) != (payload.repeat_count != 0U)) {
            runtime::Panic("application.wait_event.key_repeat", MICROPIXEL_STATUS_INTERNAL);
        }
        event = Event{KeyEvent{timestamp, static_cast<KeyCode>(payload.code), phase, payload.repeat_count}};
        return true;
    }

    event = Event{timestamp};
    return true;
}

}  // namespace micropixel
