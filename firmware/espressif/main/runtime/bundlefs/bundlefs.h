#ifndef MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_H
#define MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Plain C value types shared by every Bundle store implementation. The store
// operations themselves live behind runtime::BundleStore (bundle_store.hpp).

// Catalog entry capacity written by Format(). The Host catalog and App Hall
// arrays are sized from it; a store formatted with a larger capacity still
// mounts, List() simply reports NO_SPACE for a smaller caller array.
#define BUNDLEFS_MAX_FILES 50U
#define BUNDLEFS_MAX_NAME_LENGTH 64U
#define BUNDLEFS_SHA256_SIZE 32U
// Handles identify a file; block indices stay in the store's cached Catalog
// (or the active writer), so a file may span any number of data blocks.
#define BUNDLEFS_FILE_HANDLE_WORDS 32U
#define BUNDLEFS_WRITER_HANDLE_WORDS 40U

typedef enum bundlefs_error {
    BUNDLEFS_OK = 0,
    BUNDLEFS_ERR_UNAVAILABLE,
    BUNDLEFS_ERR_UNSUPPORTED_FORMAT,
    BUNDLEFS_ERR_CORRUPT,
    BUNDLEFS_ERR_INVALID_ARGUMENT,
    BUNDLEFS_ERR_NOT_FOUND,
    BUNDLEFS_ERR_EXISTS,
    BUNDLEFS_ERR_TOO_MANY_FILES,
    BUNDLEFS_ERR_NO_SPACE,
    BUNDLEFS_ERR_BUSY,
    BUNDLEFS_ERR_CONFLICT,
    BUNDLEFS_ERR_IO,
    BUNDLEFS_ERR_HASH_MISMATCH,
    BUNDLEFS_ERR_COMMIT,
    // The medium holds data that is not BundleFS (e.g. a factory FAT image);
    // Format() is required before use.
    BUNDLEFS_ERR_NOT_FORMATTED,
} bundlefs_error_t;

typedef struct bundlefs_file {
    uint32_t opaque[BUNDLEFS_FILE_HANDLE_WORDS];
} bundlefs_file_t;

typedef struct bundlefs_writer {
    uint32_t opaque[BUNDLEFS_WRITER_HANDLE_WORDS];
} bundlefs_writer_t;

typedef struct bundlefs_file_info {
    char name[BUNDLEFS_MAX_NAME_LENGTH + 1U];
    uint32_t size;
    uint32_t content_id;
    uint8_t sha256[BUNDLEFS_SHA256_SIZE];
} bundlefs_file_info_t;

typedef struct bundlefs_store_info {
    uint32_t data_block_size;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t total_blocks;
    uint32_t used_blocks;
    uint32_t file_count;
} bundlefs_store_info_t;

typedef struct bundlefs_mapping {
    const uint8_t* data;
    const void* mapping;
    uint32_t size;
    uint32_t mapping_handle;
} bundlefs_mapping_t;

#ifdef __cplusplus
}
#endif

#endif
