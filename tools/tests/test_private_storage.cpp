// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "fake_nvs.hpp"
#include "platform/storage/network_settings_migration.hpp"
#include "runtime/services/app_storage.hpp"
#include "runtime/services/storage_service.hpp"

namespace {
void UsageStatistics() {
    using micropixel::runtime::ReadAppStorageUsage;
    // 16 NVS pages, 126 entries/page; one page is reserved for GC.
    fake_nvs::stats = {.used_entries = 100U,
                       .free_entries = 1916U,
                       .available_entries = 1790U,
                       .total_entries = 2016U,
                       .namespace_count = 3U};
    auto usage = ReadAppStorageUsage();
    assert(usage && usage->partition_bytes == 65536U);
    assert(usage->used_bytes == 3200U && usage->available_bytes == 57280U);
    // Deleted/reclaimable entries count as available, but the GC page never does.
    fake_nvs::stats = {.used_entries = 3U,
                       .free_entries = 2013U,
                       .available_entries = 1887U,
                       .total_entries = 2016U,
                       .namespace_count = 3U};
    usage = ReadAppStorageUsage();
    assert(usage && usage->used_bytes == 96U && usage->available_bytes == 60384U);
    fake_nvs::stats = {.used_entries = 1890U,
                       .free_entries = 126U,
                       .available_entries = 0U,
                       .total_entries = 2016U,
                       .namespace_count = 3U};
    usage = ReadAppStorageUsage();
    assert(usage && usage->available_bytes == 0U);
    fake_nvs::stats_status = ESP_FAIL;
    assert(ReadAppStorageUsage().error() == ESP_FAIL);
    fake_nvs::stats_status = ESP_OK;
    fake_nvs::partition_present = false;
    assert(ReadAppStorageUsage().error() == ESP_ERR_NOT_FOUND);
    fake_nvs::partition_present = true;
    fake_nvs::stats.used_entries = 2017U;
    assert(ReadAppStorageUsage().error() == ESP_ERR_INVALID_STATE);
    fake_nvs::stats.used_entries = 1890U;
    fake_nvs::stats.available_entries = 127U;
    assert(ReadAppStorageUsage().error() == ESP_ERR_INVALID_STATE);
    fake_nvs::stats = {};
}

void Migration() {
    using micropixel::platform::storage::MigrateNetworkSettings;
    fake_nvs::data.clear();
    assert(MigrateNetworkSettings() == ESP_OK);
    assert(fake_nvs::data.size() == 1 && fake_nvs::data.contains("nvs/mp_migrations"));
    fake_nvs::data.clear();
    const fake_nvs::Value wifi{NVS_TYPE_BLOB, {1, 2, 3, 4}};
    const fake_nvs::Value cellular{NVS_TYPE_I32, {1, 0, 0, 0}};
    fake_nvs::data["runtime_nvs/host_wifi"]["state"] = wifi;
    fake_nvs::data["runtime_nvs/host_wifi"]["unrelated"] = wifi;
    fake_nvs::data["runtime_nvs/network"]["type"] = cellular;
    fake_nvs::data["runtime_nvs/game"]["save"] = wifi;
    fake_nvs::fail_write = true;
    assert(MigrateNetworkSettings() == ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    assert(fake_nvs::data["runtime_nvs/host_wifi"].contains("state"));
    fake_nvs::fail_write = false;
    fake_nvs::fail_commit_partition = "nvs";
    assert(MigrateNetworkSettings() == ESP_FAIL);
    assert(fake_nvs::data["runtime_nvs/host_wifi"].contains("state"));
    fake_nvs::fail_commit_partition.clear();
    // Simulate an existing/newer destination after restart: it must win.
    fake_nvs::data["nvs/host_wifi"]["state"].bytes = {5, 6};
    fake_nvs::fail_erase = true;
    assert(MigrateNetworkSettings() == ESP_FAIL);
    assert(fake_nvs::data["runtime_nvs/host_wifi"].contains("state"));
    fake_nvs::fail_erase = false;
    assert(MigrateNetworkSettings() == ESP_OK);
    assert(fake_nvs::data["nvs/host_wifi"]["state"].bytes == std::vector<uint8_t>({5, 6}));
    assert(fake_nvs::data["nvs/network"]["type"].bytes == cellular.bytes);
    assert(!fake_nvs::data["runtime_nvs/host_wifi"].contains("state"));
    assert(fake_nvs::data["runtime_nvs/host_wifi"].contains("unrelated"));
    assert(fake_nvs::data["runtime_nvs/network"].empty());
    assert(fake_nvs::data["runtime_nvs/game"]["save"].bytes == wifi.bytes);
    assert(MigrateNetworkSettings() == ESP_OK);
    fake_nvs::data["runtime_nvs/host_wifi"]["state"] = wifi;
    assert(MigrateNetworkSettings() == ESP_OK);
    assert(fake_nvs::data["runtime_nvs/host_wifi"].contains("state"));
}

void QuotasAndCleanup() {
    using namespace micropixel::runtime;
    fake_nvs::data.clear();
    static_assert(CONFIG_MICROPIXEL_KV_MAX_BYTES == 16384);
    static_assert(CONFIG_MICROPIXEL_KV_MAX_VALUE_BYTES == 4096);
    micropixel_aot_package_t package{};
    std::strcpy(reinterpret_cast<char*>(package.app_id), "test.long.app.identifier");
    char name[16]{};
    assert(AppStorageNamespace("test.long.app.identifier", name));
    // Existing FNV-1a mapping must survive the refactor.
    assert(std::strcmp(name, "mf3e44e3db9afdb") == 0);
    assert(!AppStorageNamespace("invalid/app", name));
    assert(!AppStorageNamespace("", name));
    assert(AppStorageNamespace("test.long.app.identifier", name));
    const std::string path = std::string("runtime_nvs/") + name;
    std::array<uint8_t, 4097> bytes{};
    {
        StorageService service(package);
        assert(service.valid());
        assert(service.SetBytes("first", 5, bytes.data(), 4096));
        assert(service.SetBytes("second", 6, bytes.data(), 4096));
        assert(service.SetBytes("third", 5, bytes.data(), 4096));
        assert(service.SetBytes("fourth", 6, bytes.data(), 4096));
        assert(!service.SetBool("extra", 5, true));
        assert(!service.SetBytes("first", 5, bytes.data(), 4097));
        assert(service.SetBytes("first", 5, bytes.data(), 4095));
        assert(service.SetBool("extra", 5, true));
        assert(service.GetBytes("second", 6, bytes.data(), bytes.size()).value() == 4096);
    }
    fake_nvs::data["runtime_nvs/other"]["score"] = {NVS_TYPE_U8, {9}};
    fake_nvs::data["nvs/host_wifi"]["state"] = {NVS_TYPE_BLOB, {7}};
    fake_nvs::fail_erase = true;
    assert(EraseAppStorage("test.long.app.identifier") == ESP_FAIL);
    assert(!fake_nvs::data[path].empty());
    fake_nvs::fail_erase = false;
    assert(EraseAppStorage("test.long.app.identifier") == ESP_OK);
    assert(fake_nvs::data[path].empty());
    assert(!fake_nvs::data["runtime_nvs/other"].empty());
    assert(!fake_nvs::data["nvs/host_wifi"].empty());
    assert(EraseAppStorage("missing") == ESP_OK);
    assert(!fake_nvs::data.contains("runtime_nvs/missing"));
    assert(EraseAppStorage("invalid/app") == ESP_ERR_INVALID_ARG);
    // Key-count limit is independent of byte quota and launch rate limits.
    for (int i = 0; i < 16; ++i) {
        StorageService service(package);
        std::string key = "k" + std::to_string(i);
        assert(service.SetBool(key.c_str(), key.size(), true));
    }
    StorageService service(package);
    assert(!service.SetBool("overflow", 8, true));
    assert(service.SetBool("k0", 2, false));
}
}  // namespace
int main() {
    UsageStatistics();
    Migration();
    QuotasAndCleanup();
    std::puts("private storage: usage statistics, migration retries, quota boundaries and isolated cleanup passed");
}
