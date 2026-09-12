#include "platform/storage/spi_nand_block_storage.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <mutex>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace micropixel::platform::storage {
namespace {

constexpr char kTag[] = "nand_storage";
// Fraction of NAND blocks the FTL keeps as spare for garbage collection
// (library default). Lower values expose more capacity but slow writes.
constexpr uint8_t kGcFactor = 45U;

bool UsesQuadLines(spi_nand_flash_io_mode_t io_mode) {
    return io_mode == SPI_NAND_IO_MODE_QOUT || io_mode == SPI_NAND_IO_MODE_QIO;
}

// In single/dual modes the NAND's WP# and HOLD# lines are plain inputs that
// must stay high; the SPI peripheral only drives them in quad transactions.
esp_err_t HoldLineHigh(gpio_num_t line) {
    if (line == GPIO_NUM_NC) {
        return ESP_OK;
    }
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << static_cast<unsigned>(line);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "configure NAND control line failed");
    return gpio_set_level(line, 1);
}

// JEDEC READ ID (0x9F, 8 dummy bits, 3 bytes) issued directly on the device so a
// failed probe still tells whether the chip answers at all (0x00/0xFF = no response).
esp_err_t ReadJedecId(spi_device_handle_t device, uint8_t id_out[3]) {
    spi_transaction_ext_t transaction{};
    transaction.base.flags =
        SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_RXDATA;
    transaction.base.cmd = 0x9FU;
    transaction.base.rxlength = 3U * 8U;
    transaction.command_bits = 8U;
    transaction.address_bits = 0U;
    transaction.dummy_bits = 8U;
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(device, &transaction.base), kTag, "READ ID failed");
    std::memcpy(id_out, transaction.base.rx_data, 3U);
    return ESP_OK;
}

}  // namespace

SpiNandBlockStorage::~SpiNandBlockStorage() { Shutdown(); }

