#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_private/dma2d.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/dma2d_types.h"
#include "platform/graphics/pixel_compositor.hpp"

namespace micropixel::platform::graphics {

// One rectangular block copy between two equally formatted surfaces.
struct Dma2dCopyBlock final {
    ConstPixelSurface source{};
    SurfaceRect source_rect{};
    PixelSurface destination{};
    SurfaceRect destination_rect{};
};

// Direct DMA2D memory-to-memory copy path for opaque BGR888 surfaces.
//
// `esp_async_color_convert` synchronizes the *entire* source and destination
// pictures on every request, which costs roughly 0.2 ms and evicts a full
// 720x720 App Surface from the CPU cache per tile copy. This engine owns its
// descriptors, links up to `kMaxBlocks` block copies into one transaction, and
// only synchronizes the rows each block actually touches, the same way the PPA
// driver treats its extended windows.
//
// Not thread-safe: a single owner issues one blocking transaction at a time.
class Dma2dCopyEngine final {
   public:
    // Large enough for one frame of Mario-style tile strips across all damage
    // regions; descriptors cost 64 bytes per block and direction.
    static constexpr std::size_t kMaxBlocks = 32U;

    Dma2dCopyEngine() = default;
    Dma2dCopyEngine(const Dma2dCopyEngine&) = delete;
    Dma2dCopyEngine& operator=(const Dma2dCopyEngine&) = delete;
    ~Dma2dCopyEngine();

    [[nodiscard]] esp_err_t Initialize();
    void Release();
    [[nodiscard]] bool Ready() const { return pool_ != nullptr; }

    // Copies every block in one linked DMA2D transaction and blocks until the
    // receive channel reports end-of-frame. Returns false without touching any
    // pixel when a block is not an in-bounds, equally sized, opaque BGR888 copy.
    [[nodiscard]] bool CopyBlocks(const Dma2dCopyBlock* blocks, std::size_t count);
    [[nodiscard]] bool Copy(const Dma2dCopyBlock& block) { return CopyBlocks(&block, 1U); }

    [[nodiscard]] uint32_t Transactions() const { return transactions_; }
    [[nodiscard]] uint32_t BlocksCopied() const { return blocks_copied_; }
    // Cumulative telemetry: pixels moved, time spent in cache maintenance and
    // time spent waiting for the hardware.
    [[nodiscard]] uint64_t PixelsCopied() const { return pixels_copied_; }
    [[nodiscard]] uint64_t CacheSyncMicros() const { return cache_sync_us_; }
    [[nodiscard]] uint64_t TransferWaitMicros() const { return transfer_wait_us_; }
    // Block shape histogram by width: [<16, <64, <256, >=256] pixels. Narrow
    // blocks are the pathological case for row-oriented 2D transfers.
    [[nodiscard]] uint64_t MaxTransferWaitMicros() const { return max_transfer_wait_us_; }
    [[nodiscard]] uint64_t MaxTransferWaitPixels() const { return max_transfer_wait_pixels_; }
    static constexpr std::size_t kWidthBins = 4U;
    [[nodiscard]] uint32_t BlocksByWidth(std::size_t bin) const { return blocks_by_width_[bin]; }
    [[nodiscard]] uint64_t PixelsByWidth(std::size_t bin) const { return pixels_by_width_[bin]; }
    // Breakdown of TransferWaitMicros(): time until the pool handed out a
    // channel pair, time the hardware spent moving pixels, and the latency from
    // the EOF interrupt until the waiting task resumed.
    [[nodiscard]] uint64_t ChannelWaitMicros() const { return channel_wait_us_; }
    [[nodiscard]] uint64_t HardwareMicros() const { return hardware_us_; }
    [[nodiscard]] uint64_t WakeupMicros() const { return wakeup_us_; }

   private:
    static bool OnJobPicked(uint32_t channel_count, const dma2d_trans_channel_info_t* channels, void* user_config);
    static bool OnReceiveEof(dma2d_channel_handle_t channel, dma2d_event_data_t* event, void* user_data);

    dma2d_pool_handle_t pool_{};
    dma2d_descriptor_t* tx_descriptors_{};
    dma2d_descriptor_t* rx_descriptors_{};
    std::size_t descriptor_stride_bytes_{};
    dma2d_trans_t* transaction_placeholder_{};
    dma2d_trans_config_t transaction_config_{};
    StaticSemaphore_t done_storage_{};
    SemaphoreHandle_t done_{};
    uint32_t transactions_{};
    uint32_t blocks_copied_{};
    uint64_t pixels_copied_{};
    uint64_t cache_sync_us_{};
    uint64_t transfer_wait_us_{};
    uint64_t max_transfer_wait_us_{};
    uint64_t max_transfer_wait_pixels_{};
    uint32_t blocks_by_width_[kWidthBins]{};
    uint64_t pixels_by_width_[kWidthBins]{};
    uint64_t channel_wait_us_{};
    uint64_t hardware_us_{};
    uint64_t wakeup_us_{};
    // Written from the pool/ISR callbacks of the in-flight transaction and read
    // by the owning task after the completion semaphore.
    volatile int64_t picked_us_{};
    volatile int64_t eof_us_{};
};

}  // namespace micropixel::platform::graphics
