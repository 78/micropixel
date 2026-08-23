#include "runtime/services/storage_service.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "abi/micropixel_abi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "micropixel_storage";
constexpr char kPartition[] = "runtime_nvs";

int32_t StatusFromNvs(esp_err_t error) {
    if (error == ESP_OK) {
        return MICROPIXEL_STATUS_OK;
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (error == ESP_ERR_NVS_TYPE_MISMATCH) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (error == ESP_ERR_NVS_NOT_ENOUGH_SPACE || error == ESP_ERR_NO_MEM) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    return MICROPIXEL_STATUS_INTERNAL;
}

bool ValidIdentifierByte(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
           byte == '_' || byte == '-' || byte == '.';
}

uint64_t AppIdHash(const uint8_t* bytes, size_t length) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t index = 0U; index < length; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

namespace micropixel::runtime {

StorageService::StorageService(const micropixel_aot_package_t& package) : token_refill_time_us_(esp_timer_get_time()) {
    size_t length = 0U;
    while (length < sizeof(package.app_id) && package.app_id[length] != 0U) {
        if (!ValidIdentifierByte(package.app_id[length])) {
            ESP_LOGE(kTag, "Bundle AppId contains an invalid namespace byte");
            return;
        }
        ++length;
    }
    if (length == 0U || length >= sizeof(package.app_id)) {
        ESP_LOGE(kTag, "Bundle AppId is not a valid NVS namespace");
        return;
    }
    if (length < sizeof(namespace_)) {
        memcpy(namespace_, package.app_id, length);
    } else {
        const uint64_t hash = AppIdHash(package.app_id, length) & 0x00FFFFFFFFFFFFFFULL;
        int written = snprintf(namespace_, sizeof(namespace_), "m%014" PRIx64, hash);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(namespace_)) {
            ESP_LOGE(kTag, "unable to derive NVS namespace from Bundle AppId");
            return;
        }
        ESP_LOGI(kTag, "long AppId uses private KV namespace '%s'", namespace_);
    }
    esp_err_t error = nvs_open_from_partition(kPartition, namespace_, NVS_READWRITE, &handle_);
    if (error != ESP_OK) {
        handle_ = 0U;
        ESP_LOGE(kTag, "unable to open private KV namespace '%s': %s", namespace_, esp_err_to_name(error));
        return;
    }
    size_t bytes = 0U;
    size_t keys = 0U;
    if (!Usage(bytes, keys)) {
        nvs_close(handle_);
        handle_ = 0U;
        ESP_LOGE(kTag, "unable to scan private KV namespace '%s'", namespace_);
        return;
    }
    ESP_LOGI(kTag, "private KV ready: app=%s keys=%zu bytes=%zu quota=%d bytes/%d keys", namespace_, keys, bytes,
             CONFIG_MICROPIXEL_KV_MAX_BYTES, CONFIG_MICROPIXEL_KV_MAX_KEYS);
}

StorageService::~StorageService() {
    if (handle_ != 0U) {
        nvs_close(handle_);
    }
}

int32_t StorageService::NormalizeKey(const char* key, uint32_t key_length,
                                     char (&normalized)[NVS_KEY_NAME_MAX_SIZE]) const {
    if (handle_ == 0U) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (key == nullptr || key_length == 0U || key_length >= NVS_KEY_NAME_MAX_SIZE) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    for (uint32_t index = 0U; index < key_length; ++index) {
        if (!ValidIdentifierByte(static_cast<uint8_t>(key[index]))) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        normalized[index] = key[index];
    }
    normalized[key_length] = '\0';
    return MICROPIXEL_STATUS_OK;
}

bool StorageService::ValueSize(const char* key, nvs_type_t type, size_t& size_out) const {
    switch (type) {
        case NVS_TYPE_U8:
        case NVS_TYPE_I8:
            size_out = 1U;
            return true;
        case NVS_TYPE_U16:
        case NVS_TYPE_I16:
            size_out = 2U;
            return true;
        case NVS_TYPE_U32:
        case NVS_TYPE_I32:
            size_out = 4U;
            return true;
        case NVS_TYPE_U64:
        case NVS_TYPE_I64:
            size_out = 8U;
            return true;
        case NVS_TYPE_BLOB:
            size_out = 0U;
            return nvs_get_blob(handle_, key, nullptr, &size_out) == ESP_OK;
        case NVS_TYPE_STR:
            size_out = 0U;
            return nvs_get_str(handle_, key, nullptr, &size_out) == ESP_OK;
        default:
            return false;
    }
}

bool StorageService::Usage(size_t& bytes_out, size_t& keys_out) const {
    bytes_out = 0U;
    keys_out = 0U;
    nvs_iterator_t iterator = nullptr;
    esp_err_t error = nvs_entry_find_in_handle(handle_, NVS_TYPE_ANY, &iterator);
    while (error == ESP_OK) {
        nvs_entry_info_t info{};
        if (nvs_entry_info(iterator, &info) != ESP_OK) {
            nvs_release_iterator(iterator);
            return false;
        }
        size_t size = 0U;
        if (!ValueSize(info.key, info.type, size)) {
            nvs_release_iterator(iterator);
            return false;
        }
        bytes_out += size;
        ++keys_out;
        error = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    return error == ESP_ERR_NVS_NOT_FOUND;
}

bool StorageService::ConsumeWriteToken() {
    int64_t now = esp_timer_get_time();
    constexpr int64_t kRefillUs = static_cast<int64_t>(CONFIG_MICROPIXEL_KV_WRITE_REFILL_MS) * 1000LL;
    if (now > token_refill_time_us_) {
        uint64_t additions = static_cast<uint64_t>(now - token_refill_time_us_) / static_cast<uint64_t>(kRefillUs);
        if (additions != 0U) {
            uint64_t available = static_cast<uint64_t>(write_tokens_) + additions;
            write_tokens_ = static_cast<uint32_t>(
                available < CONFIG_MICROPIXEL_KV_WRITE_BURST ? available : CONFIG_MICROPIXEL_KV_WRITE_BURST);
            token_refill_time_us_ += static_cast<int64_t>(additions) * kRefillUs;
        }
    }
    if (write_tokens_ == 0U) {
        return false;
    }
    --write_tokens_;
    return true;
}

int32_t StorageService::PrepareWrite(const char* key, size_t new_size) {
    if (new_size > CONFIG_MICROPIXEL_KV_MAX_VALUE_BYTES) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    size_t bytes = 0U;
    size_t keys = 0U;
    if (!Usage(bytes, keys)) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    nvs_type_t previous_type = NVS_TYPE_ANY;
    esp_err_t find_error = nvs_find_key(handle_, key, &previous_type);
    size_t previous_size = 0U;
    if (find_error == ESP_OK) {
        if (!ValueSize(key, previous_type, previous_size)) {
            return MICROPIXEL_STATUS_INTERNAL;
        }
    } else if (find_error == ESP_ERR_NVS_NOT_FOUND) {
        if (keys >= CONFIG_MICROPIXEL_KV_MAX_KEYS) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
    } else {
        return StatusFromNvs(find_error);
    }
    if (bytes < previous_size || bytes - previous_size + new_size > CONFIG_MICROPIXEL_KV_MAX_BYTES) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    if (!ConsumeWriteToken()) {
        return MICROPIXEL_STATUS_RATE_LIMITED;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t StorageService::Commit(esp_err_t mutation_error) {
    if (mutation_error != ESP_OK) {
        return StatusFromNvs(mutation_error);
    }
    return StatusFromNvs(nvs_commit(handle_));
}

ServiceResult<uint32_t> StorageService::GetU32(const char* key, uint32_t key_length) {
    char normalized[NVS_KEY_NAME_MAX_SIZE]{};
    int32_t status = NormalizeKey(key, key_length, normalized);
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<uint32_t>(status);
    }
    uint32_t value = 0U;
    status = StatusFromNvs(nvs_get_u32(handle_, normalized, &value));
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<uint32_t>(status);
    }
    return value;
}

ServiceResult<void> StorageService::SetU32(const char* key, uint32_t key_length, uint32_t value) {
    char normalized[NVS_KEY_NAME_MAX_SIZE]{};
    int32_t status = NormalizeKey(key, key_length, normalized);
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<void>(status);
    }
    status = PrepareWrite(normalized, sizeof(value));
    return status == MICROPIXEL_STATUS_OK ? ServiceStatus(Commit(nvs_set_u32(handle_, normalized, value)))
                                          : FailService<void>(status);
}

ServiceResult<bool> StorageService::GetBool(const char* key, uint32_t key_length) {
    char normalized[NVS_KEY_NAME_MAX_SIZE]{};
    int32_t status = NormalizeKey(key, key_length, normalized);
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<bool>(status);
    }
    uint8_t value = 0U;
    status = StatusFromNvs(nvs_get_u8(handle_, normalized, &value));
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<bool>(status);
    }
    return value != 0U;
}

