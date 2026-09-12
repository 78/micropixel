#ifndef MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLE_STORE_HPP
#define MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLE_STORE_HPP

#include <cstdint>

#include "runtime/bundlefs/bundlefs.h"

namespace micropixel::runtime {

// One flat, immutable-file Bundle store. BundleFS on NOR or NAND implements
// it today; a LittleFS or FAT directory can implement the same surface later.
// File and writer handles are opaque values owned by the caller and only
// meaningful for the store that produced them.
class BundleStore {
   public:
    virtual ~BundleStore() = default;
    BundleStore(const BundleStore&) = delete;
    BundleStore& operator=(const BundleStore&) = delete;

    // True when Map() can expose file bytes without copying them into RAM.
    [[nodiscard]] virtual bool mappable() const = 0;  // NOLINT(readability-identifier-naming)

    [[nodiscard]] virtual bundlefs_error_t Mount() = 0;
    [[nodiscard]] virtual bundlefs_error_t Format() = 0;
    [[nodiscard]] virtual bundlefs_error_t GetStoreInfo(bundlefs_store_info_t& info_out) = 0;
    [[nodiscard]] virtual bundlefs_error_t List(bundlefs_file_info_t* files_out, uint32_t capacity,
                                                uint32_t& count_out) = 0;
    [[nodiscard]] virtual bundlefs_error_t GetFileSha256(const char* name,
                                                         uint8_t sha256_out[BUNDLEFS_SHA256_SIZE]) = 0;
    [[nodiscard]] virtual bundlefs_error_t Open(const char* name, bundlefs_file_t& file_out) = 0;
    [[nodiscard]] virtual bundlefs_error_t GetFileInfo(const bundlefs_file_t& file, bundlefs_file_info_t& info_out) = 0;
    [[nodiscard]] virtual bundlefs_error_t Read(const bundlefs_file_t& file, uint32_t offset, void* destination,
                                                uint32_t size) = 0;
    [[nodiscard]] virtual bundlefs_error_t Map(const bundlefs_file_t& file, uint32_t offset, uint32_t size,
                                               bundlefs_mapping_t& mapping_out) = 0;
    virtual void Unmap(bundlefs_mapping_t& mapping) = 0;

    // Replacement keeps the committed file readable until Commit() succeeds.
    [[nodiscard]] virtual bundlefs_error_t BeginReplace(const char* name, uint32_t size,
                                                        bundlefs_writer_t& writer_out) = 0;
    [[nodiscard]] virtual bundlefs_error_t Write(bundlefs_writer_t& writer, const void* data, uint32_t size) = 0;
    [[nodiscard]] virtual bundlefs_error_t OpenStaged(const bundlefs_writer_t& writer, bundlefs_file_t& file_out) = 0;
    [[nodiscard]] virtual bundlefs_error_t Commit(bundlefs_writer_t& writer,
                                                  const uint8_t expected_sha256[BUNDLEFS_SHA256_SIZE]) = 0;
    virtual void Abort(bundlefs_writer_t& writer) = 0;
    [[nodiscard]] virtual bundlefs_error_t Remove(const char* name) = 0;

   protected:
    BundleStore() = default;
};

}  // namespace micropixel::runtime

#endif
