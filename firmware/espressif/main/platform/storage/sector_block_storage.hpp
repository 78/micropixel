// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "device/contracts/block_storage.hpp"

namespace micropixel::platform::storage {

// Raw sector access to one medium. Implementations are the board's link to the
// hardware (an SD card over SDMMC, an eMMC, a USB bridge); SectorBlockStorage
// owns every BlockStorage rule so the medium link stays a thin adapter and the
// contract itself stays testable without the peripheral.
class SectorDevice {
   public:
    SectorDevice() = default;
    SectorDevice(const SectorDevice&) = delete;
    SectorDevice& operator=(const SectorDevice&) = delete;
    virtual ~SectorDevice() = default;

    [[nodiscard]] virtual uint32_t sector_size() const = 0;
    [[nodiscard]] virtual uint64_t sector_count() const = 0;
    [[nodiscard]] virtual bool ReadSectors(uint64_t first_sector, size_t count, uint8_t* destination) = 0;
    [[nodiscard]] virtual bool WriteSectors(uint64_t first_sector, size_t count, const uint8_t* source) = 0;
};

// Presents a sector device with the erase-before-program model BundleFS needs.
// A card's own erase command leaves the contents undefined, so Erase() writes
// 0xFF over the range (in whole sectors): the erased-bank and commit-marker
// rules depend on those bytes reading back. A card can only be written a sector
// at a time, so the geometry advertises a one-byte program unit - the commit
// marker is a lone four-byte program - and Program() merges partial sectors
// with a read-modify-write through the workspace. Reads may be arbitrary and
// bounce the same way, and the bytes are never CPU-addressable, so Map()
// reports kUnsupported.
class SectorBlockStorage final : public device::BlockStorage {
   public:
    // `workspace` serves erase fills, partial-sector merges and unaligned
    // reads: at least one sector, larger values cut the number of commands.
    SectorBlockStorage(SectorDevice& device, std::span<uint8_t> workspace);
    ~SectorBlockStorage() override = default;

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
    // Whole sectors the workspace can hold at once.
    [[nodiscard]] size_t WorkspaceSectors() const;
    [[nodiscard]] std::expected<void, device::BlockStorageError> ReadSectors(uint64_t first_sector, size_t count,
                                                                             uint8_t* destination);
    [[nodiscard]] std::expected<void, device::BlockStorageError> WriteSectors(uint64_t first_sector, size_t count,
                                                                              const uint8_t* source);

    SectorDevice& device_;
    std::span<uint8_t> workspace_;
    uint32_t sector_size_{};
    device::BlockStorageGeometry geometry_{};
};

}  // namespace micropixel::platform::storage
