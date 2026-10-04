// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>

#include "platform/storage/sector_block_storage.hpp"

namespace {

using micropixel::device::BlockStorageError;
using micropixel::platform::storage::SectorBlockStorage;
using micropixel::platform::storage::SectorDevice;

constexpr uint32_t kSectorSize = 512U;
constexpr uint64_t kSectorCount = 256U;  // 128 KiB
constexpr size_t kWorkspaceSectors = 4U;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

// In-memory medium: counts commands so the tests can prove the adapter batches
// whole sectors, and records the highest sector it ever wrote.
class FakeSectorDevice final : public SectorDevice {
   public:
    [[nodiscard]] uint32_t sector_size() const override { return kSectorSize; }
    [[nodiscard]] uint64_t sector_count() const override { return kSectorCount; }

    [[nodiscard]] bool ReadSectors(uint64_t first_sector, size_t count, uint8_t* destination) override {
        if (first_sector + count > kSectorCount) {
            return false;
        }
        std::memcpy(destination, bytes_.data() + first_sector * kSectorSize, count * kSectorSize);
        reads_ += 1U;
        return true;
    }

    [[nodiscard]] bool WriteSectors(uint64_t first_sector, size_t count, const uint8_t* source) override {
        if (first_sector + count > kSectorCount) {
            return false;
        }
        std::memcpy(bytes_.data() + first_sector * kSectorSize, source, count * kSectorSize);
        writes_ += 1U;
        return true;
    }

    void Fill(uint8_t value) { bytes_.fill(value); }
    [[nodiscard]] uint8_t ByteAt(uint64_t index) const { return bytes_[index]; }
    [[nodiscard]] uint32_t reads() const { return reads_; }
    [[nodiscard]] uint32_t writes() const { return writes_; }

