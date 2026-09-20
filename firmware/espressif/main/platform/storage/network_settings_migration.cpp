// SPDX-License-Identifier: Apache-2.0
#include "platform/storage/network_settings_migration.hpp"

#include <cstddef>
#include <cstdint>

#include "nvs.h"
#include "platform/memory/psram_buffer.hpp"

namespace micropixel::platform::storage {
namespace {

class Handle final {
   public:
    ~Handle() {
        if (value != 0U) nvs_close(value);
    }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    nvs_handle_t value{};
};

esp_err_t MigrateKey(const char* name, const char* key, bool blob) {
    Handle source;
    esp_err_t status = nvs_open_from_partition("runtime_nvs", name, NVS_READONLY, &source.value);
    if (status == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (status != ESP_OK) return status;

    size_t size = 0U;
    int32_t integer = 0;
    status = blob ? nvs_get_blob(source.value, key, nullptr, &size) : nvs_get_i32(source.value, key, &integer);
    if (status == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (status != ESP_OK) return status;

    Handle destination;
    status = nvs_open_from_partition("nvs", name, NVS_READWRITE, &destination.value);
    if (status != ESP_OK) return status;
    size_t destination_size = 0U;
    int32_t destination_integer = 0;
    status = blob ? nvs_get_blob(destination.value, key, nullptr, &destination_size)
                  : nvs_get_i32(destination.value, key, &destination_integer);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
        if (blob) {
            // The v1 Wi-Fi state is under 1 KiB; bound corrupt input without
            // putting a migration buffer on the startup task's stack.
            if (size == 0U || size > 4096U) return ESP_ERR_INVALID_ARG;
            memory::PsramBuffer<std::byte> bytes;
            if (!bytes.Allocate(size)) return ESP_ERR_NO_MEM;
            status = nvs_get_blob(source.value, key, bytes.View().data(), &size);
            if (status == ESP_OK) status = nvs_set_blob(destination.value, key, bytes.View().data(), size);
        } else {
            status = nvs_set_i32(destination.value, key, integer);
        }
    }
    if (status != ESP_OK) return status;
    // The destination wins on restart after an interrupted migration. Commit
    // it before removing the legacy key, including on this retry path.
    status = nvs_commit(destination.value);
    if (status != ESP_OK) return status;
    Handle cleanup;
    status = nvs_open_from_partition("runtime_nvs", name, NVS_READWRITE, &cleanup.value);
    if (status == ESP_OK) status = nvs_erase_key(cleanup.value, key);
    return status == ESP_OK ? nvs_commit(cleanup.value) : status;
}

}  // namespace

esp_err_t MigrateNetworkSettings() {
    Handle marker;
    esp_err_t status = nvs_open_from_partition("nvs", "mp_migrations", NVS_READWRITE, &marker.value);
    if (status != ESP_OK) return status;
    uint8_t migrated = 0U;
    status = nvs_get_u8(marker.value, "network_v1", &migrated);
    if (status == ESP_OK && migrated == 1U) return ESP_OK;
    if (status != ESP_OK && status != ESP_ERR_NVS_NOT_FOUND) return status;
    status = MigrateKey("host_wifi", "state", true);
    if (status == ESP_OK) status = MigrateKey("network", "type", false);
    if (status != ESP_OK) return status;
    // Once complete, these old namespace names belong to Apps too. Do not
    // mistake a future App's keys for legacy Host settings on the next boot.
    status = nvs_set_u8(marker.value, "network_v1", 1U);
    return status == ESP_OK ? nvs_commit(marker.value) : status;
}

}  // namespace micropixel::platform::storage
