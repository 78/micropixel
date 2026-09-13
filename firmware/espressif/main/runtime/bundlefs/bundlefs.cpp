#include "runtime/bundlefs/bundlefs.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <type_traits>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "runtime/bundlefs/bundlefs_format.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "bundlefs";
constexpr std::array<uint8_t, 8U> kCatalogMagic{'M', 'P', 'B', 'U', 'N', 'D', 'L', 'E'};
constexpr uint32_t kCommitMarker = 0x434f4d54U;
constexpr uint32_t kErasedWord = UINT32_MAX;
constexpr uint32_t kFileHandleMagic = 0x46494c45U;
constexpr uint32_t kWriterHandleMagic = 0x57524954U;
constexpr uint32_t kFlashChunkSize = 4096U;
constexpr uint32_t kHeaderSize = MICROPIXEL_BUNDLEFS_CATALOG_HEADER_SIZE;
constexpr uint32_t kEntrySize = MICROPIXEL_BUNDLEFS_CATALOG_ENTRY_SIZE;
constexpr uint32_t kTrailerSize = MICROPIXEL_BUNDLEFS_CATALOG_TRAILER_SIZE;
constexpr uint32_t kBankCount = MICROPIXEL_BUNDLEFS_BANK_COUNT;
constexpr uint32_t kFileCommitted = 1U;
constexpr uint32_t kFileStaged = 2U;

// ---------------------------------------------------------------------------
// v3 on-disk layout: header | entries[entry_capacity] | block_map[data_block_count] | checksum | commit marker
// ---------------------------------------------------------------------------

struct CatalogHeader final {
    std::array<uint8_t, 8U> magic{};
    uint16_t format_version{};
    uint16_t header_size{};
    uint32_t record_size{};
    uint64_t generation{};
    uint16_t bank_index{};
    uint16_t bank_count{};
    uint32_t bank_size{};
    uint32_t data_offset{};
    uint32_t data_block_size{};
    uint64_t partition_size{};
    uint32_t data_block_count{};
    uint32_t allocation_cursor{};
    uint16_t file_count{};
    uint16_t entry_capacity{};
    uint32_t block_map_count{};
};

struct CatalogEntry final {
    std::array<char, BUNDLEFS_MAX_NAME_LENGTH + 1U> name{};
    std::array<uint8_t, 3U> reserved{};
    uint32_t size{};
    uint32_t content_id{};
    uint32_t block_map_offset{};
    uint32_t block_count{};
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> sha256{};
    uint32_t reserved1{};
};

static_assert(sizeof(CatalogHeader) == kHeaderSize);
static_assert(sizeof(CatalogEntry) == kEntrySize);
static_assert(std::is_trivially_copyable_v<CatalogHeader> && std::is_trivially_copyable_v<CatalogEntry>);

// ---------------------------------------------------------------------------
// Legacy layouts (v1: 4 KiB banks, 7 files; v2: 16 KiB banks, 50 files). Both
// carry 16-bit block indices for at most 383 blocks and a fixed 64 KiB
// metadata area. They are imported into a v3 record in RAM; the next commit
// writes v3 into the following bank of the same 16 KiB ring, so migration
// keeps the power-loss rollback point.
// ---------------------------------------------------------------------------

struct LegacyEntry final {
    std::array<char, BUNDLEFS_MAX_NAME_LENGTH + 1U> name{};
    std::array<uint8_t, 3U> reserved{};
    uint32_t size{};
    uint32_t content_id{};
    uint16_t block_map_offset{};
    uint16_t block_count{};
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> sha256{};
};

template <uint32_t kMaxFiles, uint32_t kReserved>
struct LegacyRecord final {
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
    std::array<LegacyEntry, kMaxFiles> entries{};
    std::array<uint16_t, MICROPIXEL_BUNDLEFS_LEGACY_MAX_DATA_BLOCKS> block_map{};
    std::array<uint8_t, kReserved> reserved{};
    uint32_t checksum{};
    uint32_t commit_marker{kErasedWord};
};

using CatalogRecordV2 = LegacyRecord<BUNDLEFS_MAX_FILES, 9946U>;
using CatalogRecordV1 = LegacyRecord<MICROPIXEL_BUNDLEFS_V1_MAX_FILES, 2474U>;

static_assert(sizeof(LegacyEntry) == MICROPIXEL_BUNDLEFS_LEGACY_ENTRY_SIZE);
static_assert(sizeof(CatalogRecordV2) == MICROPIXEL_BUNDLEFS_V2_BANK_SIZE);
static_assert(sizeof(CatalogRecordV1) == MICROPIXEL_BUNDLEFS_V1_BANK_SIZE);
static_assert(offsetof(CatalogRecordV2, entries) == 64U);
static_assert(offsetof(CatalogRecordV1, entries) == 64U);

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

struct FileState final {
    uint32_t magic{};
    uint32_t store_tag{};
    uint32_t kind{};
    uint32_t size{};
    uint32_t content_id{};
    uint32_t block_count{};
    uint32_t entry_hint{};
    uint32_t reserved{};
    uint64_t generation{};  // Catalog generation (committed) or writer generation (staged)
    std::array<char, BUNDLEFS_MAX_NAME_LENGTH + 1U> name{};
    std::array<uint8_t, 3U> padding{};
};

struct WriterState final {
    uint32_t magic{};
    uint32_t written{};
    uint32_t allocation_cursor{};
    uint32_t reserved{};
    uint64_t base_generation{};
    FileState file{};
};

static_assert(sizeof(FileState) <= sizeof(bundlefs_file_t));
static_assert(sizeof(WriterState) <= sizeof(bundlefs_writer_t));
static_assert(std::is_trivially_copyable_v<FileState> && std::is_trivially_copyable_v<WriterState>);

// ---------------------------------------------------------------------------
// Heap helpers. Buffers are sized once per geometry (mount/format) and for
// each Catalog transaction; nothing grows on the read path.
// ---------------------------------------------------------------------------

void* AllocateBytes(uint32_t size) {
    void* memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        memory = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return memory;
}

struct ScopedBytes final {
    ScopedBytes() = default;
    explicit ScopedBytes(uint32_t size) : bytes(static_cast<uint8_t*>(AllocateBytes(size))), size(size) {
        if (bytes == nullptr) {
            this->size = 0U;
        }
    }
    ScopedBytes(const ScopedBytes&) = delete;
    ScopedBytes& operator=(const ScopedBytes&) = delete;
    ~ScopedBytes() { heap_caps_free(bytes); }

    [[nodiscard]] bool valid() const { return bytes != nullptr; }
    uint8_t* release() {
        uint8_t* result = bytes;
        bytes = nullptr;
        size = 0U;
        return result;
    }

    uint8_t* bytes{};
    uint32_t size{};
};

// ---------------------------------------------------------------------------
// Record view over a v3 buffer
// ---------------------------------------------------------------------------

struct RecordView final {
    uint8_t* bytes{};
    const BundleFsGeometry* geometry{};

    [[nodiscard]] CatalogHeader& header() const { return *reinterpret_cast<CatalogHeader*>(bytes); }
    [[nodiscard]] CatalogEntry* entries() const { return reinterpret_cast<CatalogEntry*>(bytes + kHeaderSize); }
    [[nodiscard]] uint32_t* block_map() const {
        return reinterpret_cast<uint32_t*>(bytes + kHeaderSize + geometry->entry_capacity * kEntrySize);
    }
    [[nodiscard]] uint32_t& checksum() const {
        return *reinterpret_cast<uint32_t*>(bytes + geometry->record_size - kTrailerSize);
    }
    [[nodiscard]] uint32_t& commit_marker() const {
        return *reinterpret_cast<uint32_t*>(bytes + geometry->record_size - sizeof(uint32_t));
    }
};

uint32_t RecordSizeFor(uint32_t entry_capacity, uint32_t data_block_count) {
    return kHeaderSize + entry_capacity * kEntrySize + data_block_count * static_cast<uint32_t>(sizeof(uint32_t)) +
           kTrailerSize;
}

bool IsPowerOfTwo(uint32_t value) { return value != 0U && (value & (value - 1U)) == 0U; }

uint64_t RoundUp(uint64_t value, uint64_t unit) { return (value + unit - 1U) / unit * unit; }

uint32_t BlocksFor(uint32_t block_size, uint32_t size) {
    return static_cast<uint32_t>((static_cast<uint64_t>(size) + block_size - 1U) / block_size);
}