esp_err_t SpiNandBlockStorage::Initialize(const Config& config) {
    if (present()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config.clock == GPIO_NUM_NC || config.data_out == GPIO_NUM_NC || config.data_in == GPIO_NUM_NC ||
        config.chip_select == GPIO_NUM_NC || config.clock_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (UsesQuadLines(config.io_mode) && (config.write_protect == GPIO_NUM_NC || config.hold == GPIO_NUM_NC)) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool quad = UsesQuadLines(config.io_mode);
    if (!quad) {
        ESP_RETURN_ON_ERROR(HoldLineHigh(config.write_protect), kTag, "drive NAND WP# high failed");
        ESP_RETURN_ON_ERROR(HoldLineHigh(config.hold), kTag, "drive NAND HOLD# high failed");
    }

    spi_bus_config_t bus_config{};
    bus_config.sclk_io_num = config.clock;
    bus_config.mosi_io_num = config.data_out;
    bus_config.miso_io_num = config.data_in;
    bus_config.quadwp_io_num = quad ? config.write_protect : GPIO_NUM_NC;
    bus_config.quadhd_io_num = quad ? config.hold : GPIO_NUM_NC;
    bus_config.max_transfer_sz = 0;
    bus_config.flags = quad ? SPICOMMON_BUSFLAG_QUAD : SPICOMMON_BUSFLAG_MASTER;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(config.host, &bus_config, SPI_DMA_CH_AUTO), kTag,
                        "initialize SPI NAND bus failed");
    host_ = config.host;
    bus_owned_ = true;

    spi_device_interface_config_t device_config{};
    device_config.clock_speed_hz = static_cast<int>(config.clock_hz);
    device_config.mode = 0;
    device_config.spics_io_num = config.chip_select;
    device_config.queue_size = 4;
    device_config.flags = SPI_DEVICE_HALFDUPLEX;
    esp_err_t error = spi_bus_add_device(config.host, &device_config, &device_);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "add SPI NAND device failed: %s", esp_err_to_name(error));
        Shutdown();
        return error;
    }

    uint8_t jedec_id[3] = {};
    if (ReadJedecId(device_, jedec_id) == ESP_OK) {
        ESP_LOGI(kTag, "SPI NAND JEDEC ID: %02x %02x %02x", jedec_id[0], jedec_id[1], jedec_id[2]);
    }

    spi_nand_flash_config_t nand_config{};
    nand_config.device_handle = device_;
    nand_config.gc_factor = kGcFactor;
    nand_config.io_mode = config.io_mode;
    nand_config.flags = SPI_DEVICE_HALFDUPLEX;
    error = spi_nand_flash_init_device(&nand_config, &nand_);
    if (error != ESP_OK) {
        ESP_LOGE(kTag, "SPI NAND probe failed: %s", esp_err_to_name(error));
        nand_ = nullptr;
        Shutdown();
        return error;
    }

    error = spi_nand_flash_get_sector_size(nand_, &sector_size_);
    if (error == ESP_OK) {
        error = spi_nand_flash_get_capacity(nand_, &sector_count_);
    }
    if (error != ESP_OK || sector_size_ == 0U || sector_count_ == 0U || sector_count_ > UINT32_MAX / sector_size_) {
        ESP_LOGE(kTag, "SPI NAND geometry unavailable: %s", esp_err_to_name(error));
        Shutdown();
        return error == ESP_OK ? ESP_ERR_INVALID_SIZE : error;
    }
    sector_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(sector_size_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (sector_buffer_ == nullptr) {
        ESP_LOGE(kTag, "SPI NAND sector buffer allocation failed: %" PRIu32 " bytes", sector_size_);
        Shutdown();
        return ESP_ERR_NO_MEM;
    }
    geometry_ = device::BlockStorageGeometry{
        .size_bytes = static_cast<uint64_t>(sector_size_) * sector_count_,
        .erase_size = sector_size_,
        .program_size = 1U,
        .mappable = false,
        .map_alignment = 0U,
    };
    ESP_LOGI(kTag,
             "SPI NAND ready: %" PRIu32 " logical sectors x %" PRIu32 " B = %" PRIu32 " MiB, %s @ %" PRIu32 " MHz",
             sector_count_, sector_size_, static_cast<uint32_t>(geometry_.size_bytes / (1024U * 1024U)),
             UsesQuadLines(config.io_mode) ? "quad" : "single/dual", config.clock_hz / 1000000U);
    return ESP_OK;
}

void SpiNandBlockStorage::Shutdown() {
    if (nand_ != nullptr) {
        (void)spi_nand_flash_sync(nand_);
        (void)spi_nand_flash_deinit_device(nand_);
        nand_ = nullptr;
    }
    if (device_ != nullptr) {
        (void)spi_bus_remove_device(device_);
        device_ = nullptr;
    }
    if (bus_owned_) {
        (void)spi_bus_free(host_);
        bus_owned_ = false;
    }
    heap_caps_free(sector_buffer_);
    sector_buffer_ = nullptr;
    sector_size_ = 0U;
    sector_count_ = 0U;
    geometry_ = {};
}

bool SpiNandBlockStorage::InRange(uint64_t offset, uint64_t size) const {
    return present() && offset <= geometry_.size_bytes && size <= geometry_.size_bytes - offset;
}

