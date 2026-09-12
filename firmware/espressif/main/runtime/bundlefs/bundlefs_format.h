#ifndef MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_FORMAT_H
#define MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_FORMAT_H

#include <stdint.h>

// BundleFS v3 Catalog records are self-describing: bank size, metadata size,
// data block size, block count and entry capacity are stored in the 64-byte
// record header and validated against the medium on every mount. Nothing
// about the medium size is hard-coded; the block map grows with the medium
// (one 32-bit block index per data block) and the bank grows with the record.
#define MICROPIXEL_BUNDLEFS_FORMAT_VERSION 3U
#define MICROPIXEL_BUNDLEFS_BANK_COUNT 4U
#define MICROPIXEL_BUNDLEFS_CATALOG_HEADER_SIZE 64U
#define MICROPIXEL_BUNDLEFS_CATALOG_ENTRY_SIZE 120U
#define MICROPIXEL_BUNDLEFS_CATALOG_TRAILER_SIZE 8U
// Smallest bank and metadata area. Media whose block map fits keep these
// values, which also keeps v2 media migratable in place: a v3 record for a
// 16 KiB bank describes up to 2,600 data blocks (about 165 MiB at 64 KiB).
#define MICROPIXEL_BUNDLEFS_BANK_SIZE (16U * 1024U)
#define MICROPIXEL_BUNDLEFS_METADATA_SIZE (MICROPIXEL_BUNDLEFS_BANK_COUNT * MICROPIXEL_BUNDLEFS_BANK_SIZE)
#define MICROPIXEL_BUNDLEFS_DATA_OFFSET MICROPIXEL_BUNDLEFS_METADATA_SIZE
// There is no default data block size in the format: Format() takes it from
// the medium (erase unit / MMU page, see BundleFs::DefaultDataBlockSize) and
// Mount() takes it from the committed record header.

// Legacy records kept readable for in-place migration. Both used a fixed
// 64 KiB metadata area, 16-bit block indices, at most 383 data blocks and a
// 64 KiB data block on every shipped NOR partition.
#define MICROPIXEL_BUNDLEFS_LEGACY_DATA_BLOCK_SIZE (64U * 1024U)
#define MICROPIXEL_BUNDLEFS_V2_FORMAT_VERSION 2U
#define MICROPIXEL_BUNDLEFS_V2_BANK_SIZE (16U * 1024U)
#define MICROPIXEL_BUNDLEFS_V1_FORMAT_VERSION 1U
#define MICROPIXEL_BUNDLEFS_V1_BANK_SIZE (4U * 1024U)
#define MICROPIXEL_BUNDLEFS_V1_MAX_FILES 7U
#define MICROPIXEL_BUNDLEFS_LEGACY_MAX_DATA_BLOCKS 383U
#define MICROPIXEL_BUNDLEFS_LEGACY_ENTRY_SIZE 112U

#endif
