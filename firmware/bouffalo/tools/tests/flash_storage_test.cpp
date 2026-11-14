// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <string_view>

#include "nvs.h"
#include "runtime/services/app_storage.hpp"
#include "storage.hpp"

namespace {
constexpr unsigned kSector = 4096;
using Partition = std::array<uint8_t, 8 * kSector>;
Partition appstore;
Partition media;
bool fail_read = false;

bool InBounds(unsigned offset, unsigned size) { return offset <= media.size() && size <= media.size() - offset; }
}  // namespace

extern "C" {
int bflb_mtd_open(const char* name, bflb_mtd_handle_t* handle, unsigned int) {
    if (std::strcmp(name, "appstore") == 0)
        *handle = &appstore;
    else if (std::strcmp(name, "media") == 0)
        *handle = &media;
    else
        return -1;
    return 0;
}

int bflb_mtd_close(bflb_mtd_handle_t) { return 0; }

int bflb_mtd_info(bflb_mtd_handle_t, bflb_mtd_info_t* info) {
    info->size = media.size();
    return 0;
}

int bflb_mtd_read(bflb_mtd_handle_t handle, unsigned offset, unsigned size, uint8_t* data) {
    if (!handle || fail_read || !InBounds(offset, size)) return -1;
    const auto& partition = *static_cast<Partition*>(handle);
    std::copy_n(partition.data() + offset, size, data);
    return 0;
}

int bflb_mtd_write(bflb_mtd_handle_t handle, unsigned offset, unsigned size, const uint8_t* data) {
    if (!handle || !InBounds(offset, size)) return -1;
    auto& partition = *static_cast<Partition*>(handle);
    for (unsigned i = 0; i < size; ++i) {
        if ((partition[offset + i] & data[i]) != data[i]) return -1;
    }
    std::copy_n(data, size, partition.data() + offset);
    return 0;
}

int bflb_mtd_erase(bflb_mtd_handle_t handle, unsigned offset, unsigned size) {
    if (!handle || !InBounds(offset, size) || offset % kSector != 0 || size % kSector != 0) return -1;
    auto& partition = *static_cast<Partition*>(handle);
    std::fill_n(partition.data() + offset, size, UINT8_MAX);
    return 0;
}
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string_view mode = argv[1];
    appstore.fill(UINT8_MAX);
    media.fill(UINT8_MAX);
    if (mode == "foreign") {
        media.back() = 0x42;
        const auto before = media;
        assert(!mpx_port::InitializeKv());
        assert(media == before);
        return 0;
    }
    if (mode == "read-failure") {
        fail_read = true;
        assert(!mpx_port::InitializeKv());
        assert(std::all_of(media.begin(), media.end(), [](auto byte) { return byte == UINT8_MAX; }));
        fail_read = false;
        assert(mpx_port::InitializeKv());
        return 0;
    }

    using Error = micropixel::device::BlockStorageError;
    mpx_port::FlashStorage flash;
    const std::array<uint8_t, 3> bundle{0x12, 0x34, 0x56};
    std::array<uint8_t, 3> data{};
    assert(flash.Read(0, data) == std::unexpected(Error::kUnavailable));
    assert(flash.Initialize());
    assert(flash.geometry().size_bytes == appstore.size());
    assert(flash.Read(UINT64_MAX, data) == std::unexpected(Error::kInvalidArgument));
    assert(flash.Program(appstore.size() - 1, bundle) == std::unexpected(Error::kInvalidArgument));
    assert(flash.Erase(1, kSector) == std::unexpected(Error::kInvalidArgument));
    fail_read = true;
    assert(flash.Read(0, data) == std::unexpected(Error::kIo));
    fail_read = false;

    constexpr auto app_id = "org.micropixel.flash-test";
    char name[NVS_NS_NAME_MAX_SIZE]{};
    assert(micropixel::runtime::AppStorageNamespace(app_id, name));
    nvs_handle_t handle{};
    if (mode == "write") {
        assert(mpx_port::InitializeKv());
        assert(flash.Erase(0, kSector));
        assert(flash.Program(13, bundle));
        assert(nvs_open_from_partition("runtime_nvs", name, NVS_READWRITE, &handle) == ESP_OK);
        assert(nvs_set_u32(handle, "counter", 2) == ESP_OK);
        assert(nvs_set_u32(handle, "counter", 17) == ESP_OK);
        assert(nvs_set_blob(handle, "data", bundle.data(), bundle.size()) == ESP_OK);
        assert(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);
        std::ofstream state(argv[2], std::ios::binary);
        state.write(reinterpret_cast<const char*>(appstore.data()), appstore.size());
        state.write(reinterpret_cast<const char*>(media.data()), media.size());
        state.close();
        assert(state);
    } else {
        assert(mode == "read");
        std::ifstream state(argv[2], std::ios::binary);
        state.read(reinterpret_cast<char*>(appstore.data()), appstore.size());
        state.read(reinterpret_cast<char*>(media.data()), media.size());
        assert(state);
        assert(flash.Read(13, data) && data == bundle);
        assert(mpx_port::InitializeKv());
        assert(nvs_open_from_partition("runtime_nvs", name, NVS_READONLY, &handle) == ESP_OK);
        uint32_t counter = 0;
        assert(nvs_get_u32(handle, "counter", &counter) == ESP_OK && counter == 17);
        size_t size = 0;
        assert(nvs_get_blob(handle, "data", nullptr, &size) == ESP_OK && size == bundle.size());
        data.fill(0);
        assert(nvs_get_blob(handle, "data", data.data(), &size) == ESP_OK && data == bundle);
        assert(nvs_set_u32(handle, "counter", 18) == ESP_ERR_INVALID_STATE);
        nvs_close(handle);
        assert(micropixel::runtime::EraseAppStorage(app_id) == ESP_OK);
        assert(nvs_open_from_partition("runtime_nvs", name, NVS_READONLY, &handle) == ESP_OK);
        assert(nvs_get_u32(handle, "counter", &counter) == ESP_ERR_NVS_NOT_FOUND);
        nvs_close(handle);
    }
}