std::expected<void, device::BlockStorageError> SpiNandBlockStorage::ReadSector(uint32_t sector, uint8_t* destination) {
    if (spi_nand_flash_read_sector(nand_, destination, sector) != ESP_OK) {
        ESP_LOGE(kTag, "sector read failed: sector=%" PRIu32, sector);
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<void, device::BlockStorageError> SpiNandBlockStorage::WriteSector(uint32_t sector,
                                                                                const uint8_t* source) {
    if (spi_nand_flash_write_sector(nand_, source, sector) != ESP_OK) {
        ESP_LOGE(kTag, "sector write failed: sector=%" PRIu32, sector);
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<void, device::BlockStorageError> SpiNandBlockStorage::Read(uint64_t offset,
                                                                         std::span<uint8_t> destination) {
    if (!InRange(offset, destination.size())) {
        return std::unexpected(present() ? device::BlockStorageError::kInvalidArgument
                                         : device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    uint32_t consumed = 0U;
    while (consumed < destination.size()) {
        const uint64_t current = offset + consumed;
        const uint32_t sector = static_cast<uint32_t>(current / sector_size_);
        const uint32_t within = static_cast<uint32_t>(current % sector_size_);
        const uint32_t chunk = std::min<uint32_t>(sector_size_ - within, destination.size() - consumed);
        if (within == 0U && chunk == sector_size_) {
            if (auto result = ReadSector(sector, destination.data() + consumed); !result) {
                return result;
            }
        } else {
            if (auto result = ReadSector(sector, sector_buffer_); !result) {
                return result;
            }
            std::memcpy(destination.data() + consumed, sector_buffer_ + within, chunk);
        }
        consumed += chunk;
    }
    return {};
}

std::expected<void, device::BlockStorageError> SpiNandBlockStorage::Program(uint64_t offset,
                                                                            std::span<const uint8_t> source) {
    if (!InRange(offset, source.size())) {
        return std::unexpected(present() ? device::BlockStorageError::kInvalidArgument
                                         : device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    uint32_t consumed = 0U;
    while (consumed < source.size()) {
        const uint64_t current = offset + consumed;
        const uint32_t sector = static_cast<uint32_t>(current / sector_size_);
        const uint32_t within = static_cast<uint32_t>(current % sector_size_);
        const uint32_t chunk = std::min<uint32_t>(sector_size_ - within, source.size() - consumed);
        if (within == 0U && chunk == sector_size_) {
            if (auto result = WriteSector(sector, source.data() + consumed); !result) {
                return result;
            }
        } else {
            // Partial sector: merge into the current contents so neighbouring
            // bytes (an earlier streamed chunk, the record body next to its
            // commit marker) survive.
            if (auto result = ReadSector(sector, sector_buffer_); !result) {
                return result;
            }
            std::memcpy(sector_buffer_ + within, source.data() + consumed, chunk);
            if (auto result = WriteSector(sector, sector_buffer_); !result) {
                return result;
            }
        }
        consumed += chunk;
    }
    return {};
}

std::expected<void, device::BlockStorageError> SpiNandBlockStorage::Erase(uint64_t offset, uint64_t size) {
    if (!InRange(offset, size) || (offset % sector_size_) != 0U || (size % sector_size_) != 0U) {
        return std::unexpected(present() ? device::BlockStorageError::kInvalidArgument
                                         : device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    const uint32_t first = static_cast<uint32_t>(offset / sector_size_);
    const uint32_t count = static_cast<uint32_t>(size / sector_size_);
    for (uint32_t index = 0U; index < count; ++index) {
        // Trimmed sectors read back as 0xFF from the FTL without occupying a page.
        if (spi_nand_flash_trim(nand_, first + index) != ESP_OK) {
            ESP_LOGE(kTag, "sector trim failed: sector=%" PRIu32, first + index);
            return std::unexpected(device::BlockStorageError::kIo);
        }
    }
    return {};
}

std::expected<void, device::BlockStorageError> SpiNandBlockStorage::Sync() {
    if (!present()) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    if (spi_nand_flash_sync(nand_) != ESP_OK) {
        ESP_LOGE(kTag, "FTL sync failed");
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<device::BlockStorageMapping, device::BlockStorageError> SpiNandBlockStorage::Map(
    std::span<const uint64_t> block_offsets, uint32_t block_size) {
    (void)block_offsets;
    (void)block_size;
    return std::unexpected(device::BlockStorageError::kUnsupported);
}

void SpiNandBlockStorage::Unmap(device::BlockStorageMapping& mapping) { mapping = {}; }

}  // namespace micropixel::platform::storage
