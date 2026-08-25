#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "esp_partition.h"
#include "psa/crypto.h"
#include "runtime/bundlefs/bundlefs.h"
#include "runtime/bundlefs/bundlefs_format.h"
#include "spi_flash_mmap.h"

namespace {

constexpr uint32_t kPartitionSize = 24U * 1024U * 1024U;
constexpr uint32_t kPartitionAddress = 0x800000U;

esp_partition_t test_partition{.address = kPartitionAddress, .size = kPartitionSize};
std::vector<uint8_t> test_flash(kPartitionSize, UINT8_MAX);
std::array<uint32_t, MICROPIXEL_BUNDLEFS_BANK_COUNT> bank_erase_counts{};
std::array<void*, 64U> mappings{};
bool fail_next_commit_marker = false;
uint32_t checks = 0U;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

bool IsErased(uint32_t offset, uint32_t size) {
    return std::all_of(test_flash.begin() + offset, test_flash.begin() + offset + size,
                       [](uint8_t value) { return value == UINT8_MAX; });
}

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0U; index < size; ++index) {
        crc ^= data[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ UINT32_MAX;
}

std::array<uint8_t, BUNDLEFS_SHA256_SIZE> Hash(const std::vector<uint8_t>& data) {
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> digest{};
    size_t digest_size = 0U;
    Check(psa_hash_compute(PSA_ALG_SHA_256, data.data(), data.size(), digest.data(), digest.size(), &digest_size) ==
                  PSA_SUCCESS &&
              digest_size == digest.size(),
          "test hash must succeed");
    return digest;
}

void Replace(std::string_view name, const std::vector<uint8_t>& data) {
    bundlefs_writer_t writer{};
    const std::string file_name(name);
    Check(bundlefs_begin_replace(file_name.c_str(), data.size(), &writer) == BUNDLEFS_OK, "begin replace must succeed");

    constexpr uint32_t kChunkSize = 3001U;
    for (uint32_t offset = 0U; offset < data.size();) {
        const uint32_t chunk = std::min<uint32_t>(kChunkSize, data.size() - offset);
        Check(bundlefs_write(&writer, data.data() + offset, chunk) == BUNDLEFS_OK, "streaming write must succeed");
        offset += chunk;
    }

    const auto digest = Hash(data);
    Check(bundlefs_commit(&writer, digest.data()) == BUNDLEFS_OK, "replace commit must succeed");
}

void ResetFlash() {
    std::fill(test_flash.begin(), test_flash.end(), UINT8_MAX);
    bank_erase_counts.fill(0U);
    fail_next_commit_marker = false;
    for (void*& mapping : mappings) {
        std::free(mapping);
        mapping = nullptr;
    }
}

void TestEmptyMountAndGeometry() {
    ResetFlash();
    Check(bundlefs_mount() == BUNDLEFS_OK, "erased partition must mount as an empty BundleFS");

    bundlefs_store_info_t info{};
    Check(bundlefs_get_store_info(&info) == BUNDLEFS_OK, "store info must load");
    Check(info.data_block_size == MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE, "data block size must be 64 KiB");
    Check(info.total_blocks == 383U && info.used_blocks == 0U, "24 MiB partition must expose 383 data blocks");
    Check(info.total_bytes == kPartitionSize, "store byte capacity must include the whole partition");
    Check(info.used_bytes == MICROPIXEL_BUNDLEFS_METADATA_SIZE, "empty store usage must include BundleFS metadata");
    Check(info.free_bytes == kPartitionSize - MICROPIXEL_BUNDLEFS_METADATA_SIZE,
          "empty store free bytes must include only allocatable data blocks");
    Check(!IsErased(0U, MICROPIXEL_BUNDLEFS_BANK_SIZE), "first mount must commit Bank 0");
    Check(IsErased(MICROPIXEL_BUNDLEFS_BANK_SIZE, 3U * MICROPIXEL_BUNDLEFS_BANK_SIZE),
          "first mount must leave the other three Banks erased");
}

void TestUnsupportedGeometryIsDistinctFromCorruption() {
    ResetFlash();
    Check(bundlefs_mount() == BUNDLEFS_OK, "mount before incompatible geometry test must succeed");

    constexpr uint32_t kDataBlockSizeOffset = 40U;
    constexpr uint32_t kChecksumOffset = MICROPIXEL_BUNDLEFS_BANK_SIZE - 8U;
    constexpr uint32_t kCommitOffset = MICROPIXEL_BUNDLEFS_BANK_SIZE - 4U;
    const uint32_t incompatible_block_size = 16U * 1024U;
    const uint32_t zero = 0U;
    const uint32_t erased = UINT32_MAX;
    std::memcpy(test_flash.data() + kDataBlockSizeOffset, &incompatible_block_size, sizeof(incompatible_block_size));
    std::memcpy(test_flash.data() + kChecksumOffset, &zero, sizeof(zero));
    std::memcpy(test_flash.data() + kCommitOffset, &erased, sizeof(erased));
    const uint32_t checksum = Crc32(test_flash.data(), MICROPIXEL_BUNDLEFS_BANK_SIZE);
    const uint32_t committed = 0x434f4d54U;
    std::memcpy(test_flash.data() + kChecksumOffset, &checksum, sizeof(checksum));
    std::memcpy(test_flash.data() + kCommitOffset, &committed, sizeof(committed));

    Check(bundlefs_mount() == BUNDLEFS_ERR_UNSUPPORTED_FORMAT,
          "valid Catalog with an incompatible data block size must report unsupported format");
}

void TestReadMapReplaceAndRemove() {
    ResetFlash();
    Check(bundlefs_mount() == BUNDLEFS_OK, "mount before file operations must succeed");

    std::vector<uint8_t> first(MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE + 97U);
    for (uint32_t index = 0U; index < first.size(); ++index) {
        first[index] = static_cast<uint8_t>((index * 13U) ^ (index >> 9U));
    }
    Replace("demo", first);

    bundlefs_store_info_t populated_info{};
    Check(bundlefs_get_store_info(&populated_info) == BUNDLEFS_OK && populated_info.used_blocks == 2U,
          "installed file must occupy two data blocks");
    Check(populated_info.used_bytes == MICROPIXEL_BUNDLEFS_METADATA_SIZE + 2U * MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE,
          "store usage must include metadata and allocated Bundle blocks");

    bundlefs_file_t file{};
    Check(bundlefs_open("demo", &file) == BUNDLEFS_OK, "installed file must open");
    std::array<uint8_t, 151U> cross_block{};
    const uint32_t read_offset = MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE - 73U;
    Check(bundlefs_read(&file, read_offset, cross_block.data(), cross_block.size()) == BUNDLEFS_OK,
          "cross-block read must succeed");
    Check(std::equal(cross_block.begin(), cross_block.end(), first.begin() + read_offset),
          "cross-block read must preserve logical ordering");

    bundlefs_mapping_t mapping{};
    Check(bundlefs_mmap(&file, read_offset, cross_block.size(), &mapping) == BUNDLEFS_OK,
          "cross-block mmap must succeed");
    Check(mapping.size == cross_block.size() &&
              std::equal(mapping.data, mapping.data + mapping.size, first.begin() + read_offset),
          "mmap must expose contiguous virtual bytes");
    bundlefs_munmap(&mapping);

    std::vector<uint8_t> second(4000U, 0x5aU);
    Replace("demo", second);
    Check(bundlefs_open("demo", &file) == BUNDLEFS_OK, "replacement file must open");
    std::vector<uint8_t> readback(second.size());
    Check(bundlefs_read(&file, 0U, readback.data(), readback.size()) == BUNDLEFS_OK && readback == second,
          "replacement must atomically expose new bytes");

    Check(bundlefs_remove("demo") == BUNDLEFS_OK, "remove must commit a new Catalog");
    Check(bundlefs_open("demo", &file) == BUNDLEFS_ERR_NOT_FOUND, "removed file must disappear");
    bundlefs_store_info_t info{};
    Check(bundlefs_get_store_info(&info) == BUNDLEFS_OK && info.used_blocks == 0U,
          "removed file blocks must become reusable");
}

void TestFourBankRingAndInterruptedCommit() {
    ResetFlash();
    Check(bundlefs_mount() == BUNDLEFS_OK, "mount before ring test must succeed");

    for (uint8_t generation = 0U; generation < 4U; ++generation) {
        Replace("ring", std::vector<uint8_t>(1024U, static_cast<uint8_t>(0x20U + generation)));
    }
    Check(bank_erase_counts[0] == 2U, "fifth Catalog generation must wrap and erase Bank 0 once more");
    Check(bank_erase_counts[1] == 1U && bank_erase_counts[2] == 1U && bank_erase_counts[3] == 1U,
          "Catalog generations must rotate through all four Banks");

    bundlefs_writer_t writer{};
    std::vector<uint8_t> interrupted(2048U, 0xe7U);
    Check(bundlefs_begin_replace("ring", interrupted.size(), &writer) == BUNDLEFS_OK, "interrupted replace must begin");
    Check(bundlefs_write(&writer, interrupted.data(), interrupted.size()) == BUNDLEFS_OK,
          "interrupted replace data must write");
    const auto digest = Hash(interrupted);
    fail_next_commit_marker = true;
    Check(bundlefs_commit(&writer, digest.data()) == BUNDLEFS_ERR_COMMIT, "commit marker failure must be reported");
    bundlefs_abort(&writer);

    Check(bundlefs_mount() == BUNDLEFS_OK, "mount must fall back after an interrupted Bank commit");
    bundlefs_file_t file{};
    Check(bundlefs_open("ring", &file) == BUNDLEFS_OK, "previous Catalog must remain visible");
    std::array<uint8_t, 16U> bytes{};
    Check(bundlefs_read(&file, 0U, bytes.data(), bytes.size()) == BUNDLEFS_OK &&
              std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0x23U; }),
          "interrupted commit must not expose staged data");
}

}  // namespace

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char* label) {
    return type == ESP_PARTITION_TYPE_DATA && subtype == 0x40 && label != nullptr &&
                   std::strcmp(label, "app_store") == 0
               ? &test_partition
               : nullptr;
}

