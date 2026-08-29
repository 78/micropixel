#include "host/controller/remote/remote_identity_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

namespace micropixel::firmware::remote_control {
namespace {

constexpr char kTag[] = "remote_identity";
constexpr char kPartition[] = "sys_store";
constexpr char kNamespace[] = "control";
constexpr char kIdentityKey[] = "identity";
constexpr uint8_t kIdentityVersion = 1U;

template <size_t Capacity>
void CopyText(std::array<char, Capacity>& destination, const char* source) {
    destination.fill('\0');
    if (source != nullptr) {
        const size_t length = ::strnlen(source, destination.size() - 1U);
        std::memcpy(destination.data(), source, length);
    }
}

}  // namespace

struct RemoteIdentityStore::Record final {
    uint8_t version{};
    uint8_t reserved[3]{};
    uint32_t auth_epoch{};
    char device_id[host_ui::kRemoteControlDeviceIdCapacity]{};
    char credential[1024U]{};
};

RemoteIdentityStore::RemoteIdentityStore() {
    static_assert(sizeof(Record) == 1072U, "Remote Control identity record layout changed");
    void* storage = heap_caps_calloc(1U, sizeof(Record), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage != nullptr) {
        record_ = std::construct_at(static_cast<Record*>(storage));
    }
}

RemoteIdentityStore::~RemoteIdentityStore() {
    if (record_ != nullptr) {
        std::destroy_at(record_);
        heap_caps_free(record_);
    }
}

bool RemoteIdentityStore::Load(RemoteIdentity& identity) const {
    if (record_ == nullptr) {
        return false;
    }
    nvs_handle_t handle{};
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    *record_ = {};
    size_t size = sizeof(*record_);
    const esp_err_t error = nvs_get_blob(handle, kIdentityKey, record_, &size);
    nvs_close(handle);
    const bool valid =
        error == ESP_OK && size == sizeof(*record_) && record_->version == kIdentityVersion &&
        record_->auth_epoch != 0U && record_->device_id[0] != '\0' && record_->credential[0] != '\0' &&
        ::strnlen(record_->device_id, sizeof(record_->device_id)) < sizeof(record_->device_id) &&
        ::strnlen(record_->credential, sizeof(record_->credential)) < sizeof(record_->credential) &&
        std::all_of(
            record_->reserved, record_->reserved + sizeof(record_->reserved), [](uint8_t byte) { return byte == 0U; });
    if (!valid) {
        return false;
    }
    CopyText(identity.device_id, record_->device_id);
    CopyText(identity.credential, record_->credential);
    identity.auth_epoch = record_->auth_epoch;
    return true;
}

bool RemoteIdentityStore::Save(const RemoteIdentity& identity) const {
    if (record_ == nullptr) {
        return false;
    }
    *record_ = {};
    record_->version = kIdentityVersion;
    record_->auth_epoch = identity.auth_epoch;
    std::snprintf(record_->device_id, sizeof(record_->device_id), "%s", identity.device_id.data());
    std::snprintf(record_->credential, sizeof(record_->credential), "%s", identity.credential.data());
    nvs_handle_t handle{};
    esp_err_t error = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, kIdentityKey, record_, sizeof(*record_));
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to save Remote Control identity: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}

bool RemoteIdentityStore::Clear() const {
    nvs_handle_t handle{};
    esp_err_t error = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_erase_key(handle, kIdentityKey);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            error = ESP_OK;
        }
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "failed to clear rejected Remote Control identity: %s", esp_err_to_name(error));
        return false;
    }
    return true;
}

}  // namespace micropixel::firmware::remote_control
