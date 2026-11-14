// SPDX-License-Identifier: Apache-2.0
#include "runtime/services/app_storage.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "nvs.h"
#include "runtime/bundle/bundle_format.h"

namespace micropixel::runtime {

bool AppStorageNamespace(std::string_view app_id, std::span<char> output) {
    if (app_id.empty() || app_id.size() > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH || output.size() < NVS_NS_NAME_MAX_SIZE) {
        return false;
    }
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : app_id) {
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
              byte == '_' || byte == '-' || byte == '.')) {
            return false;
        }
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    if (app_id.size() < NVS_NS_NAME_MAX_SIZE) {
        std::memcpy(output.data(), app_id.data(), app_id.size());
        output[app_id.size()] = '\0';
        return true;
    }
    const int written =
        std::snprintf(output.data(), output.size(), "m%014" PRIx64, hash & UINT64_C(0x00FFFFFFFFFFFFFF));
    return written == NVS_NS_NAME_MAX_SIZE - 1;
}

esp_err_t EraseAppStorage(std::string_view app_id) {
    char name[NVS_NS_NAME_MAX_SIZE]{};
    if (!AppStorageNamespace(app_id, name)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle{};
    esp_err_t status = nvs_open_from_partition("runtime_nvs", name, NVS_READONLY, &handle);
    if (status == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (status != ESP_OK) return status;
    nvs_close(handle);
    status = nvs_open_from_partition("runtime_nvs", name, NVS_READWRITE, &handle);
    if (status != ESP_OK) return status;
    status = nvs_erase_all(handle);
    if (status == ESP_OK) status = nvs_commit(handle);
    nvs_close(handle);
    return status;
}

}  // namespace micropixel::runtime
