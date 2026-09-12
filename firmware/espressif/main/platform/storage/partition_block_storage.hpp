#ifndef MICROPIXEL_PLATFORM_STORAGE_PARTITION_BLOCK_STORAGE_HPP
#define MICROPIXEL_PLATFORM_STORAGE_PARTITION_BLOCK_STORAGE_HPP

#include <cstdint>
#include <expected>
#include <span>

#include "device/contracts/block_storage.hpp"
#include "esp_partition.h"

namespace micropixel::platform::storage {

// XIP NOR data partition exposed as BlockStorage. Erase and program go through
// esp_partition; Map() stitches discontiguous 64 KiB data blocks into one
// virtual range with spi_flash_mmap_pages, exactly like the BundleFS mmap
// path did before storage became pluggable.
class PartitionBlockStorage final : public device::BlockStorage {
   public:
    // Looks up the labelled data partition; present() is false when missing.
    explicit PartitionBlockStorage(const char* label, esp_partition_subtype_t subtype);
    explicit PartitionBlockStorage(const esp_partition_t* partition);

    [[nodiscard]] bool present() const { return partition_ != nullptr; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] const device::BlockStorageGeometry& geometry() const override { return geometry_; }

    [[nodiscard]] std::expected<void, device::BlockStorageError> Read(uint64_t offset,
                                                                      std::span<uint8_t> destination) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Program(uint64_t offset,
                                                                         std::span<const uint8_t> source) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Erase(uint64_t offset, uint64_t size) override;
    [[nodiscard]] std::expected<device::BlockStorageMapping, device::BlockStorageError> Map(
        std::span<const uint64_t> block_offsets, uint32_t block_size) override;
    void Unmap(device::BlockStorageMapping& mapping) override;

   private:
    [[nodiscard]] bool InRange(uint64_t offset, uint64_t size) const;

    const esp_partition_t* partition_{};
    device::BlockStorageGeometry geometry_{};
};

}  // namespace micropixel::platform::storage

#endif
