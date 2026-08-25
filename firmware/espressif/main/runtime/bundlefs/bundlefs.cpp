#include "runtime/bundlefs/bundlefs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#include "runtime/bundlefs/bundlefs_format.h"
#include "spi_flash_mmap.h"

namespace {

constexpr char kTag[] = "bundlefs";
constexpr char kPartitionName[] = "app_store";
constexpr esp_partition_subtype_t kPartitionSubtype = static_cast<esp_partition_subtype_t>(0x40);
constexpr std::array<uint8_t, 8U> kCatalogMagic{'M', 'P', 'B', 'U', 'N', 'D', 'L', 'E'};
constexpr uint32_t kCommitMarker = 0x434f4d54U;
constexpr uint32_t kErasedWord = UINT32_MAX;
constexpr uint32_t kFileHandleMagic = 0x46494c45U;
constexpr uint32_t kWriterHandleMagic = 0x57524954U;
constexpr uint32_t kFlashChunkSize = 4096U;

struct CatalogEntry final {
    std::array<char, BUNDLEFS_MAX_NAME_LENGTH + 1U> name{};
    std::array<uint8_t, 3U> reserved{};
    uint32_t size{};
    uint32_t content_id{};
    uint16_t block_map_offset{};
    uint16_t block_count{};
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> sha256{};
};

struct CatalogRecord final {
    std::array<uint8_t, 8U> magic{};
    uint16_t format_version{};
    uint16_t header_size{};
    uint32_t record_size{};
    uint64_t generation{};
    uint16_t bank_index{};
    uint16_t bank_count{};
    uint32_t bank_size{};
    uint32_t metadata_size{};
    uint32_t data_offset{};
    uint32_t data_block_size{};
    uint32_t partition_size{};
    uint16_t data_block_count{};
    uint16_t allocation_cursor{};
    uint16_t file_count{};
    uint16_t block_map_count{};
    uint32_t feature_flags{};
    uint32_t payload_size{};
    std::array<CatalogEntry, BUNDLEFS_MAX_FILES> entries{};
    std::array<uint16_t, MICROPIXEL_BUNDLEFS_MAX_DATA_BLOCKS> block_map{};
    std::array<uint8_t, 9946U> reserved{};
    uint32_t checksum{};
    uint32_t commit_marker{kErasedWord};
};

// BundleFS v1 used four 4 KiB records and could describe seven files. Keep the
// exact wire layout so existing devices can mount it and migrate on their next
// Catalog commit. The first v2 migration write targets Bank 1 at offset 16 KiB,
// after all four legacy records, preserving power-loss rollback.
struct CatalogRecordV1 final {
    std::array<uint8_t, 8U> magic{};
    uint16_t format_version{};
    uint16_t header_size{};
    uint32_t record_size{};
    uint64_t generation{};
    uint16_t bank_index{};
    uint16_t bank_count{};
    uint32_t bank_size{};
    uint32_t metadata_size{};
    uint32_t data_offset{};
    uint32_t data_block_size{};
    uint32_t partition_size{};
    uint16_t data_block_count{};
    uint16_t allocation_cursor{};
    uint16_t file_count{};
    uint16_t block_map_count{};
    uint32_t feature_flags{};
    uint32_t payload_size{};
    std::array<CatalogEntry, MICROPIXEL_BUNDLEFS_V1_MAX_FILES> entries{};
    std::array<uint16_t, MICROPIXEL_BUNDLEFS_MAX_DATA_BLOCKS> block_map{};
    std::array<uint8_t, 2474U> reserved{};
    uint32_t checksum{};
    uint32_t commit_marker{kErasedWord};
};

struct FileState final {
    uint32_t magic{};
    uint64_t generation{};
    uint32_t size{};
    uint32_t content_id{};
    std::array<char, BUNDLEFS_MAX_NAME_LENGTH + 1U> name{};
    uint8_t padding{};
    uint16_t block_count{};
    uint16_t reserved{};
    std::array<uint16_t, MICROPIXEL_BUNDLEFS_MAX_FILE_BLOCKS> blocks{};
};

struct WriterState final {
    uint32_t magic{};
    uint64_t base_generation{};
    uint32_t written{};
    uint16_t allocation_cursor{};
    uint16_t reserved{};
    FileState file{};
};

struct CatalogScan final {
    CatalogRecord latest{};
    CatalogRecordV1 legacy_latest{};
    bool has_latest{};
    bool has_legacy_latest{};
    bool any_non_erased{};
    bool unsupported{};
    bool divergent{};
    bool legacy_divergent{};
};

static_assert(sizeof(CatalogEntry) == 112U);
static_assert(sizeof(CatalogRecord) == MICROPIXEL_BUNDLEFS_CATALOG_RECORD_SIZE);
static_assert(sizeof(CatalogRecordV1) == MICROPIXEL_BUNDLEFS_V1_BANK_SIZE);
static_assert(offsetof(CatalogRecord, entries) == 64U);
static_assert(offsetof(CatalogRecordV1, entries) == 64U);
static_assert(sizeof(FileState) <= sizeof(bundlefs_file_t));
static_assert(sizeof(WriterState) <= sizeof(bundlefs_writer_t));
static_assert(SPI_FLASH_MMU_PAGE_SIZE == MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE,
              "BundleFS block size must match the target MMU page size");

std::atomic_flag g_writer_active = ATOMIC_FLAG_INIT;

template <typename T>
struct HeapCapsDeleter final {
    void operator()(T* value) const {
        if (value != nullptr) {
            value->~T();
            heap_caps_free(value);
        }
    }
};

template <typename T>
using HeapCapsPtr = std::unique_ptr<T, HeapCapsDeleter<T>>;

template <typename T>
HeapCapsPtr<T> AllocateWorkspace() {
    void* memory = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (memory == nullptr) {
        return {};
    }
    return HeapCapsPtr<T>(new (memory) T{});
}

template <typename T>
void ZeroWorkspace(T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    auto* bytes = reinterpret_cast<unsigned char*>(&value);
    std::fill_n(bytes, sizeof(value), static_cast<unsigned char>(0U));
}

const esp_partition_t* FindPartition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kPartitionSubtype, kPartitionName);
}

