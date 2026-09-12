#ifndef MICROPIXEL_PLATFORM_STORAGE_SPI_NAND_BLOCK_STORAGE_HPP
#define MICROPIXEL_PLATFORM_STORAGE_SPI_NAND_BLOCK_STORAGE_HPP

#include <cstdint>
#include <expected>
#include <mutex>
#include <span>

#include "device/contracts/block_storage.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "spi_nand_flash.h"

namespace micropixel::platform::storage {

// External SPI NAND behind the esp spi_nand_flash FTL (dhara). The FTL
// presents wear-levelled logical sectors; this adapter maps them onto the
// erase/program BlockStorage model: Erase() trims sectors so they read back
// as 0xFF, Program() performs sector read-modify-write for unaligned ranges,
// and Sync() checkpoints the FTL journal. Bytes are never CPU-addressable, so
// Map() is unsupported and BundleFS copies what it needs into PSRAM.
class SpiNandBlockStorage final : public device::BlockStorage {
   public:
    struct Config final {
        spi_host_device_t host{};
        gpio_num_t clock{GPIO_NUM_NC};
        gpio_num_t data_out{GPIO_NUM_NC};  // controller -> NAND (SIO0)
        gpio_num_t data_in{GPIO_NUM_NC};   // NAND -> controller (SIO1)
        gpio_num_t chip_select{GPIO_NUM_NC};
        gpio_num_t write_protect{GPIO_NUM_NC};  // SIO2, optional
        gpio_num_t hold{GPIO_NUM_NC};           // SIO3, optional
        uint32_t clock_hz{};
        spi_nand_flash_io_mode_t io_mode{SPI_NAND_IO_MODE_SIO};
    };

    SpiNandBlockStorage() = default;
    ~SpiNandBlockStorage() override;

    // Brings up the bus, probes the chip and sizes the sector buffer. Failure
    // leaves present() false so the board can still boot without the store.
    [[nodiscard]] esp_err_t Initialize(const Config& config);
    void Shutdown();

    [[nodiscard]] bool present() const { return nand_ != nullptr; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] const device::BlockStorageGeometry& geometry() const override { return geometry_; }

    [[nodiscard]] std::expected<void, device::BlockStorageError> Read(uint64_t offset,
                                                                      std::span<uint8_t> destination) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Program(uint64_t offset,
                                                                         std::span<const uint8_t> source) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Erase(uint64_t offset, uint64_t size) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Sync() override;
    [[nodiscard]] std::expected<device::BlockStorageMapping, device::BlockStorageError> Map(
        std::span<const uint64_t> block_offsets, uint32_t block_size) override;
    void Unmap(device::BlockStorageMapping& mapping) override;

   private:
    [[nodiscard]] bool InRange(uint64_t offset, uint64_t size) const;
    [[nodiscard]] std::expected<void, device::BlockStorageError> ReadSector(uint32_t sector, uint8_t* destination);
    [[nodiscard]] std::expected<void, device::BlockStorageError> WriteSector(uint32_t sector, const uint8_t* source);

    // Serialises the shared sector buffer; the FTL keeps its own lock.
    std::mutex mutex_;
    spi_host_device_t host_{};
    bool bus_owned_{};
    spi_device_handle_t device_{};
    spi_nand_flash_device_t* nand_{};
    uint8_t* sector_buffer_{};
    uint32_t sector_size_{};
    uint32_t sector_count_{};
    device::BlockStorageGeometry geometry_{};
};

}  // namespace micropixel::platform::storage

#endif
