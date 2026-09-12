#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "esp_partition.h"
#include "platform/storage/partition_block_storage.hpp"
#include "psa/crypto.h"
#include "runtime/bundlefs/bundlefs.hpp"
#include "runtime/bundlefs/bundlefs_format.h"
#include "spi_flash_mmap.h"

namespace {

using micropixel::device::BlockStorageGeometry;
using micropixel::runtime::BundleFs;
using micropixel::runtime::BundleFsGeometry;

constexpr uint32_t kKiB = 1024U;
constexpr uint32_t kMiB = 1024U * kKiB;
constexpr uint32_t kPartitionSize = 24U * kMiB;
constexpr uint32_t kPartitionAddress = 0x800000U;
constexpr uint32_t kCommitMarker = 0x434f4d54U;
constexpr uint32_t kHeaderDataBlockSizeOffset = 36U;
constexpr uint32_t kHeaderFormatVersionOffset = 8U;

esp_partition_t test_partition{.address = kPartitionAddress, .size = kPartitionSize};
std::vector<uint8_t> test_flash(kPartitionSize, UINT8_MAX);
std::array<uint32_t, MICROPIXEL_BUNDLEFS_BANK_COUNT> bank_erase_counts{};
std::array<void*, 64U> mappings{};
bool fail_next_commit_marker = false;
uint32_t checks = 0U;
// The store under test; NOR tests point it at a BundleFs over the esp_partition stubs.
micropixel::runtime::BundleStore* store = nullptr;

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

uint32_t BlocksFor(uint32_t block_size, uint32_t size) { return (size + block_size - 1U) / block_size; }

std::array<uint8_t, BUNDLEFS_SHA256_SIZE> Hash(const std::vector<uint8_t>& data) {
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> digest{};
    size_t digest_size = 0U;
    Check(psa_hash_compute(PSA_ALG_SHA_256, data.data(), data.size(), digest.data(), digest.size(), &digest_size) ==
                  PSA_SUCCESS &&
              digest_size == digest.size(),
          "test hash must succeed");
    return digest;
}

std::vector<uint8_t> Pattern(uint32_t size, uint32_t seed) {
    std::vector<uint8_t> data(size);
    for (uint32_t index = 0U; index < size; ++index) {
        data[index] = static_cast<uint8_t>((index * (13U + seed)) ^ (index >> 9U) ^ seed);
    }
    return data;
}

void Replace(std::string_view name, const std::vector<uint8_t>& data) {
    bundlefs_writer_t writer{};
    const std::string file_name(name);
    Check(store->BeginReplace(file_name.c_str(), data.size(), writer) == BUNDLEFS_OK, "begin replace must succeed");

    constexpr uint32_t kChunkSize = 3001U;
    for (uint32_t offset = 0U; offset < data.size();) {
        const uint32_t chunk = std::min<uint32_t>(kChunkSize, data.size() - offset);
        Check(store->Write(writer, data.data() + offset, chunk) == BUNDLEFS_OK, "streaming write must succeed");
        offset += chunk;
    }

    const auto digest = Hash(data);
    Check(store->Commit(writer, digest.data()) == BUNDLEFS_OK, "replace commit must succeed");
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

// Writes one committed legacy (v1 or v2) Catalog into Bank 0 holding a single
// 1 KiB file named "legacy" in data block 0. Both legacy layouts share the
// 64-byte header and 112-byte entries; they differ in bank size and entry count.
void SeedLegacyCatalog(uint16_t format_version, uint32_t bank_size, uint32_t max_files) {
    constexpr uint32_t kEntrySize = MICROPIXEL_BUNDLEFS_LEGACY_ENTRY_SIZE;
    constexpr uint32_t kEntriesOffset = 64U;
    constexpr uint32_t kBlockSize = MICROPIXEL_BUNDLEFS_LEGACY_DATA_BLOCK_SIZE;
    const uint32_t block_map_offset = kEntriesOffset + max_files * kEntrySize;
    const uint32_t checksum_offset = bank_size - 8U;
    const uint32_t commit_offset = bank_size - 4U;
    std::vector<uint8_t> bank(bank_size, 0U);
    const std::array<uint8_t, 8U> magic{'M', 'P', 'B', 'U', 'N', 'D', 'L', 'E'};
    std::memcpy(bank.data(), magic.data(), magic.size());
    const auto put16 = [&](uint32_t offset, uint16_t value) {
        std::memcpy(bank.data() + offset, &value, sizeof(value));
    };
    const auto put32 = [&](uint32_t offset, uint32_t value) {
        std::memcpy(bank.data() + offset, &value, sizeof(value));
    };
    const auto put64 = [&](uint32_t offset, uint64_t value) {
        std::memcpy(bank.data() + offset, &value, sizeof(value));
    };
    const uint16_t data_blocks = static_cast<uint16_t>(std::min<uint32_t>(
        (kPartitionSize - MICROPIXEL_BUNDLEFS_DATA_OFFSET) / kBlockSize, MICROPIXEL_BUNDLEFS_LEGACY_MAX_DATA_BLOCKS));
    put16(8U, format_version);
    put16(10U, kEntriesOffset);
    put32(12U, bank_size);
    put64(16U, 9U);
    put16(24U, 0U);
    put16(26U, MICROPIXEL_BUNDLEFS_BANK_COUNT);
    put32(28U, bank_size);
    put32(32U, MICROPIXEL_BUNDLEFS_METADATA_SIZE);
    put32(36U, MICROPIXEL_BUNDLEFS_DATA_OFFSET);
    put32(40U, kBlockSize);
    put32(44U, kPartitionSize);
    put16(48U, data_blocks);
    put16(50U, 1U);
    put16(52U, 1U);
    put16(54U, 1U);
    put32(60U, max_files * kEntrySize + MICROPIXEL_BUNDLEFS_LEGACY_MAX_DATA_BLOCKS * sizeof(uint16_t));
    std::memcpy(bank.data() + kEntriesOffset, "legacy", 6U);
    put32(kEntriesOffset + 68U, 1024U);
    put32(kEntriesOffset + 72U, 0x12345678U);
    put16(kEntriesOffset + 76U, 0U);
    put16(kEntriesOffset + 78U, 1U);
    put16(block_map_offset, 0U);
    put32(checksum_offset, 0U);
    put32(commit_offset, UINT32_MAX);
    put32(checksum_offset, Crc32(bank.data(), bank.size()));
    put32(commit_offset, kCommitMarker);
    std::copy(bank.begin(), bank.end(), test_flash.begin());
    std::fill_n(test_flash.begin() + MICROPIXEL_BUNDLEFS_DATA_OFFSET, 1024U, 0x6cU);
}

std::vector<std::string> ListFiles(micropixel::runtime::BundleStore& target) {
    std::array<bundlefs_file_info_t, BUNDLEFS_MAX_FILES> files{};
    uint32_t count = 0U;
    Check(target.List(files.data(), files.size(), count) == BUNDLEFS_OK, "file list must load");
    std::vector<std::string> names;
    names.reserve(count);
    for (uint32_t index = 0U; index < count; ++index) {
        names.emplace_back(files[index].name);
    }
    return names;
}

std::vector<std::string> ListFiles() { return ListFiles(*store); }

uint16_t FormatVersionAt(uint32_t offset) {
    uint16_t version = 0U;
    std::memcpy(&version, test_flash.data() + offset + kHeaderFormatVersionOffset, sizeof(version));
    return version;
}

void TestEmptyMountAndGeometry(const BundleFsGeometry& geometry) {
    ResetFlash();
    Check(store->Mount() == BUNDLEFS_OK, "erased partition must mount as an empty BundleFS");

    bundlefs_store_info_t info{};
    Check(store->GetStoreInfo(info) == BUNDLEFS_OK, "store info must load");
    Check(info.data_block_size == geometry.data_block_size, "data block size must match the planned geometry");
    Check(info.total_blocks == geometry.data_block_count && info.used_blocks == 0U,
          "data block count must cover the whole partition after the metadata area");
    Check(info.total_bytes == kPartitionSize, "store byte capacity must include the whole partition");
    const uint64_t allocatable = static_cast<uint64_t>(geometry.data_block_count) * geometry.data_block_size;
    Check(info.used_bytes == kPartitionSize - allocatable && info.free_bytes == allocatable,
          "empty store usage must be the metadata area and free bytes the allocatable data blocks");
    Check(!IsErased(0U, geometry.bank_size), "first mount must commit Bank 0");
    Check(IsErased(geometry.bank_size, 3U * geometry.bank_size), "first mount must leave the other three Banks erased");
    Check(FormatVersionAt(0U) == MICROPIXEL_BUNDLEFS_FORMAT_VERSION, "fresh media must be formatted as v3");
}

void TestUnsupportedGeometryIsDistinctFromCorruption(const BundleFsGeometry& geometry) {
    ResetFlash();
    Check(store->Mount() == BUNDLEFS_OK, "mount before incompatible geometry test must succeed");

    // Rewrite the committed record with another block size and a valid checksum.
    const uint32_t checksum_offset = geometry.record_size - 8U;
    const uint32_t commit_offset = geometry.record_size - 4U;
    const uint32_t incompatible_block_size = geometry.data_block_size * 2U;
    const uint32_t zero = 0U;
    const uint32_t erased = UINT32_MAX;
    std::memcpy(test_flash.data() + kHeaderDataBlockSizeOffset, &incompatible_block_size,
                sizeof(incompatible_block_size));
    std::memcpy(test_flash.data() + checksum_offset, &zero, sizeof(zero));
    std::memcpy(test_flash.data() + commit_offset, &erased, sizeof(erased));
    const uint32_t checksum = Crc32(test_flash.data(), geometry.record_size);
    std::memcpy(test_flash.data() + checksum_offset, &checksum, sizeof(checksum));
    std::memcpy(test_flash.data() + commit_offset, &kCommitMarker, sizeof(kCommitMarker));

    Check(store->Mount() == BUNDLEFS_ERR_UNSUPPORTED_FORMAT,
          "valid Catalog with an incompatible data block size must report unsupported format");
}

void TestReadMapReplaceAndRemove(const BundleFsGeometry& geometry) {
    ResetFlash();
    Check(store->Mount() == BUNDLEFS_OK, "mount before file operations must succeed");

    const uint32_t block_size = geometry.data_block_size;
    const std::vector<uint8_t> first = Pattern(block_size + 97U, 1U);
    Replace("demo", first);

    const auto first_digest = Hash(first);
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> listed_digest{};
    Check(store->GetFileSha256("demo", listed_digest.data()) == BUNDLEFS_OK && listed_digest == first_digest,
          "digest lookup must expose the committed SHA-256 without expanding every file record");
    std::array<bundlefs_file_info_t, BUNDLEFS_MAX_FILES> listed{};
    uint32_t listed_count = 0U;
    Check(store->List(listed.data(), listed.size(), listed_count) == BUNDLEFS_OK && listed_count == 1U &&
              std::equal(first_digest.begin(), first_digest.end(), listed[0].sha256),
          "file list must expose the committed SHA-256");

    bundlefs_store_info_t populated_info{};
    Check(store->GetStoreInfo(populated_info) == BUNDLEFS_OK && populated_info.used_blocks == 2U,
          "installed file must occupy two data blocks");
    Check(populated_info.used_bytes == kPartitionSize - populated_info.free_bytes &&
              populated_info.free_bytes == static_cast<uint64_t>(geometry.data_block_count - 2U) * block_size,
          "store usage must include metadata and allocated Bundle blocks");

    bundlefs_file_t file{};
    Check(store->Open("demo", file) == BUNDLEFS_OK, "installed file must open");
    std::array<uint8_t, 151U> cross_block{};
    const uint32_t read_offset = block_size - 73U;
    Check(store->Read(file, read_offset, cross_block.data(), cross_block.size()) == BUNDLEFS_OK,
          "cross-block read must succeed");
    Check(std::equal(cross_block.begin(), cross_block.end(), first.begin() + read_offset),
          "cross-block read must preserve logical ordering");

    bundlefs_mapping_t mapping{};
    Check(store->Map(file, read_offset, cross_block.size(), mapping) == BUNDLEFS_OK, "cross-block mmap must succeed");
    Check(mapping.size == cross_block.size() &&
              std::equal(mapping.data, mapping.data + mapping.size, first.begin() + read_offset),
          "mmap must expose contiguous virtual bytes");
    store->Unmap(mapping);

    std::vector<uint8_t> second(4000U, 0x5aU);
    Replace("demo", second);
    std::array<uint8_t, 16U> stale{};
    Check(store->Read(file, 0U, stale.data(), stale.size()) == BUNDLEFS_ERR_NOT_FOUND,
          "a handle to the replaced file must not read the new content");
    Check(store->Open("demo", file) == BUNDLEFS_OK, "replacement file must open");
    std::vector<uint8_t> readback(second.size());
    Check(store->Read(file, 0U, readback.data(), readback.size()) == BUNDLEFS_OK && readback == second,
          "replacement must atomically expose new bytes");

    Check(store->Remove("demo") == BUNDLEFS_OK, "remove must commit a new Catalog");
    Check(store->Open("demo", file) == BUNDLEFS_ERR_NOT_FOUND, "removed file must disappear");
    bundlefs_store_info_t info{};
    Check(store->GetStoreInfo(info) == BUNDLEFS_OK && info.used_blocks == 0U,
          "removed file blocks must become reusable");
}

void TestStagedReadsAndAbort() {
    ResetFlash();
    Check(store->Mount() == BUNDLEFS_OK, "mount before staged read test must succeed");
    Replace("keep", std::vector<uint8_t>(512U, 0x11U));

    const std::vector<uint8_t> staged_bytes = Pattern(9000U, 5U);
    bundlefs_writer_t writer{};
    Check(store->BeginReplace("keep", staged_bytes.size(), writer) == BUNDLEFS_OK, "staged replace must begin");
    bundlefs_file_t staged{};
    Check(store->OpenStaged(writer, staged) == BUNDLEFS_ERR_INVALID_ARGUMENT,
          "a staged file must not open before all bytes are written");
    Check(store->Write(writer, staged_bytes.data(), staged_bytes.size()) == BUNDLEFS_OK, "staged bytes must write");
    Check(store->OpenStaged(writer, staged) == BUNDLEFS_OK, "a fully written staged file must open");
    std::vector<uint8_t> readback(staged_bytes.size());
    Check(store->Read(staged, 0U, readback.data(), readback.size()) == BUNDLEFS_OK && readback == staged_bytes,
          "staged bytes must read back before commit (install-time validation)");
    bundlefs_file_t committed{};
    std::array<uint8_t, 4U> head{};
    Check(store->Open("keep", committed) == BUNDLEFS_OK &&
              store->Read(committed, 0U, head.data(), head.size()) == BUNDLEFS_OK && head[0] == 0x11U,
          "the committed file must stay readable while its replacement is staged");
    bundlefs_writer_t second{};
    Check(store->BeginReplace("other", 1U, second) == BUNDLEFS_ERR_BUSY, "only one writer may be active");
    store->Abort(writer);
    Check(store->Read(staged, 0U, readback.data(), 16U) == BUNDLEFS_ERR_NOT_FOUND,
          "an aborted staged handle must stop reading");
    Check(ListFiles() == std::vector<std::string>({"keep"}), "abort must leave the Catalog unchanged");
}

void TestNewestInstallIsFirstAndUpdateKeepsPosition() {
    ResetFlash();
    Check(store->Mount() == BUNDLEFS_OK, "mount before Catalog ordering test must succeed");

    Replace("blocks", std::vector<uint8_t>(1024U, 0x31U));
    Replace("snake", std::vector<uint8_t>(1024U, 0x42U));
    Replace("demo", std::vector<uint8_t>(1024U, 0x53U));
    Check(ListFiles() == std::vector<std::string>({"demo", "snake", "blocks"}),
          "new installs must be inserted at Catalog index zero");

    Replace("snake", std::vector<uint8_t>(2048U, 0x64U));
    Check(ListFiles() == std::vector<std::string>({"demo", "snake", "blocks"}),
          "updating an existing App must preserve its Catalog position");

    Check(store->Remove("snake") == BUNDLEFS_OK, "middle App must uninstall");
    Check(ListFiles() == std::vector<std::string>({"demo", "blocks"}),
          "uninstall must preserve the remaining relative order");

    Replace("snake", std::vector<uint8_t>(1024U, 0x75U));
    Check(ListFiles() == std::vector<std::string>({"snake", "demo", "blocks"}),
          "reinstalling a removed App must make it the newest entry");
}

void TestLegacyMigration(micropixel::device::BlockStorage& nor_storage, uint16_t format_version, uint32_t bank_size,
                         uint32_t max_files) {
    ResetFlash();
    SeedLegacyCatalog(format_version, bank_size, max_files);
    BundleFs legacy_store(nor_storage);
    Check(legacy_store.Mount() == BUNDLEFS_OK && ListFiles(legacy_store) == std::vector<std::string>({"legacy"}),
          "a valid legacy Catalog must remain readable");
    Check(legacy_store.data_block_size() == MICROPIXEL_BUNDLEFS_LEGACY_DATA_BLOCK_SIZE,
          "the block size must be taken from the committed Catalog, not from the medium's default");
    bundlefs_file_t file{};
    std::array<uint8_t, 8U> bytes{};
    Check(legacy_store.Open("legacy", file) == BUNDLEFS_OK &&
              legacy_store.Read(file, 0U, bytes.data(), bytes.size()) == BUNDLEFS_OK &&
              std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0x6cU; }),
          "legacy file bytes must resolve through the imported block map");
    Check(IsErased(MICROPIXEL_BUNDLEFS_V2_BANK_SIZE, MICROPIXEL_BUNDLEFS_V2_BANK_SIZE),
          "reading a legacy Catalog must not write to the medium");