ServiceResult<void> StorageService::SetBool(const char* key, uint32_t key_length, bool value) {
    char normalized[NVS_KEY_NAME_MAX_SIZE]{};
    int32_t status = NormalizeKey(key, key_length, normalized);
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<void>(status);
    }
    status = PrepareWrite(normalized, sizeof(uint8_t));
    return status == MICROPIXEL_STATUS_OK
               ? ServiceStatus(Commit(nvs_set_u8(handle_, normalized, static_cast<uint8_t>(value))))
               : FailService<void>(status);
}

ServiceResult<uint32_t> StorageService::GetBytes(const char* key, uint32_t key_length, uint8_t* bytes_out,
                                                 uint32_t capacity) {
    char normalized[NVS_KEY_NAME_MAX_SIZE]{};
    int32_t status = NormalizeKey(key, key_length, normalized);
    if (status != MICROPIXEL_STATUS_OK || (bytes_out == nullptr && capacity != 0U)) {
        if (status == MICROPIXEL_STATUS_OK) {
            status = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return FailService<uint32_t>(status);
    }
    size_t required = 0U;
    esp_err_t error = nvs_get_blob(handle_, normalized, nullptr, &required);
    if (error != ESP_OK) {
        return FailService<uint32_t>(StatusFromNvs(error));
    }
    if (required > capacity) {
        return FailService<uint32_t>(MICROPIXEL_STATUS_BUFFER_TOO_SMALL, static_cast<uint32_t>(required));
    }
    size_t actual = capacity;
    status = StatusFromNvs(nvs_get_blob(handle_, normalized, bytes_out, &actual));
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<uint32_t>(status);
    }
    return static_cast<uint32_t>(actual);
}