   private:
    std::array<uint8_t, kSectorCount * kSectorSize> bytes_{};
    uint32_t reads_{};
    uint32_t writes_{};
};

struct Fixture final {
    FakeSectorDevice device;
    std::array<uint8_t, kWorkspaceSectors * kSectorSize> workspace{};
    SectorBlockStorage storage{device, workspace};
};

void GeometryMatchesTheMedium() {
    Fixture fixture;
    const auto& geometry = fixture.storage.geometry();
    Check(geometry.size_bytes == static_cast<uint64_t>(kSectorSize) * kSectorCount, "size comes from the medium");
    Check(geometry.erase_size == kSectorSize, "the sector is the erase unit");
    Check(geometry.program_size == 1U, "a lone commit marker must be programmable");
    Check(!geometry.mappable, "a card is never CPU addressable");
    Check(geometry.map_alignment == 0U, "unmappable media carry no map alignment");
}

void ErasedRangesReadBackAsAllOnes() {
    Fixture fixture;
    fixture.device.Fill(0x5AU);
    const uint64_t first = 8U * kSectorSize;
    const uint64_t length = 16U * kSectorSize;
    Check(fixture.storage.Erase(first, length).has_value(), "an aligned erase succeeds");
    for (uint64_t offset = first; offset < first + length; offset += 97U) {
        Check(fixture.device.ByteAt(offset) == 0xFFU, "erased bytes read back as 0xFF");
    }
    Check(fixture.device.ByteAt(first - 1U) == 0x5AU, "the erase stays inside its range");
    Check(fixture.device.ByteAt(first + length) == 0x5AU, "the erase does not run past its range");
    // A range spanning more than one workspace pass must still be filled fully.
    const uint64_t wide = 2U * kWorkspaceSectors * kSectorSize;
    Check(fixture.storage.Erase(0U, wide).has_value(), "an erase larger than the workspace succeeds");
    Check(fixture.device.ByteAt(wide - 1U) == 0xFFU, "the last byte of a wide erase is 0xFF");
    Check(fixture.device.writes() >= 2U, "a wide erase is split into whole-sector writes");
}

void ProgramPersistsWholeSectors() {
    Fixture fixture;
    std::array<uint8_t, kSectorSize> payload{};
    for (size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<uint8_t>(index);
    }
    const auto program_status = fixture.storage.Program(3U * kSectorSize, payload);
    Check(program_status.has_value(), "a sector-aligned program succeeds");
    std::array<uint8_t, kSectorSize> read_back{};
    const auto read_status = fixture.storage.Read(3U * kSectorSize, read_back);
    Check(read_status.has_value(), "an aligned read succeeds");
    Check(read_back == payload, "programmed bytes read back unchanged");
}

void ProgramMergesPartialSectors() {
    Fixture fixture;
    std::array<uint8_t, kSectorSize> payload{};
    for (size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<uint8_t>(0x10U + (index % 0x20U));
    }
    const uint64_t sector = 5U;
    Check(fixture.storage.Program(sector * kSectorSize, payload).has_value(), "a whole sector is written directly");
    // The commit marker BundleFS lands after a record body: four bytes inside an
    // already programmed sector must leave its neighbours alone.
    const std::array<uint8_t, 4U> marker{0x11U, 0x22U, 0x33U, 0x44U};
    const uint64_t marker_offset = sector * kSectorSize + 100U;
    Check(fixture.storage.Program(marker_offset, marker).has_value(), "a lone four-byte program succeeds");
    std::array<uint8_t, kSectorSize> merged{};
    Check(fixture.storage.Read(sector * kSectorSize, merged).has_value(), "the merged sector reads back");
    for (size_t index = 0U; index < merged.size(); ++index) {
        const uint8_t expected = (index >= 100U && index < 104U) ? marker[index - 100U] : payload[index];
        Check(merged[index] == expected, "a partial program keeps the rest of the sector");
    }
    // A program across a sector boundary merges both sides.
    const std::array<uint8_t, 6U> straddle{1U, 2U, 3U, 4U, 5U, 6U};
    const uint64_t straddle_offset = 7U * kSectorSize - 3U;
    Check(fixture.storage.Program(straddle_offset, straddle).has_value(), "a straddling program succeeds");
    std::array<uint8_t, 16U> window{};
    Check(fixture.storage.Read(straddle_offset - 4U, window).has_value(), "the straddled bytes read back");
    for (size_t index = 0U; index < straddle.size(); ++index) {
        Check(window[4U + index] == straddle[index], "both sectors of a straddling program were merged");
    }
    const auto past_end = fixture.storage.Program(static_cast<uint64_t>(kSectorSize) * kSectorCount, marker);
    Check(!past_end.has_value() && past_end.error() == BlockStorageError::kInvalidArgument,
          "a program past the end is rejected");
}

void EraseRequiresWholeSectors() {
    Fixture fixture;
    Check(fixture.storage.Erase(0U, 0U).has_value(), "an empty erase is a no-op");
    const auto unaligned = fixture.storage.Erase(kSectorSize / 2U, kSectorSize);
    Check(!unaligned.has_value() && unaligned.error() == BlockStorageError::kInvalidArgument,
          "an erase off the sector boundary is rejected");
    const auto past_end = fixture.storage.Erase(kSectorSize * kSectorCount, kSectorSize);
    Check(!past_end.has_value() && past_end.error() == BlockStorageError::kInvalidArgument,
          "an erase past the end is rejected");
    Check(fixture.device.writes() == 0U, "a rejected erase never reaches the medium");
}

void UnalignedReadsCrossSectors() {
    Fixture fixture;
    std::array<uint8_t, 16U * kSectorSize> payload{};
    for (size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<uint8_t>(index * 7U);
    }
    Check(fixture.storage.Program(0U, payload).has_value(), "the fixture payload is written");
    // Starts mid-sector and spans more than one workspace pass.
    const uint64_t offset = 100U;
    std::array<uint8_t, 4000U> window{};
    Check(fixture.storage.Read(offset, window).has_value(), "an unaligned read succeeds");
    Check(std::memcmp(window.data(), payload.data() + offset, window.size()) == 0, "unaligned bytes match the medium");
    Check(fixture.device.reads() >= 2U, "a read wider than the workspace is split");
}

void BoundsAndCapabilitiesAreEnforced() {
    Fixture fixture;
    std::array<uint8_t, 16U> window{};
    const auto past_end = fixture.storage.Read(static_cast<uint64_t>(kSectorSize) * kSectorCount, window);
    Check(!past_end.has_value() && past_end.error() == BlockStorageError::kInvalidArgument,
          "a read past the end is rejected");
    const auto mapped = fixture.storage.Map(std::span<const uint64_t>{}, kSectorSize);
    Check(!mapped.has_value() && mapped.error() == BlockStorageError::kUnsupported, "Map is unsupported");
    Check(fixture.storage.Sync().has_value(), "a card has nothing to checkpoint");
}

void UnusableMediaReportThemselves() {
    class EmptyDevice final : public SectorDevice {
       public:
        [[nodiscard]] uint32_t sector_size() const override { return 0U; }
        [[nodiscard]] uint64_t sector_count() const override { return 0U; }
        [[nodiscard]] bool ReadSectors(uint64_t, size_t, uint8_t*) override { return false; }
        [[nodiscard]] bool WriteSectors(uint64_t, size_t, const uint8_t*) override { return false; }
    } empty;
    std::array<uint8_t, kSectorSize> workspace{};
    SectorBlockStorage storage(empty, workspace);
    std::array<uint8_t, kSectorSize> window{};
    const auto read_status = storage.Read(0U, window);
    Check(!read_status.has_value() && read_status.error() == BlockStorageError::kUnavailable,
          "a medium without sectors is unavailable");
    const auto erase_status = storage.Erase(0U, kSectorSize);
    Check(!erase_status.has_value() && erase_status.error() == BlockStorageError::kUnavailable,
          "an unusable medium cannot be erased");
    Check(storage.geometry().size_bytes == 0U, "an empty medium has no size");
}

}  // namespace

int main() {
    GeometryMatchesTheMedium();
    ErasedRangesReadBackAsAllOnes();
    ProgramPersistsWholeSectors();
    ProgramMergesPartialSectors();
    EraseRequiresWholeSectors();
    UnalignedReadsCrossSectors();
    BoundsAndCapabilitiesAreEnforced();
    UnusableMediaReportThemselves();
    return 0;
}
