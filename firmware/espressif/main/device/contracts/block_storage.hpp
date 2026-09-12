#ifndef MICROPIXEL_DEVICE_BLOCK_STORAGE_HPP
#define MICROPIXEL_DEVICE_BLOCK_STORAGE_HPP

#include <cstdint>
#include <expected>
#include <span>

namespace micropixel::device {

enum class BlockStorageError : uint8_t {
    kUnavailable,
    kInvalidArgument,
    kUnsupported,
    kIo,
};

// Byte-addressable erase/program storage. Every implementation exposes the
// same erase-before-program model: after Erase() the range reads back as 0xFF
// and Program() may only clear bits. Media without physical erase semantics
// (an FTL-managed NAND, an SD card) emulate that model so BundleFS can keep
// its erased-bank and commit-marker rules unchanged.
struct BlockStorageGeometry final {
    // Total addressable bytes. 64-bit so removable media (a 64 GB SD card)
    // are described exactly.
    uint64_t size_bytes{};
    // Erase() offset and size must be multiples of this value.
    uint32_t erase_size{};
    // Program() offset and size must be multiples of this value. 1 allows
    // arbitrary byte writes.
    uint32_t program_size{};
    // Map() is available: the bytes live in the CPU address space (XIP NOR).
    bool mappable{};
    // Map() block offsets and block size must be multiples of this value
    // (the MMU page size on XIP NOR). Zero when not mappable.
    uint32_t map_alignment{};
};

// One read-only window produced by BlockStorage::Map(). `handle` is
// implementation-defined and returned unchanged to Unmap().
struct BlockStorageMapping final {
    const void* data{};
    uint32_t handle{};
};

class BlockStorage {
   public:
    virtual ~BlockStorage() = default;
    BlockStorage(const BlockStorage&) = delete;
    BlockStorage& operator=(const BlockStorage&) = delete;

    [[nodiscard]] virtual const BlockStorageGeometry& geometry() const = 0;  // NOLINT(readability-identifier-naming)

    [[nodiscard]] virtual std::expected<void, BlockStorageError> Read(uint64_t offset,
                                                                      std::span<uint8_t> destination) = 0;
    [[nodiscard]] virtual std::expected<void, BlockStorageError> Program(uint64_t offset,
                                                                         std::span<const uint8_t> source) = 0;
    [[nodiscard]] virtual std::expected<void, BlockStorageError> Erase(uint64_t offset, uint64_t size) = 0;
    // Makes every completed Program()/Erase() durable. Journaled media (an
    // FTL) persist writes in issue order, so callers only need Sync() at the
    // points where a successful return must survive power loss. Direct NOR
    // writes are already durable and return immediately.
    [[nodiscard]] virtual std::expected<void, BlockStorageError> Sync() { return {}; }

    // Presents `block_size` bytes at each of `block_offsets` as one contiguous
    // read-only range in the order given. Only available when
    // geometry().mappable is true; other media return kUnsupported.
    [[nodiscard]] virtual std::expected<BlockStorageMapping, BlockStorageError> Map(
        std::span<const uint64_t> block_offsets, uint32_t block_size) = 0;
    virtual void Unmap(BlockStorageMapping& mapping) = 0;

   protected:
    BlockStorage() = default;
};

}  // namespace micropixel::device

#endif