    store = &legacy_store;
    Replace("migrated", std::vector<uint8_t>(1024U, 0x51U));
    Check(ListFiles() == std::vector<std::string>({"migrated", "legacy"}),
          "the first v3 commit must preserve and reorder imported legacy entries");
    Check(FormatVersionAt(MICROPIXEL_BUNDLEFS_V2_BANK_SIZE) == MICROPIXEL_BUNDLEFS_FORMAT_VERSION,
          "legacy migration must commit v3 into Bank 1 of the 16 KiB ring");
    Check(!IsErased(0U, MICROPIXEL_BUNDLEFS_V1_BANK_SIZE), "legacy Bank 0 must survive the migration commit");

    BundleFs remounted(nor_storage);
    Check(remounted.Mount() == BUNDLEFS_OK &&
              ListFiles(remounted) == std::vector<std::string>({"migrated", "legacy"}) &&
              remounted.data_block_size() == MICROPIXEL_BUNDLEFS_LEGACY_DATA_BLOCK_SIZE,
          "a fresh instance must prefer the migrated v3 record and keep the legacy block size");
    Check(remounted.Open("legacy", file) == BUNDLEFS_OK &&
              remounted.Read(file, 0U, bytes.data(), bytes.size()) == BUNDLEFS_OK && bytes[0] == 0x6cU,
          "legacy data blocks must stay addressable after migration");
}