uint64_t BlockOffset(const BundleFsGeometry& geometry, uint32_t block) {
    return static_cast<uint64_t>(geometry.data_offset) + static_cast<uint64_t>(block) * geometry.data_block_size;
}

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

uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t index = 0U; index < size; ++index) {
        crc ^= data[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

// CRC of the record body followed by a zero checksum word and an erased
// commit marker, so the same value is valid before and after the marker lands.
uint32_t BodyChecksum(const uint8_t* bytes, uint32_t body_size) {
    constexpr std::array<uint8_t, 4U> kZeroWord{};
    constexpr std::array<uint8_t, 4U> kErasedMarker{UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
    uint32_t crc = UINT32_MAX;
    crc = Crc32Update(crc, bytes, body_size);
    crc = Crc32Update(crc, kZeroWord.data(), kZeroWord.size());
    crc = Crc32Update(crc, kErasedMarker.data(), kErasedMarker.size());
    return crc ^ UINT32_MAX;
}

bool AllErased(const uint8_t* bytes, size_t size) {
    return std::all_of(bytes, bytes + size, [](uint8_t value) { return value == UINT8_MAX; });
}

bool IsZero(const uint8_t* bytes, size_t size) {
    return std::all_of(bytes, bytes + size, [](uint8_t value) { return value == 0U; });
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

bool ValidBlockSize(const device::BlockStorageGeometry& storage, uint32_t block_size) {
    if (!IsPowerOfTwo(block_size) || storage.erase_size == 0U || (block_size % storage.erase_size) != 0U ||
        storage.program_size == 0U || (block_size % storage.program_size) != 0U) {
        return false;
    }
    if (storage.mappable && (storage.map_alignment == 0U || (block_size % storage.map_alignment) != 0U)) {
        return false;
    }
    // The commit marker is a lone 32-bit program after the record body.
    return storage.program_size <= sizeof(kCommitMarker);
}

// Geometry described by a v3 header, or invalid when the header does not fit
// this medium. `requested_block_size` of zero accepts the recorded block size.
BundleFsGeometry GeometryFromHeader(const CatalogHeader& header, const device::BlockStorageGeometry& storage,
                                    uint32_t requested_block_size) {
    BundleFsGeometry geometry{};
    if (header.magic != kCatalogMagic || header.format_version != MICROPIXEL_BUNDLEFS_FORMAT_VERSION ||
        header.header_size != kHeaderSize || header.bank_count != kBankCount || header.bank_index >= kBankCount ||
        header.partition_size != storage.size_bytes || !ValidBlockSize(storage, header.data_block_size) ||
        (requested_block_size != 0U && requested_block_size != header.data_block_size) || header.bank_size == 0U ||
        (header.bank_size % storage.erase_size) != 0U || header.data_offset == 0U ||
        (header.data_offset % header.data_block_size) != 0U ||
        static_cast<uint64_t>(header.bank_size) * kBankCount > header.data_offset ||
        header.data_offset >= storage.size_bytes || header.entry_capacity == 0U) {
        return geometry;
    }
    const uint64_t data_blocks = (storage.size_bytes - header.data_offset) / header.data_block_size;
    if (data_blocks == 0U || data_blocks > UINT32_MAX || header.data_block_count != data_blocks ||
        header.record_size != RecordSizeFor(header.entry_capacity, header.data_block_count) ||
        header.record_size > header.bank_size) {
        return geometry;
    }
    geometry.size_bytes = storage.size_bytes;
    geometry.data_block_size = header.data_block_size;
    geometry.bank_size = header.bank_size;
    geometry.data_offset = header.data_offset;
    geometry.data_block_count = header.data_block_count;
    geometry.entry_capacity = header.entry_capacity;
    geometry.record_size = header.record_size;
    return geometry;
}

// Geometry for importing a legacy Catalog: the fixed 64 KiB metadata area
// and 16 KiB banks, block size taken from the legacy record.
BundleFsGeometry LegacyImportGeometry(const device::BlockStorageGeometry& storage, uint32_t block_size) {
    BundleFsGeometry geometry{};
    if (!ValidBlockSize(storage, block_size) || (MICROPIXEL_BUNDLEFS_V2_BANK_SIZE % storage.erase_size) != 0U ||
        (MICROPIXEL_BUNDLEFS_DATA_OFFSET % block_size) != 0U || storage.size_bytes <= MICROPIXEL_BUNDLEFS_DATA_OFFSET) {
        return geometry;
    }
    const uint64_t data_blocks = (storage.size_bytes - MICROPIXEL_BUNDLEFS_DATA_OFFSET) / block_size;
    const uint32_t record_size = RecordSizeFor(BUNDLEFS_MAX_FILES, static_cast<uint32_t>(data_blocks));
    if (data_blocks == 0U || data_blocks > UINT32_MAX || record_size > MICROPIXEL_BUNDLEFS_V2_BANK_SIZE) {
        return geometry;
    }
    geometry.size_bytes = storage.size_bytes;
    geometry.data_block_size = block_size;
    geometry.bank_size = MICROPIXEL_BUNDLEFS_V2_BANK_SIZE;
    geometry.data_offset = MICROPIXEL_BUNDLEFS_DATA_OFFSET;
    geometry.data_block_count = static_cast<uint32_t>(data_blocks);
    geometry.entry_capacity = BUNDLEFS_MAX_FILES;
    geometry.record_size = record_size;
    return geometry;
}

// ---------------------------------------------------------------------------
// v3 record validation
// ---------------------------------------------------------------------------

bool ValidRecordPayload(const RecordView& record, ScopedBytes& used_bits) {
    const BundleFsGeometry& geometry = *record.geometry;
    const CatalogHeader& header = record.header();
    if (header.generation == 0U || header.file_count > geometry.entry_capacity ||
        header.block_map_count > geometry.data_block_count || header.allocation_cursor >= geometry.data_block_count) {
        return false;
    }
    std::fill_n(used_bits.bytes, used_bits.size, static_cast<uint8_t>(0U));
    const CatalogEntry* entries = record.entries();
    const uint32_t* block_map = record.block_map();
    uint32_t expected_map_offset = 0U;
    for (uint32_t index = 0U; index < header.file_count; ++index) {
        const CatalogEntry& entry = entries[index];
        if (!ValidName(entry.name.data()) || !IsZero(entry.reserved.data(), entry.reserved.size()) ||
            entry.reserved1 != 0U || entry.size == 0U || entry.block_count == 0U ||
            entry.block_count != BlocksFor(geometry.data_block_size, entry.size) ||
            entry.block_map_offset != expected_map_offset ||
            entry.block_count > header.block_map_count - entry.block_map_offset) {
            return false;
        }
        for (uint32_t previous = 0U; previous < index; ++previous) {
            if (std::strcmp(entries[previous].name.data(), entry.name.data()) == 0) {
                return false;
            }
        }
        for (uint32_t block_index = 0U; block_index < entry.block_count; ++block_index) {
            const uint32_t block = block_map[entry.block_map_offset + block_index];
            if (block >= geometry.data_block_count) {
                return false;
            }
            uint8_t& bits = used_bits.bytes[block / 8U];
            const uint8_t mask = static_cast<uint8_t>(1U << (block % 8U));
            if ((bits & mask) != 0U) {
                return false;
            }
            bits |= mask;
        }
        expected_map_offset += entry.block_count;
    }
    if (expected_map_offset != header.block_map_count) {
        return false;
    }
    for (uint32_t index = header.file_count; index < geometry.entry_capacity; ++index) {
        if (!IsZero(reinterpret_cast<const uint8_t*>(&entries[index]), sizeof(entries[index]))) {
            return false;
        }
    }
    for (uint32_t index = header.block_map_count; index < geometry.data_block_count; ++index) {
        if (block_map[index] != 0U) {
            return false;
        }
    }
    return true;
}

bool HasCommittedChecksum(const RecordView& record) {
    return record.commit_marker() == kCommitMarker &&
           record.checksum() == BodyChecksum(record.bytes, record.geometry->record_size - kTrailerSize);
}

uint32_t UsedBitsSize(uint32_t data_block_count) { return (data_block_count + 7U) / 8U; }

void InitializeEmptyRecord(const BundleFsGeometry& geometry, uint64_t generation, uint32_t allocation_cursor,
                           RecordView& record) {
    std::fill_n(record.bytes, geometry.record_size, static_cast<uint8_t>(0U));
    CatalogHeader& header = record.header();
    header.magic = kCatalogMagic;
    header.format_version = MICROPIXEL_BUNDLEFS_FORMAT_VERSION;
    header.header_size = kHeaderSize;
    header.record_size = geometry.record_size;
    header.generation = generation;
    header.bank_count = kBankCount;
    header.bank_size = geometry.bank_size;
    header.data_offset = geometry.data_offset;
    header.data_block_size = geometry.data_block_size;
    header.partition_size = geometry.size_bytes;
    header.data_block_count = geometry.data_block_count;
    header.allocation_cursor = allocation_cursor < geometry.data_block_count ? allocation_cursor : 0U;
    header.entry_capacity = static_cast<uint16_t>(geometry.entry_capacity);
    record.commit_marker() = kErasedWord;
}

bundlefs_error_t AppendEntry(RecordView& record, const char* name, uint32_t size, uint32_t content_id,
                             const uint8_t sha256[BUNDLEFS_SHA256_SIZE], const uint32_t* blocks, uint32_t block_count) {
    CatalogHeader& header = record.header();
    const BundleFsGeometry& geometry = *record.geometry;
    if (header.file_count >= geometry.entry_capacity ||
        block_count > geometry.data_block_count - header.block_map_count) {
        return BUNDLEFS_ERR_CORRUPT;
    }
    CatalogEntry& entry = record.entries()[header.file_count++];
    std::snprintf(entry.name.data(), entry.name.size(), "%s", name);
    entry.size = size;
    entry.content_id = content_id;
    entry.block_map_offset = header.block_map_count;
    entry.block_count = block_count;
    std::copy_n(sha256, BUNDLEFS_SHA256_SIZE, entry.sha256.begin());
    std::copy_n(blocks, block_count, record.block_map() + header.block_map_count);
    header.block_map_count += block_count;
    return BUNDLEFS_OK;
}

bundlefs_error_t AppendExisting(RecordView& destination, const RecordView& source, const CatalogEntry& entry) {
    return AppendEntry(destination, entry.name.data(), entry.size, entry.content_id, entry.sha256.data(),
                       source.block_map() + entry.block_map_offset, entry.block_count);
}

const CatalogEntry* FindEntry(const RecordView& record, const char* name, uint32_t* index_out = nullptr) {
    const CatalogEntry* entries = record.entries();
    for (uint32_t index = 0U; index < record.header().file_count; ++index) {
        if (std::strcmp(entries[index].name.data(), name) == 0) {
            if (index_out != nullptr) {
                *index_out = index;
            }
            return &entries[index];
        }
    }
    return nullptr;
}

uint32_t ContentId(const uint8_t digest[BUNDLEFS_SHA256_SIZE]) {
    return (static_cast<uint32_t>(digest[0]) << 24U) | (static_cast<uint32_t>(digest[1]) << 16U) |
           (static_cast<uint32_t>(digest[2]) << 8U) | static_cast<uint32_t>(digest[3]);
}

// ---------------------------------------------------------------------------
// Legacy record validation (verbatim v1/v2 rules)
// ---------------------------------------------------------------------------

struct LegacyLayout final {
    uint64_t size{};
    uint32_t block_size{};
};

uint16_t LegacyDataBlockCount(const LegacyLayout& layout) {
    if (layout.block_size == 0U || layout.size < MICROPIXEL_BUNDLEFS_DATA_OFFSET) {
        return 0U;
    }
    const uint64_t count = (layout.size - MICROPIXEL_BUNDLEFS_DATA_OFFSET) / layout.block_size;
    return static_cast<uint16_t>(std::min<uint64_t>(count, MICROPIXEL_BUNDLEFS_LEGACY_MAX_DATA_BLOCKS));
}

template <typename Record>
uint32_t LegacyChecksum(const Record& record) {
    return BodyChecksum(reinterpret_cast<const uint8_t*>(&record), offsetof(Record, checksum));
}

template <typename Record>
bool LegacyCommitted(const Record& record) {
    return record.magic == kCatalogMagic && record.commit_marker == kCommitMarker &&
           record.checksum == LegacyChecksum(record);
}

template <typename Record>
bool LegacyGeometryMatches(const Record& record, uint16_t format_version, uint32_t bank_size,
                           const LegacyLayout& layout) {
    return record.format_version == format_version && record.header_size == offsetof(Record, entries) &&
           record.record_size == sizeof(record) && record.bank_count == kBankCount && record.bank_size == bank_size &&
           record.metadata_size == MICROPIXEL_BUNDLEFS_METADATA_SIZE &&
           record.data_offset == MICROPIXEL_BUNDLEFS_DATA_OFFSET && record.data_block_size == layout.block_size &&
           record.partition_size == layout.size;
}

template <typename Record>
bool LegacyPayloadValid(const Record& record, const LegacyLayout& layout) {
    const uint16_t data_blocks = LegacyDataBlockCount(layout);
    if (record.generation == 0U || record.bank_index >= record.bank_count || record.data_block_count != data_blocks ||
        data_blocks == 0U || record.allocation_cursor >= data_blocks || record.file_count > record.entries.size() ||
        record.block_map_count > data_blocks || record.feature_flags != 0U ||
        record.payload_size != sizeof(record.entries) + sizeof(record.block_map) ||
        !IsZero(record.reserved.data(), record.reserved.size())) {
        return false;
    }
    std::array<bool, MICROPIXEL_BUNDLEFS_LEGACY_MAX_DATA_BLOCKS> used{};
    uint32_t expected_map_offset = 0U;
    for (uint32_t index = 0U; index < record.file_count; ++index) {
        const LegacyEntry& entry = record.entries[index];
        if (!ValidName(entry.name.data()) || !IsZero(entry.reserved.data(), entry.reserved.size()) ||
            entry.size == 0U || entry.block_count == 0U ||
            entry.block_count != BlocksFor(layout.block_size, entry.size) ||
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

// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------

bundlefs_error_t ReadStorage(device::BlockStorage& storage, uint64_t offset, void* destination, uint32_t size) {
    return storage.Read(offset, std::span<uint8_t>(static_cast<uint8_t*>(destination), size)) ? BUNDLEFS_OK
                                                                                              : BUNDLEFS_ERR_IO;
}

bundlefs_error_t ProgramStorage(device::BlockStorage& storage, uint64_t offset, const void* source, uint32_t size) {
    return storage.Program(offset, std::span<const uint8_t>(static_cast<const uint8_t*>(source), size))
               ? BUNDLEFS_OK
               : BUNDLEFS_ERR_IO;
}

bundlefs_error_t EraseStorage(device::BlockStorage& storage, uint64_t offset, uint64_t size) {
    return storage.Erase(offset, size) ? BUNDLEFS_OK : BUNDLEFS_ERR_IO;
}

// Result of scanning every bank of a medium.
struct ScanResult final {
    ScopedBytes latest{};  // v3 record with the highest generation
    BundleFsGeometry geometry{};
    bool has_latest{};
    bool divergent{};
    bool unsupported{};
    bool any_non_erased{};
    bool any_bundlefs{};
};

// Grows `buffer` to at least `size` bytes; contents are not preserved.
bool Reserve(ScopedBytes& buffer, uint32_t size) {
    if (buffer.valid() && buffer.size >= size) {
        return true;
    }
    heap_caps_free(buffer.bytes);
    buffer.bytes = static_cast<uint8_t*>(AllocateBytes(size));
    buffer.size = buffer.bytes != nullptr ? size : 0U;
    return buffer.valid();
}

// Scans the v3 ring assuming banks of `bank_size` bytes.
bundlefs_error_t ScanBanks(device::BlockStorage& storage, uint32_t bank_size, uint32_t requested_block_size,
                           ScanResult& result) {
    const device::BlockStorageGeometry& medium = storage.geometry();
    if (bank_size == 0U || static_cast<uint64_t>(bank_size) * kBankCount > medium.size_bytes) {
        return BUNDLEFS_OK;
    }
    alignas(8) std::array<uint8_t, kHeaderSize> header_bytes{};
    ScopedBytes scratch;
    ScopedBytes used_bits;
    for (uint32_t bank = 0U; bank < kBankCount; ++bank) {
        const uint64_t offset = static_cast<uint64_t>(bank) * bank_size;
        bundlefs_error_t error = ReadStorage(storage, offset, header_bytes.data(), header_bytes.size());
        if (error != BUNDLEFS_OK) {
            return error;
        }
        if (AllErased(header_bytes.data(), header_bytes.size())) {
            continue;
        }
        result.any_non_erased = true;
        CatalogHeader header{};
        std::memcpy(&header, header_bytes.data(), sizeof(header));
        if (header.magic != kCatalogMagic) {
            continue;
        }
        result.any_bundlefs = true;
        if (header.format_version != MICROPIXEL_BUNDLEFS_FORMAT_VERSION) {
            continue;  // legacy records are handled by the caller
        }
        const BundleFsGeometry geometry = GeometryFromHeader(header, medium, requested_block_size);
        if (!geometry.valid()) {
            // A complete, committed record with a foreign geometry (another
            // block size, another medium size) is unsupported rather than corrupt.
            if (header.record_size >= kHeaderSize + kTrailerSize && header.record_size <= bank_size &&
                Reserve(scratch, header.record_size) &&
                ReadStorage(storage, offset, scratch.bytes, header.record_size) == BUNDLEFS_OK) {
                uint32_t checksum = 0U;
                uint32_t marker = 0U;
                std::memcpy(&checksum, scratch.bytes + header.record_size - kTrailerSize, sizeof(checksum));
                std::memcpy(&marker, scratch.bytes + header.record_size - sizeof(uint32_t), sizeof(marker));
                if (marker == kCommitMarker &&
                    checksum == BodyChecksum(scratch.bytes, header.record_size - kTrailerSize)) {
                    result.unsupported = true;
                }
            }
            continue;
        }
        if (geometry.bank_size != bank_size || header.bank_index != bank) {
            continue;
        }
        if (!Reserve(scratch, geometry.record_size) || !Reserve(used_bits, UsedBitsSize(geometry.data_block_count))) {
            return BUNDLEFS_ERR_UNAVAILABLE;
        }
        error = ReadStorage(storage, offset, scratch.bytes, geometry.record_size);
        if (error != BUNDLEFS_OK) {
            return error;
        }
        const RecordView view{.bytes = scratch.bytes, .geometry = &geometry};
        if (!HasCommittedChecksum(view) || !ValidRecordPayload(view, used_bits)) {
            continue;
        }
        const uint64_t generation = view.header().generation;
        if (result.has_latest) {
            const RecordView latest{.bytes = result.latest.bytes, .geometry = &result.geometry};
            const uint64_t latest_generation = latest.header().generation;
            if (generation == latest_generation && latest.checksum() != view.checksum()) {
                result.divergent = true;
                continue;
            }
            if (generation <= latest_generation) {
                continue;
            }
        }
        if (!Reserve(result.latest, geometry.record_size)) {
            return BUNDLEFS_ERR_UNAVAILABLE;
        }
        std::memcpy(result.latest.bytes, scratch.bytes, geometry.record_size);
        result.geometry = geometry;
        result.has_latest = true;
    }
    return BUNDLEFS_OK;
}

// Scans one legacy ring and imports the newest valid record into `result`
// as a v3 record when nothing newer is known.
template <typename Record>
bundlefs_error_t ScanLegacy(device::BlockStorage& storage, uint16_t format_version, uint32_t bank_size,
                            uint32_t requested_block_size, ScanResult& result) {
    const device::BlockStorageGeometry& medium = storage.geometry();
    if (static_cast<uint64_t>(bank_size) * kBankCount > medium.size_bytes) {
        return BUNDLEFS_OK;
    }
    ScopedBytes scratch(sizeof(Record));
    ScopedBytes newest(sizeof(Record));
    if (!scratch.valid() || !newest.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    bool has_newest = false;
    bool divergent = false;
    for (uint32_t bank = 0U; bank < kBankCount; ++bank) {
        const uint64_t offset = static_cast<uint64_t>(bank) * bank_size;
        const bundlefs_error_t error = ReadStorage(storage, offset, scratch.bytes, sizeof(Record));
        if (error != BUNDLEFS_OK) {
            return error;
        }
        if (AllErased(scratch.bytes, sizeof(Record))) {
            continue;
        }
        result.any_non_erased = true;
        const auto* record = reinterpret_cast<const Record*>(scratch.bytes);
        if (record->magic != kCatalogMagic) {
            continue;
        }
        result.any_bundlefs = true;
        if (!LegacyCommitted(*record)) {
            continue;
        }
        const LegacyLayout layout{.size = medium.size_bytes, .block_size = record->data_block_size};
        const bool block_size_accepted =
            ValidBlockSize(medium, record->data_block_size) &&
            (requested_block_size == 0U || requested_block_size == record->data_block_size);
        if (!block_size_accepted || !LegacyGeometryMatches(*record, format_version, bank_size, layout)) {
            result.unsupported = true;
            continue;
        }
        if (!LegacyPayloadValid(*record, layout) || record->bank_index != bank) {
            continue;
        }
        const auto* current = reinterpret_cast<const Record*>(newest.bytes);
        if (has_newest && record->generation == current->generation && record->checksum != current->checksum) {
            divergent = true;
            continue;
        }
        if (has_newest && record->generation <= current->generation) {
            continue;
        }
        std::memcpy(newest.bytes, scratch.bytes, sizeof(Record));
        has_newest = true;
    }
    if (result.has_latest || !has_newest) {
        return BUNDLEFS_OK;
    }
    if (divergent) {
        result.divergent = true;
        return BUNDLEFS_OK;
    }
    const auto* legacy = reinterpret_cast<const Record*>(newest.bytes);
    const BundleFsGeometry geometry = LegacyImportGeometry(medium, legacy->data_block_size);
    if (!geometry.valid()) {
        result.unsupported = true;
        return BUNDLEFS_OK;
    }
    if (!Reserve(result.latest, geometry.record_size)) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    result.geometry = geometry;
    RecordView view{.bytes = result.latest.bytes, .geometry = &result.geometry};
    InitializeEmptyRecord(geometry, legacy->generation, legacy->allocation_cursor, view);
    // v1 records live in the first 16 KiB, so pretend the imported generation
    // occupied v3 Bank 0: its successor lands at 16 KiB, beyond all four v1
    // records. v2 banks share the v3 ring and keep their index.
    view.header().bank_index = format_version == MICROPIXEL_BUNDLEFS_V1_FORMAT_VERSION ? 0U : legacy->bank_index;
    for (uint32_t index = 0U; index < legacy->file_count; ++index) {
        const LegacyEntry& entry = legacy->entries[index];
        std::array<uint32_t, MICROPIXEL_BUNDLEFS_LEGACY_MAX_DATA_BLOCKS> blocks{};
        for (uint32_t block_index = 0U; block_index < entry.block_count; ++block_index) {
            blocks[block_index] = legacy->block_map[entry.block_map_offset + block_index];
        }
        if (AppendEntry(view, entry.name.data(), entry.size, entry.content_id, entry.sha256.data(), blocks.data(),
                        entry.block_count) != BUNDLEFS_OK) {
            result.unsupported = true;
            return BUNDLEFS_OK;
        }
    }
    result.has_latest = true;
    ESP_LOGI(kTag, "mounted legacy v%u Catalog: generation=%" PRIu64 " files=%u; migration pending",
             static_cast<unsigned>(format_version), legacy->generation, static_cast<unsigned>(legacy->file_count));
    return BUNDLEFS_OK;
}

// Reads the header at offset 0 and returns its bank size when it looks like a
// v3 record, so a store formatted with a larger bank is found even though the
// planned geometry for this medium would put banks elsewhere.
uint32_t BankSizeHint(device::BlockStorage& storage) {
    alignas(8) std::array<uint8_t, kHeaderSize> bytes{};
    if (ReadStorage(storage, 0U, bytes.data(), bytes.size()) != BUNDLEFS_OK) {
        return 0U;
    }
    CatalogHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const device::BlockStorageGeometry& medium = storage.geometry();
    if (header.magic != kCatalogMagic || header.format_version != MICROPIXEL_BUNDLEFS_FORMAT_VERSION ||
        header.bank_size == 0U || medium.erase_size == 0U || (header.bank_size % medium.erase_size) != 0U ||
        static_cast<uint64_t>(header.bank_size) * kBankCount > medium.size_bytes) {
        return 0U;
    }
    return header.bank_size;
}

// Programs `record` into `bank` in two steps (body, then commit marker),
// verifying each, and returns the verified read-back in `verified`.
bundlefs_error_t WriteCatalogBank(device::BlockStorage& storage, const BundleFsGeometry& geometry, uint32_t bank,
                                  RecordView& record, ScopedBytes& verified) {
    if (bank >= kBankCount) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const uint64_t destination = static_cast<uint64_t>(bank) * geometry.bank_size;
    if (EraseStorage(storage, destination, geometry.bank_size) != BUNDLEFS_OK) {
        return BUNDLEFS_ERR_IO;
    }
    const uint32_t body_size = geometry.record_size - kTrailerSize;
    record.header().bank_index = static_cast<uint16_t>(bank);
    record.checksum() = 0U;
    record.commit_marker() = kErasedWord;
    record.checksum() = BodyChecksum(record.bytes, body_size);
    if (ProgramStorage(storage, destination, record.bytes, body_size + sizeof(uint32_t)) != BUNDLEFS_OK) {
        return BUNDLEFS_ERR_IO;
    }
    if (!Reserve(verified, geometry.record_size)) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    if (ReadStorage(storage, destination, verified.bytes, geometry.record_size) != BUNDLEFS_OK ||
        std::memcmp(record.bytes, verified.bytes, body_size + sizeof(uint32_t)) != 0) {
        ESP_LOGE(kTag, "Catalog bank %" PRIu32 " read-back mismatch before commit", bank);
        return BUNDLEFS_ERR_IO;
    }
    if (ProgramStorage(storage, destination + geometry.record_size - sizeof(uint32_t), &kCommitMarker,
                       sizeof(kCommitMarker)) != BUNDLEFS_OK) {
        ESP_LOGE(kTag, "Catalog bank %" PRIu32 " commit marker program failed", bank);
        return BUNDLEFS_ERR_COMMIT;
    }
    ScopedBytes used_bits(UsedBitsSize(geometry.data_block_count));
    if (!used_bits.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    const RecordView check{.bytes = verified.bytes, .geometry = &geometry};
    if (ReadStorage(storage, destination, verified.bytes, geometry.record_size) != BUNDLEFS_OK ||
        !GeometryFromHeader(check.header(), storage.geometry(), geometry.data_block_size).valid() ||
        !HasCommittedChecksum(check) || !ValidRecordPayload(check, used_bits) || check.header().bank_index != bank) {
        ESP_LOGE(kTag, "Catalog bank %" PRIu32 " did not validate after commit", bank);
        return BUNDLEFS_ERR_COMMIT;
    }
    // Journaled media persist in issue order, so one barrier after the marker
    // makes the whole generation (data blocks, record, marker) durable.
    if (!storage.Sync()) {
        ESP_LOGE(kTag, "Catalog bank %" PRIu32 " durability barrier failed", bank);
        return BUNDLEFS_ERR_COMMIT;
    }
    return BUNDLEFS_OK;
}

FileState FileFromEntry(uint32_t store_tag, uint64_t generation, uint32_t entry_index, const CatalogEntry& entry) {
    FileState file{};
    file.magic = kFileHandleMagic;
    file.store_tag = store_tag;
    file.kind = kFileCommitted;
    file.size = entry.size;
    file.content_id = entry.content_id;
    file.block_count = entry.block_count;
    file.entry_hint = entry_index;
    file.generation = generation;
    std::snprintf(file.name.data(), file.name.size(), "%s", entry.name.data());
    return file;
}

void StoreFile(const FileState& state, bundlefs_file_t& file) {
    file = {};
    std::memcpy(file.opaque, &state, sizeof(state));
}

bool LoadFile(uint32_t store_tag, const bundlefs_file_t& file, FileState& state) {
    std::memcpy(&state, file.opaque, sizeof(state));
    return state.magic == kFileHandleMagic && state.store_tag == store_tag && ValidName(state.name.data()) &&
           state.size > 0U && state.block_count > 0U && (state.kind == kFileCommitted || state.kind == kFileStaged);
}

void StoreWriter(const WriterState& state, bundlefs_writer_t& writer) {
    writer = {};
    std::memcpy(writer.opaque, &state, sizeof(state));
}

bool LoadWriter(uint32_t store_tag, const bundlefs_writer_t& writer, WriterState& state) {
    std::memcpy(&state, writer.opaque, sizeof(state));
    return state.magic == kWriterHandleMagic && state.file.magic == kFileHandleMagic &&
           state.file.store_tag == store_tag && state.file.kind == kFileStaged;
}

uint32_t StoreTag(const void* instance) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(instance)); }

#ifndef CONFIG_MICROPIXEL_BUNDLEFS_BLOCK_MAP_BUDGET_KIB
#define CONFIG_MICROPIXEL_BUNDLEFS_BLOCK_MAP_BUDGET_KIB 1024
#endif
constexpr uint64_t kBlockMapBudgetBytes =
    static_cast<uint64_t>(CONFIG_MICROPIXEL_BUNDLEFS_BLOCK_MAP_BUDGET_KIB) * 1024U;

}  // namespace

// ---------------------------------------------------------------------------
// Geometry planning
// ---------------------------------------------------------------------------

uint32_t BundleFs::DefaultDataBlockSize(const device::BlockStorageGeometry& geometry) {
    if (geometry.erase_size == 0U || !IsPowerOfTwo(geometry.erase_size) ||
        (geometry.mappable && !IsPowerOfTwo(geometry.map_alignment))) {
        return 0U;
    }
    // The medium's own unit: erase sector, or the MMU page when it maps.
    uint32_t block_size = geometry.erase_size;
    if (geometry.mappable) {
        block_size = std::max(block_size, geometry.map_alignment);
    }
    // Grow only while the block map (4 bytes per block, resident while
    // mounted) would exceed the configured RAM budget.
    while (block_size != 0U && (geometry.size_bytes / block_size) * sizeof(uint32_t) > kBlockMapBudgetBytes) {
        block_size <<= 1U;
    }
    return block_size != 0U && ValidBlockSize(geometry, block_size) ? block_size : 0U;
}

BundleFsGeometry BundleFs::PlanGeometry(const device::BlockStorageGeometry& storage, uint32_t data_block_size) {
    BundleFsGeometry geometry{};
    const uint32_t block_size = data_block_size == 0U ? DefaultDataBlockSize(storage) : data_block_size;
    if (!ValidBlockSize(storage, block_size)) {
        return geometry;
    }
    const uint32_t entry_capacity = BUNDLEFS_MAX_FILES;
    // Banks hold one record each and are erase-aligned; the metadata area is
    // four banks rounded up to the data block. Iterate because the block count
    // depends on the metadata size and vice versa; it converges immediately in
    // practice since the record only shrinks as the metadata area grows.
    uint64_t data_offset = RoundUp(std::max<uint64_t>(MICROPIXEL_BUNDLEFS_METADATA_SIZE, block_size), block_size);
    for (uint32_t iteration = 0U; iteration < 8U; ++iteration) {
        if (data_offset >= storage.size_bytes) {
            return {};
        }
        const uint64_t data_blocks = (storage.size_bytes - data_offset) / block_size;
        if (data_blocks == 0U || data_blocks > UINT32_MAX) {
            return {};
        }
        const uint64_t record_size = RecordSizeFor(entry_capacity, static_cast<uint32_t>(data_blocks));
        const uint64_t bank_size =
            RoundUp(std::max<uint64_t>(record_size, MICROPIXEL_BUNDLEFS_BANK_SIZE), storage.erase_size);
        const uint64_t required_offset = RoundUp(bank_size * kBankCount, block_size);
        if (bank_size > UINT32_MAX || required_offset > UINT32_MAX) {
            return {};
        }
        if (required_offset <= data_offset) {
            geometry.size_bytes = storage.size_bytes;
            geometry.data_block_size = block_size;
            geometry.bank_size = static_cast<uint32_t>(bank_size);
            geometry.data_offset = static_cast<uint32_t>(data_offset);
            geometry.data_block_count = static_cast<uint32_t>(data_blocks);
            geometry.entry_capacity = entry_capacity;
            geometry.record_size = static_cast<uint32_t>(record_size);
            return geometry;
        }
        data_offset = required_offset;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

BundleFs::BundleFs(device::BlockStorage& storage, uint32_t data_block_size)
    : storage_(storage), requested_block_size_(data_block_size) {
    if (!PlanGeometry(storage.geometry(), data_block_size).valid()) {
        const device::BlockStorageGeometry& geometry = storage.geometry();
        ESP_LOGE(kTag,
                 "unsupported storage geometry: size=%" PRIu64 " erase=%" PRIu32 " program=%" PRIu32 " block=%" PRIu32,
                 geometry.size_bytes, geometry.erase_size, geometry.program_size, data_block_size);
    }
}

BundleFs::~BundleFs() {
    heap_caps_free(catalog_.bytes);
    heap_caps_free(writer_blocks_.bytes);
}

uint32_t BundleFs::data_block_size() const {
    return mounted_ ? geometry_.data_block_size
                    : PlanGeometry(storage_.geometry(), requested_block_size_).data_block_size;
}

bundlefs_error_t BundleFs::LoadLocked() {
    ScanResult scan{};
    const BundleFsGeometry planned = PlanGeometry(storage_.geometry(), requested_block_size_);
    bundlefs_error_t error = BUNDLEFS_OK;
    const uint32_t hinted_bank = BankSizeHint(storage_);
    if (hinted_bank != 0U) {
        error = ScanBanks(storage_, hinted_bank, requested_block_size_, scan);
    }
    if (error == BUNDLEFS_OK && !scan.has_latest && planned.valid() && planned.bank_size != hinted_bank) {
        error = ScanBanks(storage_, planned.bank_size, requested_block_size_, scan);
    }
    if (error == BUNDLEFS_OK && !scan.has_latest && MICROPIXEL_BUNDLEFS_V2_BANK_SIZE != hinted_bank &&
        (!planned.valid() || planned.bank_size != MICROPIXEL_BUNDLEFS_V2_BANK_SIZE)) {
        // v3 written into a legacy 16 KiB ring by an in-place migration.
        error = ScanBanks(storage_, MICROPIXEL_BUNDLEFS_V2_BANK_SIZE, requested_block_size_, scan);
    }
    if (error == BUNDLEFS_OK && !scan.has_latest) {
        error = ScanLegacy<CatalogRecordV2>(storage_, MICROPIXEL_BUNDLEFS_V2_FORMAT_VERSION,
                                            MICROPIXEL_BUNDLEFS_V2_BANK_SIZE, requested_block_size_, scan);
    }
    if (error == BUNDLEFS_OK && !scan.has_latest) {
        error = ScanLegacy<CatalogRecordV1>(storage_, MICROPIXEL_BUNDLEFS_V1_FORMAT_VERSION,
                                            MICROPIXEL_BUNDLEFS_V1_BANK_SIZE, requested_block_size_, scan);
    }
    if (error != BUNDLEFS_OK) {
        return error;
    }
    if (scan.divergent) {
        return BUNDLEFS_ERR_CORRUPT;
    }
    if (scan.has_latest) {
        heap_caps_free(catalog_.bytes);
        catalog_ = Buffer{.bytes = scan.latest.release(), .size = scan.geometry.record_size};
        geometry_ = scan.geometry;
        mounted_ = true;
        return BUNDLEFS_OK;
    }
    if (scan.unsupported) {
        return BUNDLEFS_ERR_UNSUPPORTED_FORMAT;
    }
    if (scan.any_non_erased) {
        return scan.any_bundlefs ? BUNDLEFS_ERR_CORRUPT : BUNDLEFS_ERR_NOT_FORMATTED;
    }
    if (!planned.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    // Fully erased medium: commit generation 1 with the planned geometry.
    ScopedBytes empty(planned.record_size);
    ScopedBytes verified;
    if (!empty.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    RecordView record{.bytes = empty.bytes, .geometry = &planned};
    InitializeEmptyRecord(planned, 1U, 0U, record);
    error = WriteCatalogBank(storage_, planned, 0U, record, verified);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    heap_caps_free(catalog_.bytes);
    catalog_ = Buffer{.bytes = verified.release(), .size = planned.record_size};
    geometry_ = planned;
    mounted_ = true;
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::EnsureMountedLocked() { return mounted_ ? BUNDLEFS_OK : LoadLocked(); }

bundlefs_error_t BundleFs::Mount() {
    const std::lock_guard lock(mutex_);
    const bundlefs_error_t error = LoadLocked();
    if (error == BUNDLEFS_OK) {
        const RecordView record{.bytes = catalog_.bytes, .geometry = &geometry_};
        const CatalogHeader& header = record.header();
        ESP_LOGI(kTag,
                 "mounted: generation=%" PRIu64 " files=%u blocks=%" PRIu32 "/%" PRIu32 " block=%" PRIu32 " KiB %s",
                 header.generation, static_cast<unsigned>(header.file_count), header.block_map_count,
                 geometry_.data_block_count, geometry_.data_block_size / 1024U, mappable() ? "mappable" : "read-only");
    }
    return error;
}

bundlefs_error_t BundleFs::Format() {
    const std::lock_guard lock(mutex_);
    if (writer_active_) {
        return BUNDLEFS_ERR_BUSY;
    }
    const BundleFsGeometry planned = PlanGeometry(storage_.geometry(), requested_block_size_);
    if (!planned.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    mounted_ = false;
    // Data blocks are erased when allocated; the metadata area is what makes
    // the medium a BundleFS, so that is what Format() resets.
    if (EraseStorage(storage_, 0U, planned.data_offset) != BUNDLEFS_OK) {
        return BUNDLEFS_ERR_IO;
    }
    ScopedBytes empty(planned.record_size);
    ScopedBytes verified;
    if (!empty.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    RecordView record{.bytes = empty.bytes, .geometry = &planned};
    InitializeEmptyRecord(planned, 1U, 0U, record);
    const bundlefs_error_t error = WriteCatalogBank(storage_, planned, 0U, record, verified);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    heap_caps_free(catalog_.bytes);
    catalog_ = Buffer{.bytes = verified.release(), .size = planned.record_size};
    geometry_ = planned;
    mounted_ = true;
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::GetStoreInfo(bundlefs_store_info_t& info_out) {
    const std::lock_guard lock(mutex_);
    const bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const RecordView record{.bytes = catalog_.bytes, .geometry = &geometry_};
    const CatalogHeader& header = record.header();
    const uint64_t free =
        static_cast<uint64_t>(geometry_.data_block_count - header.block_map_count) * geometry_.data_block_size;
    info_out = bundlefs_store_info_t{
        .data_block_size = geometry_.data_block_size,
        .total_bytes = geometry_.size_bytes,
        .used_bytes = geometry_.size_bytes - free,
        .free_bytes = free,
        .total_blocks = geometry_.data_block_count,
        .used_blocks = header.block_map_count,
        .file_count = header.file_count,
    };
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::List(bundlefs_file_info_t* files_out, uint32_t capacity, uint32_t& count_out) {
    if (files_out == nullptr || capacity == 0U) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const std::lock_guard lock(mutex_);
    const bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const RecordView record{.bytes = catalog_.bytes, .geometry = &geometry_};
    const CatalogHeader& header = record.header();
    if (capacity < header.file_count) {
        return BUNDLEFS_ERR_NO_SPACE;
    }
    count_out = header.file_count;
    const CatalogEntry* entries = record.entries();
    for (uint32_t index = 0U; index < header.file_count; ++index) {
        files_out[index] = {};
        std::snprintf(files_out[index].name, sizeof(files_out[index].name), "%s", entries[index].name.data());
        files_out[index].size = entries[index].size;
        files_out[index].content_id = entries[index].content_id;
        std::copy(entries[index].sha256.begin(), entries[index].sha256.end(), files_out[index].sha256);
    }
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::GetFileSha256(const char* name, uint8_t sha256_out[BUNDLEFS_SHA256_SIZE]) {
    if (!ValidName(name) || sha256_out == nullptr) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const std::lock_guard lock(mutex_);
    const bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const RecordView record{.bytes = catalog_.bytes, .geometry = &geometry_};
    const CatalogEntry* entry = FindEntry(record, name);
    if (entry == nullptr) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    std::copy(entry->sha256.begin(), entry->sha256.end(), sha256_out);
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::Open(const char* name, bundlefs_file_t& file_out) {
    if (!ValidName(name)) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const std::lock_guard lock(mutex_);
    const bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const RecordView record{.bytes = catalog_.bytes, .geometry = &geometry_};
    uint32_t index = 0U;
    const CatalogEntry* entry = FindEntry(record, name, &index);
    if (entry == nullptr) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    StoreFile(FileFromEntry(StoreTag(this), record.header().generation, index, *entry), file_out);
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::GetFileInfo(const bundlefs_file_t& file, bundlefs_file_info_t& info_out) {
    FileState state;
    if (!LoadFile(StoreTag(this), file, state)) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    info_out = {};
    std::snprintf(info_out.name, sizeof(info_out.name), "%s", state.name.data());
    info_out.size = state.size;
    info_out.content_id = state.content_id;
    return BUNDLEFS_OK;
}

// Translates one logical block of `file` to its physical block. Committed
// files are resolved through the cached Catalog so a handle stays valid
// across commits until its file is replaced or removed; staged files use the
// active writer's allocation.
bundlefs_error_t BundleFs::ResolveBlockLocked(const bundlefs_file_t& file, uint32_t logical_block,
                                              uint32_t& physical_out, uint32_t& file_size_out) {
    FileState state;
    if (!LoadFile(StoreTag(this), file, state) || logical_block >= state.block_count) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    file_size_out = state.size;
    if (state.kind == kFileStaged) {
        if (!writer_active_ || state.generation != writer_generation_ || logical_block >= writer_block_count_) {
            return BUNDLEFS_ERR_NOT_FOUND;
        }
        std::memcpy(&physical_out, writer_blocks_.bytes + static_cast<size_t>(logical_block) * sizeof(uint32_t),
                    sizeof(physical_out));
        return BUNDLEFS_OK;
    }
    const bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const RecordView record{.bytes = catalog_.bytes, .geometry = &geometry_};
    const CatalogHeader& header = record.header();
    const CatalogEntry* entry = nullptr;
    if (state.entry_hint < header.file_count &&
        std::strcmp(record.entries()[state.entry_hint].name.data(), state.name.data()) == 0) {
        entry = &record.entries()[state.entry_hint];
    } else {
        entry = FindEntry(record, state.name.data());
    }
    if (entry == nullptr || entry->content_id != state.content_id || entry->size != state.size ||
        logical_block >= entry->block_count) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    physical_out = record.block_map()[entry->block_map_offset + logical_block];
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::ReadFileLocked(const bundlefs_file_t& file, uint32_t offset, void* destination,
                                          uint32_t size) {
    FileState state;
    if (!LoadFile(StoreTag(this), file, state) || destination == nullptr || offset > state.size ||
        size > state.size - offset) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    if (!mounted_ && state.kind == kFileCommitted) {
        const bundlefs_error_t error = EnsureMountedLocked();
        if (error != BUNDLEFS_OK) {
            return error;
        }
    }
    const uint32_t block_size = geometry_.data_block_size;
    uint8_t* output = static_cast<uint8_t*>(destination);
    uint32_t consumed = 0U;
    while (consumed < size) {
        const uint32_t current = offset + consumed;
        const uint32_t within_block = current % block_size;
        const uint32_t chunk = std::min(size - consumed, block_size - within_block);
        uint32_t physical = 0U;
        uint32_t file_size = 0U;
        const bundlefs_error_t error = ResolveBlockLocked(file, current / block_size, physical, file_size);
        if (error != BUNDLEFS_OK) {
            return error;
        }
        if (ReadStorage(storage_, BlockOffset(geometry_, physical) + within_block, output + consumed, chunk) !=
            BUNDLEFS_OK) {
            return BUNDLEFS_ERR_IO;
        }
        consumed += chunk;
    }
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::Read(const bundlefs_file_t& file, uint32_t offset, void* destination, uint32_t size) {
    const std::lock_guard lock(mutex_);
    return ReadFileLocked(file, offset, destination, size);
}

bundlefs_error_t BundleFs::Map(const bundlefs_file_t& file, uint32_t offset, uint32_t size,
                               bundlefs_mapping_t& mapping_out) {
    const std::lock_guard lock(mutex_);
    FileState state;
    if (!LoadFile(StoreTag(this), file, state) || size == 0U || offset > state.size || size > state.size - offset) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    if (!storage_.geometry().mappable) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    const bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const uint32_t block_size = geometry_.data_block_size;
    // One immutable file has one mapping identity, regardless of the requested section.
    const uint32_t block_count = BlocksFor(block_size, state.size);
    ScopedBytes offsets(block_count * static_cast<uint32_t>(sizeof(uint64_t)));
    if (!offsets.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    auto* block_offsets = reinterpret_cast<uint64_t*>(offsets.bytes);
    for (uint32_t block_index = 0U; block_index < block_count; ++block_index) {
        uint32_t physical = 0U;
        uint32_t file_size = 0U;
        const bundlefs_error_t resolve_error = ResolveBlockLocked(file, block_index, physical, file_size);
        if (resolve_error != BUNDLEFS_OK) {
            return resolve_error;
        }
        block_offsets[block_index] = BlockOffset(geometry_, physical);
    }
    auto mapping = storage_.Map(std::span<const uint64_t>(block_offsets, block_count), block_size);
    if (!mapping) {
        ESP_LOGE(kTag, "Storage mapping failed: blocks=%" PRIu32 " bytes=%" PRIu32 " error=%u", block_count, size,
                 static_cast<unsigned>(mapping.error()));
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    mapping_out = bundlefs_mapping_t{
        .data = static_cast<const uint8_t*>(mapping->data) + offset,
        .mapping = mapping->data,
        .size = size,
        .mapping_handle = mapping->handle,
    };
    return BUNDLEFS_OK;
}

void BundleFs::Unmap(bundlefs_mapping_t& mapping) {
    if (mapping.mapping != nullptr) {
        device::BlockStorageMapping storage_mapping{.data = mapping.mapping, .handle = mapping.mapping_handle};
        storage_.Unmap(storage_mapping);
    }
    mapping = {};
}

void BundleFs::ReleaseWriterLocked() {
    heap_caps_free(writer_blocks_.bytes);
    writer_blocks_ = {};
    writer_block_count_ = 0U;
    writer_active_ = false;
}

bundlefs_error_t BundleFs::BeginReplace(const char* name, uint32_t size, bundlefs_writer_t& writer_out) {
    if (!ValidName(name) || size == 0U) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const std::lock_guard lock(mutex_);
    bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    if (writer_active_) {
        return BUNDLEFS_ERR_BUSY;
    }
    const RecordView record{.bytes = catalog_.bytes, .geometry = &geometry_};
    const CatalogHeader& header = record.header();
    if (FindEntry(record, name) == nullptr && header.file_count >= geometry_.entry_capacity) {
        return BUNDLEFS_ERR_TOO_MANY_FILES;
    }
    const uint32_t required = BlocksFor(geometry_.data_block_size, size);
    if (required > geometry_.data_block_count - header.block_map_count) {
        return BUNDLEFS_ERR_NO_SPACE;
    }
    ScopedBytes used_bits(UsedBitsSize(geometry_.data_block_count));
    ScopedBytes blocks(required * static_cast<uint32_t>(sizeof(uint32_t)));
    if (!used_bits.valid() || !blocks.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    std::fill_n(used_bits.bytes, used_bits.size, static_cast<uint8_t>(0U));
    const uint32_t* block_map = record.block_map();
    for (uint32_t index = 0U; index < header.block_map_count; ++index) {
        used_bits.bytes[block_map[index] / 8U] |= static_cast<uint8_t>(1U << (block_map[index] % 8U));
    }
    auto* allocated_blocks = reinterpret_cast<uint32_t*>(blocks.bytes);
    uint32_t allocated = 0U;
    for (uint32_t scanned = 0U; scanned < geometry_.data_block_count && allocated < required; ++scanned) {
        const uint32_t block = static_cast<uint32_t>((static_cast<uint64_t>(header.allocation_cursor) + scanned) %
                                                     geometry_.data_block_count);
        if ((used_bits.bytes[block / 8U] & (1U << (block % 8U))) == 0U) {
            allocated_blocks[allocated++] = block;
        }
    }
    if (allocated != required) {
        return BUNDLEFS_ERR_NO_SPACE;
    }
    for (uint32_t index = 0U; index < required; ++index) {
        if (EraseStorage(storage_, BlockOffset(geometry_, allocated_blocks[index]), geometry_.data_block_size) !=
            BUNDLEFS_OK) {
            return BUNDLEFS_ERR_IO;
        }
    }
    WriterState writer{};
    writer.magic = kWriterHandleMagic;
    writer.base_generation = header.generation;
    writer.allocation_cursor = static_cast<uint32_t>((static_cast<uint64_t>(allocated_blocks[required - 1U]) + 1U) %
                                                     geometry_.data_block_count);
    writer.file.magic = kFileHandleMagic;
    writer.file.store_tag = StoreTag(this);
    writer.file.kind = kFileStaged;
    writer.file.size = size;
    writer.file.block_count = required;
    writer.file.generation = header.generation + 1U;
    std::snprintf(writer.file.name.data(), writer.file.name.size(), "%s", name);

    writer_blocks_ = Buffer{.bytes = blocks.release(), .size = required * static_cast<uint32_t>(sizeof(uint32_t))};
    writer_block_count_ = required;
    writer_generation_ = writer.file.generation;
    writer_active_ = true;
    StoreWriter(writer, writer_out);
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::Write(bundlefs_writer_t& writer_handle, const void* data, uint32_t size) {
    const std::lock_guard lock(mutex_);
    WriterState writer;
    if (!LoadWriter(StoreTag(this), writer_handle, writer) || !writer_active_ ||
        writer.file.generation != writer_generation_ || data == nullptr || writer.written > writer.file.size ||
        size > writer.file.size - writer.written) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    bundlefs_file_t staged{};
    StoreFile(writer.file, staged);
    const uint32_t block_size = geometry_.data_block_size;
    const uint8_t* input = static_cast<const uint8_t*>(data);
    uint32_t consumed = 0U;
    while (consumed < size) {
        const uint32_t current = writer.written + consumed;
        const uint32_t within_block = current % block_size;
        const uint32_t chunk = std::min(size - consumed, block_size - within_block);
        uint32_t physical = 0U;
        uint32_t file_size = 0U;
        const bundlefs_error_t error = ResolveBlockLocked(staged, current / block_size, physical, file_size);
        if (error != BUNDLEFS_OK) {
            return error;
        }
        if (ProgramStorage(storage_, BlockOffset(geometry_, physical) + within_block, input + consumed, chunk) !=
            BUNDLEFS_OK) {
            return BUNDLEFS_ERR_IO;
        }
        consumed += chunk;
    }
    writer.written += size;
    StoreWriter(writer, writer_handle);
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::OpenStaged(const bundlefs_writer_t& writer_handle, bundlefs_file_t& file_out) {
    const std::lock_guard lock(mutex_);
    WriterState writer;
    if (!LoadWriter(StoreTag(this), writer_handle, writer) || !writer_active_ ||
        writer.file.generation != writer_generation_ || writer.written != writer.file.size) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    StoreFile(writer.file, file_out);
    return BUNDLEFS_OK;
}

// Writes `next` into the bank after the current one and, on success, makes
// the verified read-back the cached Catalog.
bundlefs_error_t BundleFs::CommitRecordLocked(Buffer& next) {
    const RecordView current{.bytes = catalog_.bytes, .geometry = &geometry_};
    RecordView record{.bytes = next.bytes, .geometry = &geometry_};
    if (record.header().generation != current.header().generation + 1U) {
        return BUNDLEFS_ERR_CONFLICT;
    }
    const uint32_t next_bank = (static_cast<uint32_t>(current.header().bank_index) + 1U) % kBankCount;
    ScopedBytes verified;
    const bundlefs_error_t error = WriteCatalogBank(storage_, geometry_, next_bank, record, verified);
    if (error != BUNDLEFS_OK) {
        // The medium may now hold a partial bank; drop the cache so the next
        // operation re-scans and falls back to the previous generation.
        mounted_ = false;
        return error;
    }
    heap_caps_free(catalog_.bytes);
    catalog_ = Buffer{.bytes = verified.release(), .size = geometry_.record_size};
    return BUNDLEFS_OK;
}

bundlefs_error_t BundleFs::Commit(bundlefs_writer_t& writer_handle,
                                  const uint8_t expected_sha256[BUNDLEFS_SHA256_SIZE]) {
    const std::lock_guard lock(mutex_);
    WriterState writer;
    if (!LoadWriter(StoreTag(this), writer_handle, writer) || !writer_active_ ||
        writer.file.generation != writer_generation_ || expected_sha256 == nullptr ||
        writer.written != writer.file.size) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    bundlefs_file_t staged{};
    StoreFile(writer.file, staged);

    ScopedBytes chunk_buffer(kFlashChunkSize);
    if (!chunk_buffer.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    std::array<uint8_t, BUNDLEFS_SHA256_SIZE> digest{};
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    bool valid = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (uint32_t offset = 0U; valid && offset < writer.file.size;) {
        const uint32_t chunk = std::min<uint32_t>(kFlashChunkSize, writer.file.size - offset);
        valid = ReadFileLocked(staged, offset, chunk_buffer.bytes, chunk) == BUNDLEFS_OK &&
                psa_hash_update(&operation, chunk_buffer.bytes, chunk) == PSA_SUCCESS;
        offset += chunk;
    }
    size_t digest_size = 0U;
    valid = valid && psa_hash_finish(&operation, digest.data(), digest.size(), &digest_size) == PSA_SUCCESS &&
            digest_size == digest.size();
    (void)psa_hash_abort(&operation);
    if (!valid) {
        return BUNDLEFS_ERR_IO;
    }
    if (!std::equal(digest.begin(), digest.end(), expected_sha256)) {
        return BUNDLEFS_ERR_HASH_MISMATCH;
    }

    bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const RecordView current{.bytes = catalog_.bytes, .geometry = &geometry_};
    if (current.header().generation != writer.base_generation) {
        return BUNDLEFS_ERR_CONFLICT;
    }
    ScopedBytes next_bytes(geometry_.record_size);
    if (!next_bytes.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    RecordView next{.bytes = next_bytes.bytes, .geometry = &geometry_};
    InitializeEmptyRecord(geometry_, current.header().generation + 1U, writer.allocation_cursor, next);
    const auto* staged_blocks = reinterpret_cast<const uint32_t*>(writer_blocks_.bytes);
    const bool replacing = FindEntry(current, writer.file.name.data()) != nullptr;
    if (!replacing) {
        error = AppendEntry(next, writer.file.name.data(), writer.file.size, ContentId(digest.data()), digest.data(),
                            staged_blocks, writer.file.block_count);
        if (error != BUNDLEFS_OK) {
            return error;
        }
    }
    const CatalogEntry* entries = current.entries();
    for (uint32_t index = 0U; index < current.header().file_count; ++index) {
        const CatalogEntry& entry = entries[index];
        if (std::strcmp(entry.name.data(), writer.file.name.data()) == 0) {
            error = AppendEntry(next, writer.file.name.data(), writer.file.size, ContentId(digest.data()),
                                digest.data(), staged_blocks, writer.file.block_count);
        } else {
            error = AppendExisting(next, current, entry);
        }
        if (error != BUNDLEFS_OK) {
            return error;
        }
    }
    Buffer next_buffer{.bytes = next_bytes.bytes, .size = geometry_.record_size};
    error = CommitRecordLocked(next_buffer);
    if (error != BUNDLEFS_OK) {
        return error;
    }
    writer_handle = {};
    ReleaseWriterLocked();
    return BUNDLEFS_OK;
}

void BundleFs::Abort(bundlefs_writer_t& writer_handle) {
    const std::lock_guard lock(mutex_);
    WriterState writer;
    if (LoadWriter(StoreTag(this), writer_handle, writer) && writer_active_ &&
        writer.file.generation == writer_generation_) {
        writer_handle = {};
        ReleaseWriterLocked();
    }
}

bundlefs_error_t BundleFs::Remove(const char* name) {
    if (!ValidName(name)) {
        return BUNDLEFS_ERR_INVALID_ARGUMENT;
    }
    const std::lock_guard lock(mutex_);
    if (writer_active_) {
        return BUNDLEFS_ERR_BUSY;
    }
    bundlefs_error_t error = EnsureMountedLocked();
    if (error != BUNDLEFS_OK) {
        return error;
    }
    const RecordView current{.bytes = catalog_.bytes, .geometry = &geometry_};
    if (FindEntry(current, name) == nullptr) {
        return BUNDLEFS_ERR_NOT_FOUND;
    }
    ScopedBytes next_bytes(geometry_.record_size);
    if (!next_bytes.valid()) {
        return BUNDLEFS_ERR_UNAVAILABLE;
    }
    RecordView next{.bytes = next_bytes.bytes, .geometry = &geometry_};
    InitializeEmptyRecord(geometry_, current.header().generation + 1U, current.header().allocation_cursor, next);
    const CatalogEntry* entries = current.entries();
    for (uint32_t index = 0U; index < current.header().file_count; ++index) {
        if (std::strcmp(entries[index].name.data(), name) != 0) {
            error = AppendExisting(next, current, entries[index]);
            if (error != BUNDLEFS_OK) {
                return error;
            }
        }
    }
    Buffer next_buffer{.bytes = next_bytes.bytes, .size = geometry_.record_size};
    return CommitRecordLocked(next_buffer);
}

}  // namespace micropixel::runtime