uint16_t DataBlockCount(const esp_partition_t* partition) {
    if (partition == nullptr || partition->size < MICROPIXEL_BUNDLEFS_DATA_OFFSET) {
        return 0U;
    }
    const uint32_t count = (partition->size - MICROPIXEL_BUNDLEFS_DATA_OFFSET) / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
    return static_cast<uint16_t>(std::min<uint32_t>(count, MICROPIXEL_BUNDLEFS_MAX_DATA_BLOCKS));
}

uint32_t BlockOffset(uint16_t block) {
    return MICROPIXEL_BUNDLEFS_DATA_OFFSET + static_cast<uint32_t>(block) * MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
}

uint32_t CatalogBankOffset(uint32_t bank) { return bank * MICROPIXEL_BUNDLEFS_BANK_SIZE; }

uint32_t LegacyCatalogBankOffset(uint32_t bank) { return bank * MICROPIXEL_BUNDLEFS_V1_BANK_SIZE; }

bool ValidName(const char* name) {
    if (name == nullptr) {
        return false;
    }
    const size_t length = ::strnlen(name, BUNDLEFS_MAX_NAME_LENGTH + 1U);
    if (length == 0U || length > BUNDLEFS_MAX_NAME_LENGTH) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const char byte = name[index];
        if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
              byte == '_' || byte == '-' || byte == '.')) {
            return false;
        }
    }
    return true;
}

bool IsNewer(uint64_t left, uint64_t right) { return left > right; }

uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        crc ^= data[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

template <typename Record>
uint32_t CatalogChecksum(const Record& record) {
    constexpr std::array<uint8_t, 4U> kZeroWord{};
    constexpr std::array<uint8_t, 4U> kErasedMarker{UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t crc = UINT32_MAX;
    crc = Crc32Update(crc, bytes, offsetof(Record, checksum));
    crc = Crc32Update(crc, kZeroWord.data(), kZeroWord.size());
    crc = Crc32Update(crc, kErasedMarker.data(), kErasedMarker.size());
    return crc ^ UINT32_MAX;
}

template <typename Record>
bool IsErased(const Record& record) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    return std::all_of(bytes, bytes + sizeof(record), [](uint8_t value) { return value == UINT8_MAX; });
}

bool IsZero(const uint8_t* bytes, size_t size) {
    return std::all_of(bytes, bytes + size, [](uint8_t value) { return value == 0U; });
}

template <typename Record>
bool HasCommittedChecksum(const Record& record) {
    return record.magic == kCatalogMagic && record.commit_marker == kCommitMarker &&
           record.checksum == CatalogChecksum(record);
}

bool HasSupportedGeometry(const CatalogRecord& record, const esp_partition_t* partition) {
    return record.format_version == MICROPIXEL_BUNDLEFS_FORMAT_VERSION &&
           record.header_size == offsetof(CatalogRecord, entries) && record.record_size == sizeof(record) &&
           record.bank_count == MICROPIXEL_BUNDLEFS_BANK_COUNT && record.bank_size == MICROPIXEL_BUNDLEFS_BANK_SIZE &&
           record.metadata_size == MICROPIXEL_BUNDLEFS_METADATA_SIZE &&
           record.data_offset == MICROPIXEL_BUNDLEFS_DATA_OFFSET &&
           record.data_block_size == MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE && record.partition_size == partition->size;
}

bool HasSupportedGeometry(const CatalogRecordV1& record, const esp_partition_t* partition) {
    return record.format_version == MICROPIXEL_BUNDLEFS_V1_FORMAT_VERSION &&
           record.header_size == offsetof(CatalogRecordV1, entries) && record.record_size == sizeof(record) &&
           record.bank_count == MICROPIXEL_BUNDLEFS_BANK_COUNT &&
           record.bank_size == MICROPIXEL_BUNDLEFS_V1_BANK_SIZE &&
           record.metadata_size == MICROPIXEL_BUNDLEFS_METADATA_SIZE &&
           record.data_offset == MICROPIXEL_BUNDLEFS_DATA_OFFSET &&
           record.data_block_size == MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE && record.partition_size == partition->size;
}

template <typename Record>
bool ValidCatalogPayload(const Record& record, const esp_partition_t* partition) {
    const uint16_t data_blocks = DataBlockCount(partition);
    if (record.generation == 0U || record.bank_index >= record.bank_count || record.data_block_count != data_blocks ||
        data_blocks == 0U || record.allocation_cursor >= data_blocks || record.file_count > record.entries.size() ||
        record.block_map_count > data_blocks || record.feature_flags != 0U ||
        record.payload_size != sizeof(record.entries) + sizeof(record.block_map) ||
        !IsZero(record.reserved.data(), record.reserved.size())) {
        return false;
    }

    std::array<bool, MICROPIXEL_BUNDLEFS_MAX_DATA_BLOCKS> used{};
    uint32_t expected_map_offset = 0U;
    for (uint32_t index = 0U; index < record.file_count; ++index) {
        const CatalogEntry& entry = record.entries[index];
        const uint32_t expected_blocks =
            (entry.size + MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE - 1U) / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
        if (!ValidName(entry.name.data()) || !IsZero(entry.reserved.data(), entry.reserved.size()) ||
            entry.size == 0U || entry.size > MICROPIXEL_BUNDLEFS_MAX_FILE_SIZE || entry.block_count == 0U ||
            entry.block_count != expected_blocks || entry.block_count > MICROPIXEL_BUNDLEFS_MAX_FILE_BLOCKS ||
            entry.block_map_offset != expected_map_offset ||
            entry.block_count > record.block_map_count - entry.block_map_offset) {
            return false;
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (std::strcmp(record.entries[previous].name.data(), entry.name.data()) == 0) {
                return false;
            }
        }
        for (uint32_t block_index = 0U; block_index < entry.block_count; ++block_index) {
            const uint16_t block = record.block_map[entry.block_map_offset + block_index];
            if (block >= data_blocks || used[block]) {
                return false;
            }
            used[block] = true;
        }
        expected_map_offset += entry.block_count;
    }
    if (expected_map_offset != record.block_map_count) {
        return false;
    }
    for (uint32_t index = record.file_count; index < record.entries.size(); ++index) {
        if (!IsZero(reinterpret_cast<const uint8_t*>(&record.entries[index]), sizeof(record.entries[index]))) {
            return false;
        }
    }
    for (uint32_t index = record.block_map_count; index < record.block_map.size(); ++index) {
        if (record.block_map[index] != 0U) {
            return false;
        }
    }
    return true;
}

bool ValidCatalog(const CatalogRecord& record, const esp_partition_t* partition) {
    return HasCommittedChecksum(record) && HasSupportedGeometry(record, partition) &&
           ValidCatalogPayload(record, partition);
}

bool ValidCatalog(const CatalogRecordV1& record, const esp_partition_t* partition) {
    return HasCommittedChecksum(record) && HasSupportedGeometry(record, partition) &&
           ValidCatalogPayload(record, partition);
}

void InitializeEmptyCatalog(const esp_partition_t* partition, uint64_t generation, CatalogRecord& record,
                            uint16_t allocation_cursor = 0U) {
    ZeroWorkspace(record);
    record.magic = kCatalogMagic;
    record.format_version = MICROPIXEL_BUNDLEFS_FORMAT_VERSION;
    record.header_size = offsetof(CatalogRecord, entries);
    record.record_size = sizeof(record);
    record.generation = generation;
    record.bank_count = MICROPIXEL_BUNDLEFS_BANK_COUNT;
    record.bank_size = MICROPIXEL_BUNDLEFS_BANK_SIZE;
    record.metadata_size = MICROPIXEL_BUNDLEFS_METADATA_SIZE;
    record.data_offset = MICROPIXEL_BUNDLEFS_DATA_OFFSET;
    record.data_block_size = MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
    record.partition_size = partition->size;
    record.data_block_count = DataBlockCount(partition);
    record.allocation_cursor = allocation_cursor < record.data_block_count ? allocation_cursor : 0U;
    record.payload_size = sizeof(record.entries) + sizeof(record.block_map);
    record.commit_marker = kErasedWord;
}

bundlefs_error_t ReadRecord(const esp_partition_t* partition, uint32_t offset, CatalogRecord& record) {
    return esp_partition_read(partition, offset, &record, sizeof(record)) == ESP_OK ? BUNDLEFS_OK : BUNDLEFS_ERR_IO;
}

bundlefs_error_t ReadRecord(const esp_partition_t* partition, uint32_t offset, CatalogRecordV1& record) {
    return esp_partition_read(partition, offset, &record, sizeof(record)) == ESP_OK ? BUNDLEFS_OK : BUNDLEFS_ERR_IO;
}

bundlefs_error_t ScanCatalog(const esp_partition_t* partition, CatalogScan& scan) {
    ZeroWorkspace(scan);
    auto record = AllocateWorkspace<CatalogRecord>();
    if (record == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    for (uint32_t bank = 0U; bank < MICROPIXEL_BUNDLEFS_BANK_COUNT; ++bank) {
        const uint32_t offset = CatalogBankOffset(bank);
        const bundlefs_error_t read_error = ReadRecord(partition, offset, *record);
        if (read_error != BUNDLEFS_OK) {
            return read_error;
        }
        if (IsErased(*record)) {
            continue;
        }
        scan.any_non_erased = true;
        if (HasCommittedChecksum(*record) && !HasSupportedGeometry(*record, partition)) {
            scan.unsupported = true;
            continue;
        }
        if (!ValidCatalog(*record, partition) || record->bank_index != bank) {
            continue;
        }
        if (!scan.has_latest || IsNewer(record->generation, scan.latest.generation)) {
            scan.latest = *record;
            scan.has_latest = true;
        } else if (record->generation == scan.latest.generation && record->checksum != scan.latest.checksum) {
            scan.divergent = true;
        }
    }
    auto legacy = AllocateWorkspace<CatalogRecordV1>();
    if (legacy == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    for (uint32_t bank = 0U; bank < MICROPIXEL_BUNDLEFS_BANK_COUNT; ++bank) {
        const uint32_t offset = LegacyCatalogBankOffset(bank);
        const bundlefs_error_t read_error = ReadRecord(partition, offset, *legacy);
        if (read_error != BUNDLEFS_OK) {
            return read_error;
        }
        if (IsErased(*legacy)) {
            continue;
        }
        scan.any_non_erased = true;
        if (HasCommittedChecksum(*legacy) && !HasSupportedGeometry(*legacy, partition)) {
            scan.unsupported = true;
            continue;
        }
        if (!ValidCatalog(*legacy, partition) || legacy->bank_index != bank) {
            continue;
        }
        if (!scan.has_legacy_latest || IsNewer(legacy->generation, scan.legacy_latest.generation)) {
            scan.legacy_latest = *legacy;
            scan.has_legacy_latest = true;
        } else if (legacy->generation == scan.legacy_latest.generation &&
                   legacy->checksum != scan.legacy_latest.checksum) {
            scan.legacy_divergent = true;
        }
    }
    return BUNDLEFS_OK;
}

bundlefs_error_t WriteCatalogBank(const esp_partition_t* partition, uint32_t bank, CatalogRecord& record,
                                  CatalogRecord* record_out = nullptr) {
    if (bank >= MICROPIXEL_BUNDLEFS_BANK_COUNT) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const uint32_t destination = CatalogBankOffset(bank);
    if (esp_partition_erase_range(partition, destination, MICROPIXEL_BUNDLEFS_BANK_SIZE) != ESP_OK) {
        return BUNDLEFS_ERR_IO;
    }

    record.bank_index = static_cast<uint16_t>(bank);
    record.checksum = 0U;
    record.commit_marker = kErasedWord;
    record.checksum = CatalogChecksum(record);
    if (esp_partition_write(partition, destination, &record, offsetof(CatalogRecord, commit_marker)) != ESP_OK) {
        return BUNDLEFS_ERR_IO;
    }
    auto verification = AllocateWorkspace<CatalogRecord>();
    if (verification == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    if (ReadRecord(partition, destination, *verification) != BUNDLEFS_OK ||
        std::memcmp(&record, verification.get(), offsetof(CatalogRecord, commit_marker)) != 0) {
        return BUNDLEFS_ERR_IO;
    }
    if (esp_partition_write(partition, destination + offsetof(CatalogRecord, commit_marker), &kCommitMarker,
                            sizeof(kCommitMarker)) != ESP_OK) {
        return BUNDLEFS_ERR_COMMIT;
    }
    if (ReadRecord(partition, destination, *verification) != BUNDLEFS_OK || !ValidCatalog(*verification, partition) ||
        verification->bank_index != bank) {
        return BUNDLEFS_ERR_COMMIT;
    }
    if (record_out != nullptr) {
        *record_out = *verification;
    }
    return BUNDLEFS_OK;
}

bundlefs_error_t LoadCatalog(const esp_partition_t* partition, CatalogRecord& record_out) {
    if (partition == nullptr || DataBlockCount(partition) == 0U) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    auto scan = AllocateWorkspace<CatalogScan>();
    if (scan == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    bundlefs_error_t error = ScanCatalog(partition, *scan);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    if (scan->divergent || (!scan->has_latest && scan->legacy_divergent)) {
        return BUNDLEFS_ERR_CORRUPT;
    }
    if (scan->has_latest) {
        record_out = scan->latest;
        return BUNDLEFS_OK;
    }
    if (scan->has_legacy_latest) {
        const CatalogRecordV1& legacy = scan->legacy_latest;
        InitializeEmptyCatalog(partition, legacy.generation, record_out, legacy.allocation_cursor);
        // Pretend the imported generation occupied v2 Bank 0. Its first v2
        // successor is therefore committed at offset 16 KiB, beyond all four
        // v1 records, so migration remains atomic across power loss.
        record_out.bank_index = 0U;
        record_out.file_count = legacy.file_count;
        record_out.block_map_count = legacy.block_map_count;
        std::copy_n(legacy.entries.begin(), legacy.file_count, record_out.entries.begin());
        std::copy_n(legacy.block_map.begin(), legacy.block_map_count, record_out.block_map.begin());
        ESP_LOGI(kTag, "mounted legacy v1 Catalog: generation=%" PRIu64 " files=%u; migration pending",
                 legacy.generation, static_cast<unsigned>(legacy.file_count));
        return BUNDLEFS_OK;
    }
    {
        if (scan->unsupported) {
            return BUNDLEFS_ERR_UNSUPPORTED_FORMAT;
        }
        if (scan->any_non_erased) {
            return BUNDLEFS_ERR_CORRUPT;
        }
        InitializeEmptyCatalog(partition, 1U, record_out);
        error = WriteCatalogBank(partition, 0U, record_out, &record_out);
        if (error != BUNDLEFS_OK) {
            return error;
        }
        return BUNDLEFS_OK;
    }
}

bundlefs_error_t CommitCatalog(const esp_partition_t* partition, const CatalogRecord& current, CatalogRecord& next) {
    if (next.generation != current.generation + 1U) {
        return BUNDLEFS_ERR_CONFLICT;
    }
    const uint32_t next_bank = (static_cast<uint32_t>(current.bank_index) + 1U) % current.bank_count;
    return WriteCatalogBank(partition, next_bank, next);
}

const CatalogEntry* FindEntry(const CatalogRecord& record, const char* name, uint32_t* index_out = nullptr) {
    for (uint32_t index = 0U; index < record.file_count; ++index) {
        if (std::strcmp(record.entries[index].name.data(), name) == 0) {
            if (index_out != nullptr) {
                *index_out = index;
            }
            return &record.entries[index];
        }
    }
    return nullptr;
}

FileState FileFromEntry(const CatalogRecord& record, const CatalogEntry& entry) {
    FileState file{};
    file.magic = kFileHandleMagic;
    file.generation = record.generation;
    file.size = entry.size;
    file.content_id = entry.content_id;
    std::snprintf(file.name.data(), file.name.size(), "%s", entry.name.data());
    file.block_count = entry.block_count;
    std::copy_n(record.block_map.begin() + entry.block_map_offset, entry.block_count, file.blocks.begin());
    return file;
}

void StoreFile(const FileState& state, bundlefs_file_t* file) {
    *file = {};
    std::memcpy(file->opaque, &state, sizeof(state));
}

bool LoadFile(const bundlefs_file_t* file, FileState& state) {
    if (file == nullptr) {
        return false;
    }
    std::memcpy(&state, file->opaque, sizeof(state));
    return state.magic == kFileHandleMagic && ValidName(state.name.data()) && state.size > 0U &&
           state.size <= MICROPIXEL_BUNDLEFS_MAX_FILE_SIZE && state.block_count > 0U &&
           state.block_count <= MICROPIXEL_BUNDLEFS_MAX_FILE_BLOCKS &&
           state.block_count ==
               (state.size + MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE - 1U) / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
}

void StoreWriter(const WriterState& state, bundlefs_writer_t* writer) {
    *writer = {};
    std::memcpy(writer->opaque, &state, sizeof(state));
}

bool LoadWriter(const bundlefs_writer_t* writer, WriterState& state) {
    if (writer == nullptr) {
        return false;
    }
    std::memcpy(&state, writer->opaque, sizeof(state));
    return state.magic == kWriterHandleMagic && state.file.magic == kFileHandleMagic;
}

bool AcquireWriter() { return !g_writer_active.test_and_set(std::memory_order_acquire); }

void ReleaseWriter() { g_writer_active.clear(std::memory_order_release); }

bundlefs_error_t ReadFileState(const esp_partition_t* partition, const FileState& file, uint32_t offset,
                               void* destination, uint32_t size) {
    if (destination == nullptr || offset > file.size || size > file.size - offset) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    uint8_t* output = static_cast<uint8_t*>(destination);
    uint32_t consumed = 0U;
    while (consumed < size) {
        const uint32_t current = offset + consumed;
        const uint32_t logical_block = current / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
        const uint32_t within_block = current % MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
        const uint32_t remaining = MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE - within_block;
        const uint32_t chunk = std::min(size - consumed, remaining);
        if (esp_partition_read(partition, BlockOffset(file.blocks[logical_block]) + within_block, output + consumed,
                               chunk) != ESP_OK) {
            return BUNDLEFS_ERR_IO;
        }
        consumed += chunk;
    }
    return BUNDLEFS_OK;
}

bundlefs_error_t MapFileState(const esp_partition_t* partition, const FileState& file, uint32_t offset, uint32_t size,
                              bundlefs_mapping_t* mapping_out) {
    if (mapping_out == nullptr || size == 0U || offset > file.size || size > file.size - offset) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const uint32_t first_block = offset / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
    const uint32_t within_first = offset % MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
    const uint32_t page_count =
        (within_first + size + MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE - 1U) / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
    int* pages =
        static_cast<int*>(heap_caps_malloc(page_count * sizeof(*pages), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (pages == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    for (uint32_t index = 0U; index < page_count; ++index) {
        const uint32_t physical = partition->address + BlockOffset(file.blocks[first_block + index]);
        if ((physical % SPI_FLASH_MMU_PAGE_SIZE) != 0U) {
            free(pages);
            return BUNDLEFS_ERR_CORRUPT;
        }
        pages[index] = static_cast<int>(physical / SPI_FLASH_MMU_PAGE_SIZE);
    }
    const void* mapping = nullptr;
    spi_flash_mmap_handle_t handle = 0U;
    const esp_err_t error = spi_flash_mmap_pages(pages, page_count, SPI_FLASH_MMAP_FLAG_DATA, &mapping, &handle);
    free(pages);
    if (error != ESP_OK) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    *mapping_out = bundlefs_mapping_t{
        .data = static_cast<const uint8_t*>(mapping) + within_first,
        .mapping = mapping,
        .size = size,
        .mapping_handle = handle,
    };
    return BUNDLEFS_OK;
}

bundlefs_error_t HashFile(const esp_partition_t* partition, const FileState& file,
                          std::array<uint8_t, BUNDLEFS_SHA256_SIZE>& digest) {
    std::array<uint8_t, kFlashChunkSize> buffer{};
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool valid = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (uint32_t offset = 0U; valid && offset < file.size;) {
        const uint32_t chunk = std::min<uint32_t>(buffer.size(), file.size - offset);
        valid = ReadFileState(partition, file, offset, buffer.data(), chunk) == BUNDLEFS_OK &&
                psa_hash_update(&operation, buffer.data(), chunk) == PSA_SUCCESS;
        offset += chunk;
    }
    size_t digest_size = 0U;
    valid = valid && psa_hash_finish(&operation, digest.data(), digest.size(), &digest_size) == PSA_SUCCESS &&
            digest_size == digest.size();
    (void)psa_hash_abort(&operation);
    return valid ? BUNDLEFS_OK : BUNDLEFS_ERR_IO;
}

uint32_t ContentId(const uint8_t digest[BUNDLEFS_SHA256_SIZE]) {
    return (static_cast<uint32_t>(digest[0]) << 24U) | (static_cast<uint32_t>(digest[1]) << 16U) |
           (static_cast<uint32_t>(digest[2]) << 8U) | static_cast<uint32_t>(digest[3]);
}

bundlefs_error_t AppendEntry(CatalogRecord& destination, const char* name, uint32_t size, uint32_t content_id,
                             const uint8_t sha256[BUNDLEFS_SHA256_SIZE], const uint16_t* blocks, uint16_t block_count) {
    if (destination.file_count >= destination.entries.size() ||
        block_count > destination.block_map.size() - destination.block_map_count) {
        return BUNDLEFS_ERR_CORRUPT;
    }
    CatalogEntry& entry = destination.entries[destination.file_count++];
    std::snprintf(entry.name.data(), entry.name.size(), "%s", name);
    entry.size = size;
    entry.content_id = content_id;
    entry.block_map_offset = destination.block_map_count;
    entry.block_count = block_count;
    std::copy_n(sha256, BUNDLEFS_SHA256_SIZE, entry.sha256.begin());
    std::copy_n(blocks, block_count, destination.block_map.begin() + destination.block_map_count);
    destination.block_map_count += block_count;
    return BUNDLEFS_OK;
}

bundlefs_error_t AppendExisting(CatalogRecord& destination, const CatalogRecord& source, const CatalogEntry& entry) {
    return AppendEntry(destination, entry.name.data(), entry.size, entry.content_id, entry.sha256.data(),
                       source.block_map.data() + entry.block_map_offset, entry.block_count);
}

}  // namespace

extern "C" {

bundlefs_error_t bundlefs_mount(void) {
    const esp_partition_t* partition = FindPartition();
    auto record = AllocateWorkspace<CatalogRecord>();
    if (record == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    const bundlefs_error_t error = LoadCatalog(partition, *record);
    if (error == BUNDLEFS_OK) {
        ESP_LOGI(kTag, "mounted: generation=%" PRIu64 " files=%u blocks=%u/%u", record->generation,
                 static_cast<unsigned>(record->file_count), static_cast<unsigned>(record->block_map_count),
                 static_cast<unsigned>(record->data_block_count));
    }
    return error;
}

bundlefs_error_t bundlefs_format(void) {
    if (!AcquireWriter()) {
        return BUNDLEFS_ERR_BUSY;
    }
    const esp_partition_t* partition = FindPartition();
    bundlefs_error_t error = BUNDLEFS_ERR_UNAVAILABLE;
    if (partition != nullptr && DataBlockCount(partition) > 0U &&
        esp_partition_erase_range(partition, 0U, partition->size) == ESP_OK) {
        auto empty = AllocateWorkspace<CatalogRecord>();
        if (empty != nullptr) {
            InitializeEmptyCatalog(partition, 1U, *empty);
            error = WriteCatalogBank(partition, 0U, *empty);
        }
    }
    ReleaseWriter();
    return error;
}

bundlefs_error_t bundlefs_get_store_info(bundlefs_store_info_t* info_out) {
    if (info_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const esp_partition_t* partition = FindPartition();
    auto record = AllocateWorkspace<CatalogRecord>();
    if (record == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    const bundlefs_error_t error = LoadCatalog(partition, *record);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const uint32_t free =
        static_cast<uint32_t>(record->data_block_count - record->block_map_count) * MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
    const uint32_t used = partition->size - free;
    *info_out = bundlefs_store_info_t{
        .data_block_size = MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE,
        .total_bytes = partition->size,
        .used_bytes = used,
        .free_bytes = free,
        .total_blocks = record->data_block_count,
        .used_blocks = record->block_map_count,
        .file_count = record->file_count,
        .reserved = 0U,
    };
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_list(bundlefs_file_info_t* files_out, uint32_t capacity, uint32_t* count_out) {
    if (files_out == nullptr || capacity == 0U || count_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const esp_partition_t* partition = FindPartition();
    auto record = AllocateWorkspace<CatalogRecord>();
    if (record == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    const bundlefs_error_t error = LoadCatalog(partition, *record);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    if (capacity < record->file_count) {
        return BUNDLEFS_ERR_NO_SPACE;
    }
    *count_out = record->file_count;
    for (uint32_t index = 0U; index < record->file_count; ++index) {
        const CatalogEntry& entry = record->entries[index];
        files_out[index] = {};
        std::snprintf(files_out[index].name, sizeof(files_out[index].name), "%s", entry.name.data());
        files_out[index].size = entry.size;
        files_out[index].content_id = entry.content_id;
    }
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_get_file_sha256(const char* name, uint8_t sha256_out[BUNDLEFS_SHA256_SIZE]) {
    if (!ValidName(name) || sha256_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const esp_partition_t* partition = FindPartition();
    auto record = AllocateWorkspace<CatalogRecord>();
    if (record == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    const bundlefs_error_t error = LoadCatalog(partition, *record);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const CatalogEntry* entry = FindEntry(*record, name);
    if (entry == nullptr) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    std::copy(entry->sha256.begin(), entry->sha256.end(), sha256_out);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_open(const char* name, bundlefs_file_t* file_out) {
    if (!ValidName(name) || file_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const esp_partition_t* partition = FindPartition();
    auto record = AllocateWorkspace<CatalogRecord>();
    if (record == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    const bundlefs_error_t error = LoadCatalog(partition, *record);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const CatalogEntry* entry = FindEntry(*record, name);
    if (entry == nullptr) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    StoreFile(FileFromEntry(*record, *entry), file_out);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_get_file_info(const bundlefs_file_t* file, bundlefs_file_info_t* info_out) {
    FileState state;
    if (!LoadFile(file, state) || info_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    *info_out = {};
    std::snprintf(info_out->name, sizeof(info_out->name), "%s", state.name.data());
    info_out->size = state.size;
    info_out->content_id = state.content_id;
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_read(const bundlefs_file_t* file, uint32_t offset, void* destination, uint32_t size) {
    FileState state;
    const esp_partition_t* partition = FindPartition();
    if (!LoadFile(file, state) || partition == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    return ReadFileState(partition, state, offset, destination, size);
}

bundlefs_error_t bundlefs_mmap(const bundlefs_file_t* file, uint32_t offset, uint32_t size,
                               bundlefs_mapping_t* mapping_out) {
    FileState state;
    const esp_partition_t* partition = FindPartition();
    if (!LoadFile(file, state) || partition == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    return MapFileState(partition, state, offset, size, mapping_out);
}

void bundlefs_munmap(bundlefs_mapping_t* mapping) {
    if (mapping == nullptr) {
        return;
    }
    if (mapping->mapping != nullptr) {
        spi_flash_munmap(mapping->mapping_handle);
    }
    *mapping = {};
}

bundlefs_error_t bundlefs_begin_replace(const char* name, uint32_t size, bundlefs_writer_t* writer_out) {
    if (!ValidName(name) || writer_out == nullptr || size == 0U || size > MICROPIXEL_BUNDLEFS_MAX_FILE_SIZE) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    if (!AcquireWriter()) {
        return BUNDLEFS_ERR_BUSY;
    }
    const esp_partition_t* partition = FindPartition();
    auto record = AllocateWorkspace<CatalogRecord>();
    if (record == nullptr) {
        ReleaseWriter();
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    bundlefs_error_t error = LoadCatalog(partition, *record);
    if (error != BUNDLEFS_OK) {
        ReleaseWriter();
        return error;
    }
    if (FindEntry(*record, name) == nullptr && record->file_count >= BUNDLEFS_MAX_FILES) {
        ReleaseWriter();
        return BUNDLEFS_ERR_TOO_MANY_FILES;
    }

    const uint16_t required =
        static_cast<uint16_t>((size + MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE - 1U) / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE);
    std::array<bool, MICROPIXEL_BUNDLEFS_MAX_DATA_BLOCKS> used{};
    for (uint32_t index = 0U; index < record->block_map_count; ++index) {
        used[record->block_map[index]] = true;
    }
    WriterState writer{};
    writer.magic = kWriterHandleMagic;
    writer.base_generation = record->generation;
    writer.file.magic = kFileHandleMagic;
    writer.file.generation = record->generation + 1U;
    writer.file.size = size;
    std::snprintf(writer.file.name.data(), writer.file.name.size(), "%s", name);
    writer.file.block_count = required;

    uint16_t cursor = record->allocation_cursor;
    uint16_t allocated = 0U;
    for (uint32_t scanned = 0U; scanned < record->data_block_count && allocated < required; ++scanned) {
        const uint16_t block = static_cast<uint16_t>((cursor + scanned) % record->data_block_count);
        if (!used[block]) {
            writer.file.blocks[allocated++] = block;
        }
    }
    if (allocated != required) {
        ReleaseWriter();
        return BUNDLEFS_ERR_NO_SPACE;
    }
    writer.allocation_cursor =
        static_cast<uint16_t>((writer.file.blocks[required - 1U] + 1U) % record->data_block_count);
    for (uint32_t index = 0U; index < required; ++index) {
        if (esp_partition_erase_range(partition, BlockOffset(writer.file.blocks[index]),
                                      MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE) != ESP_OK) {
            ReleaseWriter();
            return BUNDLEFS_ERR_IO;
        }
    }
    StoreWriter(writer, writer_out);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_write(bundlefs_writer_t* writer_handle, const void* data, uint32_t size) {
    WriterState writer;
    const esp_partition_t* partition = FindPartition();
    if (!LoadWriter(writer_handle, writer) || partition == nullptr || data == nullptr ||
        writer.written > writer.file.size || size > writer.file.size - writer.written) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const uint8_t* input = static_cast<const uint8_t*>(data);
    uint32_t consumed = 0U;
    while (consumed < size) {
        const uint32_t current = writer.written + consumed;
        const uint32_t logical_block = current / MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
        const uint32_t within_block = current % MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE;
        const uint32_t remaining = MICROPIXEL_BUNDLEFS_DATA_BLOCK_SIZE - within_block;
        const uint32_t chunk = std::min(size - consumed, remaining);
        if (esp_partition_write(partition, BlockOffset(writer.file.blocks[logical_block]) + within_block,
                                input + consumed, chunk) != ESP_OK) {
            return BUNDLEFS_ERR_IO;
        }
        consumed += chunk;
    }
    writer.written += size;
    StoreWriter(writer, writer_handle);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_open_staged(const bundlefs_writer_t* writer_handle, bundlefs_file_t* file_out) {
    WriterState writer;
    if (!LoadWriter(writer_handle, writer) || file_out == nullptr || writer.written != writer.file.size) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    StoreFile(writer.file, file_out);
    return BUNDLEFS_OK;
}

bundlefs_error_t bundlefs_commit(bundlefs_writer_t* writer_handle,
                                 const uint8_t expected_sha256[BUNDLEFS_SHA256_SIZE]) {
    WriterState writer;
    const esp_partition_t* partition = FindPartition();
    if (!LoadWriter(writer_handle, writer) || partition == nullptr || expected_sha256 == nullptr ||
        writer.written != writer.file.size) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> digest{};
    bundlefs_error_t error = HashFile(partition, writer.file, digest);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    if (!std::equal(digest.begin(), digest.end(), expected_sha256)) {
        return BUNDLEFS_ERR_HASH_MISMATCH;
    }

    auto current = AllocateWorkspace<CatalogRecord>();
    auto next = AllocateWorkspace<CatalogRecord>();
    if (current == nullptr || next == nullptr) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    error = LoadCatalog(partition, *current);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    if (current->generation != writer.base_generation) {
        return BUNDLEFS_ERR_CONFLICT;
    }
    InitializeEmptyCatalog(partition, current->generation + 1U, *next, writer.allocation_cursor);
    const bool replacing = FindEntry(*current, writer.file.name.data()) != nullptr;
    if (!replacing) {
        error = AppendEntry(*next, writer.file.name.data(), writer.file.size, ContentId(digest.data()), digest.data(),
                            writer.file.blocks.data(), writer.file.block_count);
        if (error != BUNDLEFS_OK) {
            return error;
        }
    }
    for (uint32_t index = 0U; index < current->file_count; ++index) {
        const CatalogEntry& entry = current->entries[index];
        if (std::strcmp(entry.name.data(), writer.file.name.data()) == 0) {
            error = AppendEntry(*next, writer.file.name.data(), writer.file.size, ContentId(digest.data()),
                                digest.data(), writer.file.blocks.data(), writer.file.block_count);
        } else {
            error = AppendExisting(*next, *current, entry);
        }
        if (error != BUNDLEFS_OK) {
            return error;
        }
    }
    error = CommitCatalog(partition, *current, *next);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    *writer_handle = {};
    ReleaseWriter();
    return BUNDLEFS_OK;
}

void bundlefs_abort(bundlefs_writer_t* writer) {
    WriterState state;
    if (LoadWriter(writer, state)) {
        *writer = {};
        ReleaseWriter();
    }
}

bundlefs_error_t bundlefs_remove(const char* name) {
    if (!ValidName(name)) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    if (!AcquireWriter()) {
        return BUNDLEFS_ERR_BUSY;
    }
    const esp_partition_t* partition = FindPartition();
    auto current = AllocateWorkspace<CatalogRecord>();
    auto next = AllocateWorkspace<CatalogRecord>();
    if (current == nullptr || next == nullptr) {
        ReleaseWriter();
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    bundlefs_error_t error = LoadCatalog(partition, *current);
    if (error != BUNDLEFS_OK) {
        ReleaseWriter();
        return error;
    }
    if (FindEntry(*current, name) == nullptr) {
        ReleaseWriter();
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    InitializeEmptyCatalog(partition, current->generation + 1U, *next, current->allocation_cursor);
    for (uint32_t index = 0U; index < current->file_count; ++index) {
        const CatalogEntry& entry = current->entries[index];
        if (std::strcmp(entry.name.data(), name) != 0) {
            error = AppendExisting(*next, *current, entry);
            if (error != BUNDLEFS_OK) {
                ReleaseWriter();
                return error;
            }
        }
    }
    error = CommitCatalog(partition, *current, *next);
    ReleaseWriter();
    return error;
}

}  // extern "C"