void TestFiftyFiles() {
    ResetFlash();
    Check(store->Mount() == BUNDLEFS_OK, "mount before fifty-file capacity test must succeed");
    for (uint32_t index = 0U; index < BUNDLEFS_MAX_FILES; ++index) {
        char name[16]{};
        std::snprintf(name, sizeof(name), "app%02" PRIu32, index);
        Replace(name, std::vector<uint8_t>(1U, static_cast<uint8_t>(index)));
    }
    const auto files = ListFiles();
    Check(files.size() == BUNDLEFS_MAX_FILES && files.front() == "app49" && files.back() == "app00",
          "BundleFS must retain fifty files in newest-first order");
    bundlefs_writer_t writer{};
    Check(store->BeginReplace("overflow", 1U, writer) == BUNDLEFS_ERR_TOO_MANY_FILES,
          "a fifty-first file must fail without disturbing the Catalog");
}

void TestFourBankRingAndInterruptedCommit() {
    ResetFlash();
    Check(store->Mount() == BUNDLEFS_OK, "mount before ring test must succeed");

    for (uint8_t generation = 0U; generation < 4U; ++generation) {
        Replace("ring", std::vector<uint8_t>(1024U, static_cast<uint8_t>(0x20U + generation)));
    }
    Check(bank_erase_counts[0] == 2U, "fifth Catalog generation must wrap and erase Bank 0 once more");
    Check(bank_erase_counts[1] == 1U && bank_erase_counts[2] == 1U && bank_erase_counts[3] == 1U,
          "Catalog generations must rotate through all four Banks");

    bundlefs_writer_t writer{};
    std::vector<uint8_t> interrupted(2048U, 0xe7U);
    Check(store->BeginReplace("ring", interrupted.size(), writer) == BUNDLEFS_OK, "interrupted replace must begin");
    Check(store->Write(writer, interrupted.data(), interrupted.size()) == BUNDLEFS_OK,
          "interrupted replace data must write");
    const auto digest = Hash(interrupted);
    fail_next_commit_marker = true;
    Check(store->Commit(writer, digest.data()) == BUNDLEFS_ERR_COMMIT, "commit marker failure must be reported");
    store->Abort(writer);

    Check(store->Mount() == BUNDLEFS_OK, "mount must fall back after an interrupted Bank commit");
    bundlefs_file_t file{};
    Check(store->Open("ring", file) == BUNDLEFS_OK, "previous Catalog must remain visible");
    std::array<uint8_t, 16U> bytes{};
    Check(store->Read(file, 0U, bytes.data(), bytes.size()) == BUNDLEFS_OK &&
              std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == 0x23U; }),
          "interrupted commit must not expose staged data");
}

