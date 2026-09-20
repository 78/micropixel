// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "platform/storage/spi_nand_block_storage.hpp"

namespace {
using micropixel::platform::storage::SpiNandBlockStorage;
constexpr uint32_t kSectorSize = 2048;
std::array<std::array<uint8_t, kSectorSize>, 8> media;
std::array<unsigned, 8> reads;
bool fail_read{}, fail_write{}, fail_trim{}, fail_sync{};
SpiNandBlockStorage::Config Config() {
    SpiNandBlockStorage::Config config;
    config.clock = 1;
    config.data_in = 2;
    config.data_out = 3;
    config.chip_select = 4;
    config.clock_hz = 1000000;
    return config;
}

void CheckRead(SpiNandBlockStorage& storage, uint32_t offset, uint32_t size) {
    std::vector<uint8_t> data(size);
    assert(storage.Read(offset, data));
    for (uint32_t i = 0; i < size; ++i)
        assert(data[i] == media[(offset + i) / kSectorSize][(offset + i) % kSectorSize]);
}

void CacheAndStreamingReads() {
    SpiNandBlockStorage storage;
    assert(storage.Initialize(Config()) == ESP_OK);
    CheckRead(storage, 64, 8);
    CheckRead(storage, 72, 25);
    CheckRead(storage, 0, kSectorSize);  // whole-sector hits must also use the cached bytes
    assert(reads[0] == 1);
    CheckRead(storage, kSectorSize - 4, 8);
    assert(reads[0] == 1 && reads[1] == 1);
    storage.Shutdown();
    assert(storage.Initialize(Config()) == ESP_OK);
    reads.fill(0);
    // Resource starts 64 bytes into a sector; adjacent 4 KiB refills share one sector.
    for (uint32_t offset = 64; offset < 3 * 4096; offset += 4096) CheckRead(storage, offset, 4096);
    for (unsigned i = 0; i < 7; ++i) assert(reads[i] == 1);
    assert(reads[7] == 0);
}

void CacheInvalidation() {
    SpiNandBlockStorage storage;
    assert(storage.Initialize(Config()) == ESP_OK);
    reads.fill(0);
    CheckRead(storage, 0, 1);
    const std::array<uint8_t, 2> bytes{0x31, 0x42};
    const uint8_t neighbor = media[0][12];
    assert(storage.Program(10, bytes));
    assert(media[0][10] == bytes[0] && media[0][11] == bytes[1] && media[0][12] == neighbor);
    assert(reads[0] == 1);  // read-modify-write reused the cache
    CheckRead(storage, 10, 3);
    assert(reads[0] == 2);

    fail_write = true;
    assert(!storage.Program(10, bytes));  // driver modifies some bytes, then reports failure
    fail_write = false;
    CheckRead(storage, 10, 3);
    assert(reads[0] == 3);
    std::array<uint8_t, kSectorSize> full;
    full.fill(0x26);
    assert(storage.Program(0, full));
    CheckRead(storage, 0, 1);
    assert(reads[0] == 4);
    fail_write = true;
    assert(!storage.Program(0, full));
    fail_write = false;
    CheckRead(storage, 0, 1);
    assert(reads[0] == 5);

    assert(storage.Erase(0, kSectorSize));
    CheckRead(storage, 0, 1);
    assert(reads[0] == 6 && media[0][0] == 0xff);
    fail_trim = true;
    assert(!storage.Erase(0, kSectorSize));
    fail_trim = false;
    CheckRead(storage, 0, 1);
    assert(reads[0] == 7);

    fail_read = true;
    std::array<uint8_t, 1> output;
    assert(!storage.Read(kSectorSize, output));
    fail_read = false;
    CheckRead(storage, 0, 1);  // failed refill overwrote the old cache: it must not remain valid
    assert(reads[0] == 8);
    CheckRead(storage, kSectorSize, 1);
    assert(reads[1] == 2);
    assert(storage.Sync());
    fail_sync = true;
    assert(!storage.Sync());
    fail_sync = false;
    storage.Shutdown();
    assert(!storage.Read(0, output));
    assert(storage.Initialize(Config()) == ESP_OK);
    CheckRead(storage, kSectorSize, 1);
    assert(reads[1] == 3);
}
}  // namespace

esp_err_t spi_nand_flash_init_device(const spi_nand_flash_config_t*, spi_nand_flash_device_t** handle) {
    static spi_nand_flash_device_t device;
    *handle = &device;
    return ESP_OK;
}
esp_err_t spi_nand_flash_deinit_device(spi_nand_flash_device_t*) { return ESP_OK; }
esp_err_t spi_nand_flash_get_sector_size(spi_nand_flash_device_t*, uint32_t* size) {
    *size = kSectorSize;
    return ESP_OK;
}
esp_err_t spi_nand_flash_get_capacity(spi_nand_flash_device_t*, uint32_t* count) {
    *count = media.size();
    return ESP_OK;
}
esp_err_t spi_nand_flash_read_sector(spi_nand_flash_device_t*, uint8_t* output, uint32_t sector) {
    assert(sector < media.size());
    ++reads[sector];
    if (fail_read) {
        output[0] = 0x55;
        return ESP_FAIL;
    }
    std::memcpy(output, media[sector].data(), kSectorSize);
    return ESP_OK;
}
esp_err_t spi_nand_flash_write_sector(spi_nand_flash_device_t*, const uint8_t* input, uint32_t sector) {
    assert(sector < media.size());
    if (fail_write) {
        media[sector][10] = 0x99;
        return ESP_FAIL;
    }
    std::memcpy(media[sector].data(), input, kSectorSize);
    return ESP_OK;
}
esp_err_t spi_nand_flash_trim(spi_nand_flash_device_t*, uint32_t sector) {
    assert(sector < media.size());
    if (fail_trim) {
        media[sector][0] = 0x12;
        return ESP_FAIL;
    }
    media[sector].fill(0xff);
    return ESP_OK;
}
esp_err_t spi_nand_flash_sync(spi_nand_flash_device_t*) { return fail_sync ? ESP_FAIL : ESP_OK; }

int main() {
    for (size_t sector = 0; sector < media.size(); ++sector)
        for (size_t i = 0; i < kSectorSize; ++i) media[sector][i] = static_cast<uint8_t>(sector + i);
    CacheAndStreamingReads();
    CacheInvalidation();
    std::puts("spi_nand_block_storage tests passed");
}
