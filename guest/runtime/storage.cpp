#include "sdk/storage.hpp"

#include "runtime/service_binding.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::OpenService;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache storage_service;

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

namespace micropixel {

static_assert(KVStore::kMaximumKeyBytes == MICROPIXEL_STORAGE_MAX_KEY_BYTES, "SDK/ABI storage key limit drifted");
static_assert(KVStore::kMaximumValueBytes == MICROPIXEL_STORAGE_MAX_VALUE_BYTES, "SDK/ABI storage value limit drifted");
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

}  // namespace micropixel