// Journaled, non-mappable medium such as the SPI NAND behind an FTL: erase is
// emulated by filling 0xFF, programs overwrite in place, and every write
// becomes durable only after Sync().
class FtlLikeStorage final : public micropixel::device::BlockStorage {
   public:
    FtlLikeStorage(uint32_t size, uint32_t sector_size)
        : bytes_(size, UINT8_MAX),
          geometry_{.size_bytes = size,
                    .erase_size = sector_size,
                    .program_size = 1U,
                    .mappable = false,
                    .map_alignment = 0U} {}

    [[nodiscard]] const BlockStorageGeometry& geometry() const override { return geometry_; }
    [[nodiscard]] std::expected<void, micropixel::device::BlockStorageError> Read(
        uint64_t offset, std::span<uint8_t> destination) override {
        if (offset > bytes_.size() || destination.size() > bytes_.size() - offset) {
            return std::unexpected(micropixel::device::BlockStorageError::kInvalidArgument);
        }
        std::copy_n(bytes_.begin() + static_cast<size_t>(offset), destination.size(), destination.begin());
        return {};
    }
    [[nodiscard]] std::expected<void, micropixel::device::BlockStorageError> Program(
        uint64_t offset, std::span<const uint8_t> source) override {
        if (offset > bytes_.size() || source.size() > bytes_.size() - offset) {
            return std::unexpected(micropixel::device::BlockStorageError::kInvalidArgument);
        }
        std::copy(source.begin(), source.end(), bytes_.begin() + static_cast<size_t>(offset));
        ++programs;
        return {};
    }
    [[nodiscard]] std::expected<void, micropixel::device::BlockStorageError> Erase(uint64_t offset,
                                                                                   uint64_t size) override {
        if (offset > bytes_.size() || size > bytes_.size() - offset || (offset % geometry_.erase_size) != 0U ||
            (size % geometry_.erase_size) != 0U) {
            return std::unexpected(micropixel::device::BlockStorageError::kInvalidArgument);
        }
        std::fill_n(bytes_.begin() + static_cast<size_t>(offset), static_cast<size_t>(size), UINT8_MAX);
        return {};
    }
    [[nodiscard]] std::expected<void, micropixel::device::BlockStorageError> Sync() override {
        ++syncs;
        return {};
    }
    [[nodiscard]] std::expected<micropixel::device::BlockStorageMapping, micropixel::device::BlockStorageError> Map(
        std::span<const uint64_t>, uint32_t) override {
        ++map_attempts;
        return std::unexpected(micropixel::device::BlockStorageError::kUnsupported);
    }
    void Unmap(micropixel::device::BlockStorageMapping& mapping) override { mapping = {}; }