ServiceResult<void> StorageService::SetBytes(const char* key, uint32_t key_length, const uint8_t* bytes,
                                             uint32_t length) {
    char normalized[NVS_KEY_NAME_MAX_SIZE]{};
    int32_t status = NormalizeKey(key, key_length, normalized);
    if (status != MICROPIXEL_STATUS_OK || (bytes == nullptr && length != 0U)) {
        if (status == MICROPIXEL_STATUS_OK) {
            status = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        return FailService<void>(status);
    }
    status = PrepareWrite(normalized, length);
    return status == MICROPIXEL_STATUS_OK ? ServiceStatus(Commit(nvs_set_blob(handle_, normalized, bytes, length)))
                                          : FailService<void>(status);
}

ServiceResult<void> StorageService::Remove(const char* key, uint32_t key_length) {
    char normalized[NVS_KEY_NAME_MAX_SIZE]{};
    int32_t status = NormalizeKey(key, key_length, normalized);
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<void>(status);
    }
    esp_err_t find_error = nvs_find_key(handle_, normalized, nullptr);
    if (find_error != ESP_OK) {
        return FailService<void>(StatusFromNvs(find_error));
    }
    if (!ConsumeWriteToken()) {
        return FailService<void>(MICROPIXEL_STATUS_RATE_LIMITED);
    }
    return ServiceStatus(Commit(nvs_erase_key(handle_, normalized)));
}

}  // namespace micropixel::runtime
