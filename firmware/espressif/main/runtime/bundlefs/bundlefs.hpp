#ifndef MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_HPP
#define MICROPIXEL_RUNTIME_BUNDLEFS_BUNDLEFS_HPP

#include <cstdint>
#include <mutex>

#include "device/contracts/block_storage.hpp"
#include "runtime/bundlefs/bundle_store.hpp"

namespace micropixel::runtime {

// Geometry of one BundleFS instance. Everything here is either read from the
// committed Catalog header or, for a fresh medium, derived from the
// BlockStorage geometry at Format() time; nothing depends on the medium size
// being known at compile time.
struct BundleFsGeometry final {
    uint64_t size_bytes{};
    uint32_t data_block_size{};
    uint32_t bank_size{};
    uint32_t data_offset{};
    uint32_t data_block_count{};
    uint32_t entry_capacity{};
    uint32_t record_size{};

    [[nodiscard]] bool valid() const { return data_block_count != 0U; }  // NOLINT(readability-identifier-naming)
};

// BundleFS v3 on one BlockStorage. Every instance owns its Catalog banks,
// geometry, a RAM copy of the committed Catalog and the single writer slot;
// the design contract is documented in docs/design/bundlefs.zh-CN.md.
// The Catalog copy is refreshed from the medium by Mount() and replaced by
// the verified read-back after every commit, so RAM never holds a Catalog
// that is not also committed on the medium.
class BundleFs final : public BundleStore {
   public:
    // `data_block_size` of zero adopts the committed Catalog's block size, or
    // DefaultDataBlockSize() when formatting. A non-zero value is enforced:
    // a Catalog written with another block size mounts as unsupported.
    explicit BundleFs(device::BlockStorage& storage, uint32_t data_block_size = 0U);
    ~BundleFs() override;

    // The medium's own allocation unit: its erase sector, or the MMU page
    // when it maps (what mkfs reads from the device). It is doubled only while
    // the Catalog block map (4 bytes per block, resident while mounted) would
    // exceed CONFIG_MICROPIXEL_BUNDLEFS_BLOCK_MAP_BUDGET_KIB. Zero when the
    // medium cannot host BundleFS.
    [[nodiscard]] static uint32_t DefaultDataBlockSize(const device::BlockStorageGeometry& geometry);
    // Geometry Format() would write for `storage` with the given block size
    // (zero selects the default). `valid()` is false when the medium is unsuitable.
    [[nodiscard]] static BundleFsGeometry PlanGeometry(const device::BlockStorageGeometry& storage,
                                                       uint32_t data_block_size);

    // NOLINTBEGIN(readability-identifier-naming)
    // Block size in effect: the mounted Catalog's, else the planned one.
    [[nodiscard]] uint32_t data_block_size() const;
    [[nodiscard]] device::BlockStorage& storage() const { return storage_; }
    // NOLINTEND(readability-identifier-naming)

    [[nodiscard]] bool mappable() const override { return storage_.geometry().mappable; }
    [[nodiscard]] bundlefs_error_t Mount() override;
    [[nodiscard]] bundlefs_error_t Format() override;
    [[nodiscard]] bundlefs_error_t GetStoreInfo(bundlefs_store_info_t& info_out) override;
    [[nodiscard]] bundlefs_error_t List(bundlefs_file_info_t* files_out, uint32_t capacity,
                                        uint32_t& count_out) override;
    [[nodiscard]] bundlefs_error_t GetFileSha256(const char* name, uint8_t sha256_out[BUNDLEFS_SHA256_SIZE]) override;
    [[nodiscard]] bundlefs_error_t Open(const char* name, bundlefs_file_t& file_out) override;
    [[nodiscard]] bundlefs_error_t GetFileInfo(const bundlefs_file_t& file, bundlefs_file_info_t& info_out) override;
    [[nodiscard]] bundlefs_error_t Read(const bundlefs_file_t& file, uint32_t offset, void* destination,
                                        uint32_t size) override;
    [[nodiscard]] bundlefs_error_t Map(const bundlefs_file_t& file, uint32_t offset, uint32_t size,
                                       bundlefs_mapping_t& mapping_out) override;
    void Unmap(bundlefs_mapping_t& mapping) override;
    [[nodiscard]] bundlefs_error_t BeginReplace(const char* name, uint32_t size,
                                                bundlefs_writer_t& writer_out) override;
    [[nodiscard]] bundlefs_error_t Write(bundlefs_writer_t& writer, const void* data, uint32_t size) override;
    [[nodiscard]] bundlefs_error_t OpenStaged(const bundlefs_writer_t& writer, bundlefs_file_t& file_out) override;
    [[nodiscard]] bundlefs_error_t Commit(bundlefs_writer_t& writer,
                                          const uint8_t expected_sha256[BUNDLEFS_SHA256_SIZE]) override;
    void Abort(bundlefs_writer_t& writer) override;
    [[nodiscard]] bundlefs_error_t Remove(const char* name) override;

   private:
    // Heap-backed byte range sized when the Catalog geometry becomes known.
    struct Buffer final {
        uint8_t* bytes{};
        uint32_t size{};
    };

    [[nodiscard]] bundlefs_error_t LoadLocked();
    [[nodiscard]] bundlefs_error_t EnsureMountedLocked();
    [[nodiscard]] bundlefs_error_t ResolveBlockLocked(const bundlefs_file_t& file, uint32_t logical_block,
                                                      uint32_t& physical_out, uint32_t& file_size_out);
    [[nodiscard]] bundlefs_error_t ReadFileLocked(const bundlefs_file_t& file, uint32_t offset, void* destination,
                                                  uint32_t size);
    [[nodiscard]] bundlefs_error_t CommitRecordLocked(Buffer& next);
    void ReleaseWriterLocked();

    device::BlockStorage& storage_;
    uint32_t requested_block_size_{};
    std::mutex mutex_;
    BundleFsGeometry geometry_{};
    Buffer catalog_{};  // committed Catalog copy; valid while mounted_
    bool mounted_{};
    bool writer_active_{};
    uint64_t writer_generation_{};
    Buffer writer_blocks_{};  // uint32_t physical block per logical block of the staged file
    uint32_t writer_block_count_{};
};

}  // namespace micropixel::runtime

#endif