    uint32_t programs{};
    uint32_t syncs{};
    uint32_t map_attempts{};

   private:
    std::vector<uint8_t> bytes_;
    BlockStorageGeometry geometry_;
};

void TestGeometryPlanning() {
    constexpr uint64_t kBudgetBytes = 1024ULL * kKiB;  // CONFIG_MICROPIXEL_BUNDLEFS_BLOCK_MAP_BUDGET_KIB default
    const uint32_t nor_block = std::max<uint32_t>(4096U, SPI_FLASH_MMU_PAGE_SIZE);
    const BlockStorageGeometry nor8{.size_bytes = 8U * kMiB,
                                    .erase_size = 4096U,
                                    .program_size = 1U,
                                    .mappable = true,
                                    .map_alignment = SPI_FLASH_MMU_PAGE_SIZE};
    Check(BundleFs::DefaultDataBlockSize(nor8) == nor_block,
          "mappable NOR uses its MMU page as the data block: the medium's own unit");
    BlockStorageGeometry nor24 = nor8;
    nor24.size_bytes = 24U * kMiB;
    Check(BundleFs::DefaultDataBlockSize(nor24) == nor_block, "capacity alone never changes a small medium's block");

    const BlockStorageGeometry nand128{
        .size_bytes = 128U * kMiB, .erase_size = 2048U, .program_size = 1U, .mappable = false, .map_alignment = 0U};
    Check(BundleFs::DefaultDataBlockSize(nand128) == 2048U,
          "an FTL medium keeps its 2 KiB logical sector as the data block");
    const BundleFsGeometry nand_geometry = BundleFs::PlanGeometry(nand128, 0U);
    Check(nand_geometry.valid() && nand_geometry.data_block_size == 2048U &&
              nand_geometry.record_size > MICROPIXEL_BUNDLEFS_BANK_SIZE &&
              nand_geometry.bank_size >= nand_geometry.record_size &&
              (nand_geometry.bank_size % nand128.erase_size) == 0U &&
              nand_geometry.data_offset >= MICROPIXEL_BUNDLEFS_BANK_COUNT * nand_geometry.bank_size &&
              (nand_geometry.data_offset % nand_geometry.data_block_size) == 0U &&
              nand_geometry.data_block_count ==
                  (nand128.size_bytes - nand_geometry.data_offset) / nand_geometry.data_block_size,
          "the Catalog bank and metadata area grow with the block map instead of the block growing with the medium");
    Check(static_cast<uint64_t>(nand_geometry.data_block_count) * sizeof(uint32_t) <= kBudgetBytes,
          "the block map must fit the configured RAM budget");

    BlockStorageGeometry coarse = nand128;
    coarse.erase_size = 128U * kKiB;
    Check(BundleFs::DefaultDataBlockSize(coarse) == 128U * kKiB && BundleFs::PlanGeometry(coarse, 0U).valid(),
          "a raw 128 KiB erase block becomes the data block and the Catalog bank");

    // An SD adapter emulates byte programs by read-modify-write of a sector.
    const BlockStorageGeometry sd64g{.size_bytes = 64ULL * 1024U * kMiB,
                                     .erase_size = 512U,
                                     .program_size = 1U,
                                     .mappable = false,
                                     .map_alignment = 0U};
    const uint32_t sd_block = BundleFs::DefaultDataBlockSize(sd64g);
    const BundleFsGeometry sd_geometry = BundleFs::PlanGeometry(sd64g, 0U);
    Check(sd_block == 256U * kKiB && sd_geometry.valid() && sd_geometry.data_block_size == sd_block &&
              static_cast<uint64_t>(sd_geometry.data_block_count) * sizeof(uint32_t) <= kBudgetBytes &&
              static_cast<uint64_t>(sd_geometry.data_block_count) * sd_block > sd64g.size_bytes - 16U * kMiB,
          "a 64 GB card gets the smallest block whose map fits the RAM budget (256 KiB) and wastes under 16 MiB");
    const BundleFsGeometry sd_explicit = BundleFs::PlanGeometry(sd64g, 64U * kKiB);
    Check(sd_explicit.valid() && sd_explicit.data_block_size == 64U * kKiB &&
              sd_explicit.bank_size >= sd_explicit.data_block_count * sizeof(uint32_t),
          "an explicit block size (mkfs -b) is honoured; the Catalog bank grows to hold its map");

    FtlLikeStorage tiny(32U * kKiB, 2048U);
    BundleFs tiny_store(tiny);
    Check(tiny_store.data_block_size() == 0U && tiny_store.Mount() == BUNDLEFS_ERR_UNAVAILABLE,
          "a medium smaller than the metadata area has no data blocks and does not mount");
}

