#ifndef MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_H
#define MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUNDLEFS_MAX_FILES 7U
#define BUNDLEFS_MAX_NAME_LENGTH 64U
#define BUNDLEFS_SHA256_SIZE 32U
#define BUNDLEFS_FILE_HANDLE_WORDS 88U
#define BUNDLEFS_WRITER_HANDLE_WORDS 96U

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
} bundlefs_file_info_t;

typedef struct bundlefs_store_info {
    uint32_t data_block_size;
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t free_bytes;
    uint16_t total_blocks;
    uint16_t used_blocks;
    uint16_t file_count;
    uint16_t reserved;
} bundlefs_store_info_t;

typedef struct bundlefs_mapping {
    const uint8_t* data;
    const void* mapping;
    uint32_t size;
    uint32_t mapping_handle;
} bundlefs_mapping_t;

bundlefs_error_t bundlefs_mount(void);
bundlefs_error_t bundlefs_format(void);
bundlefs_error_t bundlefs_get_store_info(bundlefs_store_info_t* info_out);
bundlefs_error_t bundlefs_list(bundlefs_file_info_t* files_out, uint32_t capacity, uint32_t* count_out);
bundlefs_error_t bundlefs_open(const char* name, bundlefs_file_t* file_out);
bundlefs_error_t bundlefs_get_file_info(const bundlefs_file_t* file, bundlefs_file_info_t* info_out);
bundlefs_error_t bundlefs_read(const bundlefs_file_t* file, uint32_t offset, void* destination, uint32_t size);
bundlefs_error_t bundlefs_mmap(const bundlefs_file_t* file, uint32_t offset, uint32_t size,
                               bundlefs_mapping_t* mapping_out);
void bundlefs_munmap(bundlefs_mapping_t* mapping);

bundlefs_error_t bundlefs_begin_replace(const char* name, uint32_t size, bundlefs_writer_t* writer_out);
bundlefs_error_t bundlefs_write(bundlefs_writer_t* writer, const void* data, uint32_t size);
bundlefs_error_t bundlefs_open_staged(const bundlefs_writer_t* writer, bundlefs_file_t* file_out);
bundlefs_error_t bundlefs_commit(bundlefs_writer_t* writer, const uint8_t expected_sha256[BUNDLEFS_SHA256_SIZE]);
void bundlefs_abort(bundlefs_writer_t* writer);
bundlefs_error_t bundlefs_remove(const char* name);

#ifdef __cplusplus
}
#endif

#endif