esp_err_t esp_partition_read(const esp_partition_t* partition, size_t source_offset, void* destination, size_t size) {
    if (partition != &test_partition || destination == nullptr || source_offset > test_flash.size() ||
        size > test_flash.size() - source_offset) {
        return ESP_FAIL;
    }
    std::memcpy(destination, test_flash.data() + source_offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t* partition, size_t destination_offset, const void* source,
                              size_t size) {
    if (partition != &test_partition || source == nullptr || destination_offset > test_flash.size() ||
        size > test_flash.size() - destination_offset) {
        return ESP_FAIL;
    }
    if (fail_next_commit_marker &&
        destination_offset < MICROPIXEL_BUNDLEFS_BANK_COUNT * MICROPIXEL_BUNDLEFS_BANK_SIZE &&
        size == sizeof(uint32_t)) {
        fail_next_commit_marker = false;
        return ESP_FAIL;
    }
    const auto* bytes = static_cast<const uint8_t*>(source);
    for (size_t index = 0U; index < size; ++index) {
        if ((test_flash[destination_offset + index] & bytes[index]) != bytes[index]) {
            return ESP_FAIL;
        }
        test_flash[destination_offset + index] = bytes[index];
    }
    return ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t* partition, size_t start_address, size_t size) {
    if (partition != &test_partition || start_address > test_flash.size() || size > test_flash.size() - start_address ||
        (start_address % 4096U) != 0U || (size % 4096U) != 0U) {
        return ESP_FAIL;
    }
    if (size == MICROPIXEL_BUNDLEFS_BANK_SIZE && start_address < MICROPIXEL_BUNDLEFS_BANK_COUNT * size) {
        ++bank_erase_counts[start_address / size];
    }
    std::fill(test_flash.begin() + start_address, test_flash.begin() + start_address + size, UINT8_MAX);
    return ESP_OK;
}

esp_err_t spi_flash_mmap_pages(const int* pages, size_t page_count, uint32_t memory, const void** mapped_pointer,
                               spi_flash_mmap_handle_t* handle) {
    if (pages == nullptr || page_count == 0U || memory != SPI_FLASH_MMAP_FLAG_DATA || mapped_pointer == nullptr ||
        handle == nullptr) {
        return ESP_FAIL;
    }
    uint8_t* copy = static_cast<uint8_t*>(std::malloc(page_count * SPI_FLASH_MMU_PAGE_SIZE));
    if (copy == nullptr) {
        return ESP_FAIL;
    }
    for (size_t index = 0U; index < page_count; ++index) {
        const uint32_t physical = static_cast<uint32_t>(pages[index]) * SPI_FLASH_MMU_PAGE_SIZE;
        if (physical < kPartitionAddress || physical - kPartitionAddress > test_flash.size() ||
            SPI_FLASH_MMU_PAGE_SIZE > test_flash.size() - (physical - kPartitionAddress)) {
            std::free(copy);
            return ESP_FAIL;
        }
        std::memcpy(copy + index * SPI_FLASH_MMU_PAGE_SIZE, test_flash.data() + physical - kPartitionAddress,
                    SPI_FLASH_MMU_PAGE_SIZE);
    }
    for (uint32_t index = 1U; index < mappings.size(); ++index) {
        if (mappings[index] == nullptr) {
            mappings[index] = copy;
            *mapped_pointer = copy;
            *handle = index;
            return ESP_OK;
        }
    }
    std::free(copy);
    return ESP_FAIL;
}

void spi_flash_munmap(spi_flash_mmap_handle_t handle) {
    if (handle < mappings.size()) {
        std::free(mappings[handle]);
        mappings[handle] = nullptr;
    }
}

int main() {
    TestEmptyMountAndGeometry();
    TestUnsupportedGeometryIsDistinctFromCorruption();
    TestReadMapReplaceAndRemove();
    TestFourBankRingAndInterruptedCommit();
    std::printf("BundleFS tests passed (%" PRIu32 " checks).\n", checks);
    return 0;
}