void TestFtlBackedStoreWithoutMapping(BundleFs& nor_store) {
    constexpr uint32_t kNandSize = 120U * kMiB;  // FTL-usable capacity of a 128 MiB chip
    constexpr uint32_t kSectorSize = 2048U;
    FtlLikeStorage nand(kNandSize, kSectorSize);
    BundleFs nand_store(nand);
    const BundleFsGeometry geometry = BundleFs::PlanGeometry(nand.geometry(), 0U);
    Check(geometry.valid() && !nand_store.mappable() && nand_store.data_block_size() == kSectorSize,
          "FTL-backed store must use the 2 KiB sector as its block and report no mapping");
    store = &nand_store;
    Check(store->Mount() == BUNDLEFS_OK, "erased FTL medium must mount as an empty BundleFS");
    Check(nand.syncs == 1U, "the first Catalog commit must be made durable with one Sync");

    bundlefs_store_info_t info{};
    Check(store->GetStoreInfo(info) == BUNDLEFS_OK && info.data_block_size == kSectorSize &&
              info.total_blocks == geometry.data_block_count && info.total_bytes == kNandSize &&
              info.used_bytes == geometry.data_offset,
          "store info must reflect the per-instance geometry");

    // 300 KiB spans 150 blocks: files are not limited to a fixed block count.
    const std::vector<uint8_t> payload = Pattern(300U * kKiB + 4097U, 7U);
    const uint32_t syncs_before = nand.syncs;
    Replace("nand-app", payload);
    Check(nand.syncs == syncs_before + 1U, "a replacement commit must issue exactly one durability barrier");
    Check(store->GetStoreInfo(info) == BUNDLEFS_OK && info.used_blocks == BlocksFor(kSectorSize, payload.size()),
          "a file must occupy exactly as many 2 KiB blocks as its size needs");

    bundlefs_file_t file{};
    Check(store->Open("nand-app", file) == BUNDLEFS_OK, "file must open on the FTL store");
    std::vector<uint8_t> readback(payload.size());
    Check(store->Read(file, 0U, readback.data(), readback.size()) == BUNDLEFS_OK && readback == payload,
          "unaligned streamed writes must read back byte-exact across many block boundaries");
    std::array<uint8_t, 700U> tail{};
    const uint32_t tail_offset = payload.size() - tail.size();
    Check(store->Read(file, tail_offset, tail.data(), tail.size()) == BUNDLEFS_OK &&
              std::equal(tail.begin(), tail.end(), payload.begin() + tail_offset),
          "a read at the end of a multi-block file must resolve the last blocks");
    bundlefs_mapping_t mapping{};
    Check(store->Map(file, 0U, 16U, mapping) == BUNDLEFS_ERR_UNAVAILABLE && nand.map_attempts == 0U,
          "non-mappable storage must refuse Map before touching the medium");

    // Handles are bound to the store that created them.
    Check(nor_store.Read(file, 0U, readback.data(), 16U) == BUNDLEFS_ERR_INVALID_ARGUMENT,
          "a file handle from one store must be rejected by another store");

    Check(store->Remove("nand-app") == BUNDLEFS_OK && store->Open("nand-app", file) == BUNDLEFS_ERR_NOT_FOUND,
          "remove must work on the FTL store");

    // A second mount of the same medium by a fresh instance sees the same Catalog.
    BundleFs remounted(nand);
    Check(remounted.Mount() == BUNDLEFS_OK, "a fresh instance must mount the committed Catalog");
    bundlefs_store_info_t remounted_info{};
    Check(remounted.GetStoreInfo(remounted_info) == BUNDLEFS_OK && remounted_info.used_blocks == 0U &&
              remounted_info.file_count == 0U && remounted_info.data_block_size == kSectorSize,
          "the remounted Catalog must reflect the last commit and its recorded geometry");

    // An explicit block size that conflicts with the committed Catalog is unsupported, not corrupt.
    BundleFs conflicting(nand, 64U * kKiB);
    Check(conflicting.Mount() == BUNDLEFS_ERR_UNSUPPORTED_FORMAT,
          "a Catalog written with another block size must report unsupported format");

    // An explicit block size is honoured on a fresh medium (mkfs -b).
    FtlLikeStorage coarse(kNandSize, kSectorSize);
    BundleFs coarse_store(coarse, 64U * kKiB);
    Check(coarse_store.Mount() == BUNDLEFS_OK && coarse_store.data_block_size() == 64U * kKiB,
          "an explicit block size must be used when formatting an erased medium");
    BundleFs coarse_reader(coarse);
    Check(coarse_reader.Mount() == BUNDLEFS_OK && coarse_reader.data_block_size() == 64U * kKiB,
          "an instance without a block size preference must adopt the committed one");

    // Foreign content (e.g. a vendor FAT image) is reported distinctly from a damaged BundleFS.
    FtlLikeStorage foreign(kNandSize, kSectorSize);
    std::vector<uint8_t> boot_sector(kSectorSize, 0x00U);
    boot_sector[0] = 0xEBU;
    boot_sector[510] = 0x55U;
    boot_sector[511] = 0xAAU;
    Check(foreign.Program(0U, boot_sector).has_value(), "test medium must accept the foreign sector");
    BundleFs foreign_store(foreign);
    Check(foreign_store.Mount() == BUNDLEFS_ERR_NOT_FORMATTED,
          "non-BundleFS content must mount as not-formatted rather than corrupt");
    Check(foreign_store.Format() == BUNDLEFS_OK && foreign_store.Mount() == BUNDLEFS_OK,
          "formatting a foreign medium must yield an empty BundleFS");
    bundlefs_store_info_t formatted_info{};
    Check(foreign_store.GetStoreInfo(formatted_info) == BUNDLEFS_OK && formatted_info.file_count == 0U &&
              formatted_info.data_block_size == kSectorSize,
          "the formatted medium must expose the medium-derived geometry");
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
    if (fail_next_commit_marker && destination_offset < MICROPIXEL_BUNDLEFS_METADATA_SIZE && size == sizeof(uint32_t)) {
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
    micropixel::platform::storage::PartitionBlockStorage nor_storage(&test_partition);
    BundleFs nor_store(nor_storage);
    const BundleFsGeometry geometry = BundleFs::PlanGeometry(nor_storage.geometry(), 0U);
    const uint32_t nor_block = std::max<uint32_t>(4096U, SPI_FLASH_MMU_PAGE_SIZE);
    Check(nor_storage.present() && nor_store.mappable() && geometry.valid() && geometry.data_block_size == nor_block &&
              nor_store.data_block_size() == nor_block && geometry.bank_size == MICROPIXEL_BUNDLEFS_BANK_SIZE &&
              geometry.data_offset == MICROPIXEL_BUNDLEFS_DATA_OFFSET &&
              geometry.data_block_count == (kPartitionSize - geometry.data_offset) / nor_block,
          "a 24 MiB NOR partition uses the MMU page as its block inside the 16 KiB Catalog ring");
    store = &nor_store;
    TestEmptyMountAndGeometry(geometry);
    TestUnsupportedGeometryIsDistinctFromCorruption(geometry);
    TestReadMapReplaceAndRemove(geometry);
    TestStagedReadsAndAbort();
    TestNewestInstallIsFirstAndUpdateKeepsPosition();
    TestLegacyMigration(nor_storage, MICROPIXEL_BUNDLEFS_V1_FORMAT_VERSION, MICROPIXEL_BUNDLEFS_V1_BANK_SIZE,
                        MICROPIXEL_BUNDLEFS_V1_MAX_FILES);
    TestLegacyMigration(nor_storage, MICROPIXEL_BUNDLEFS_V2_FORMAT_VERSION, MICROPIXEL_BUNDLEFS_V2_BANK_SIZE,
                        BUNDLEFS_MAX_FILES);
    store = &nor_store;
    TestFiftyFiles();
    TestFourBankRingAndInterruptedCommit();
    TestGeometryPlanning();
    TestFtlBackedStoreWithoutMapping(nor_store);
    std::printf("BundleFS tests passed (%" PRIu32 " checks).\n", checks);
    return 0;
}
