#include "platform/graphics/dma2d_copy_engine.hpp"

#include <cstring>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "hal/dma2d_ll.h"
#include "soc/dma2d_channel.h"

namespace micropixel::platform::graphics {
namespace {

constexpr const char* kTag = "dma2d_copy";
constexpr uint32_t kBytesPerPixel = 3U;  // BGR888 only
constexpr uint32_t kDataBurstLength = 128U;

// A copy failure takes the whole App Surface frame down, so the first
// occurrence of each cause is worth one line on the console.
enum class FailureStage : uint8_t { kValidate, kRowSync, kDescriptorSync, kEnqueue, kTimeout, kCount };
constexpr const char* kFailureStageNames[] = {"validate", "row-sync", "descriptor-sync", "enqueue", "timeout"};

void ReportFailure(FailureStage stage_id, esp_err_t status, const Dma2dCopyBlock* block) {
    static bool reported[static_cast<std::size_t>(FailureStage::kCount)] = {};
    const std::size_t slot = static_cast<std::size_t>(stage_id);
    if (reported[slot]) {
        return;
    }
    reported[slot] = true;
    const char* stage = kFailureStageNames[slot];
    if (block != nullptr) {
        ESP_LOGE(kTag,
                 "copy failed at %s: status=%d src=%p %ux%u stride=%u size=%u rect=%d,%d %dx%d origin=%u,%u dst=%p "
                 "%ux%u stride=%u size=%u rect=%d,%d %dx%d origin=%u,%u",
                 stage, static_cast<int>(status), block->source.pixels, block->source.width, block->source.height,
                 block->source.stride, block->source.size, block->source_rect.x, block->source_rect.y,
                 block->source_rect.width, block->source_rect.height, block->source.origin_x, block->source.origin_y,
                 block->destination.pixels, block->destination.width, block->destination.height,
                 block->destination.stride, block->destination.size, block->destination_rect.x,
                 block->destination_rect.y, block->destination_rect.width, block->destination_rect.height,
                 block->destination.origin_x, block->destination.origin_y);
    } else {
        ESP_LOGE(kTag, "copy failed at %s: status=%d", stage, static_cast<int>(status));
    }
}

// Writes back (and optionally invalidates) only the rows a block touches. The
// range spans whole rows between the first and last touched byte, which is what
// the PPA driver does for its "extended window" and keeps the call count at one
// per block instead of one per row.
bool SyncBlockRows(const uint8_t* base, uint32_t stride, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   int flags) {
    if (esp_cache_get_line_size_by_addr(const_cast<uint8_t*>(base)) == 0U) {
        return true;
    }
    const std::size_t begin = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * kBytesPerPixel;
    const std::size_t end =
        static_cast<std::size_t>(y + height - 1U) * stride + static_cast<std::size_t>(x + width) * kBytesPerPixel;
    return esp_cache_msync(const_cast<uint8_t*>(base + begin), end - begin, flags | ESP_CACHE_MSYNC_FLAG_UNALIGNED) ==
           ESP_OK;
}

uint32_t PhysicalWidth(uint32_t stride) { return stride / kBytesPerPixel; }
uint32_t PhysicalHeight(uint32_t size, uint32_t stride) { return stride == 0U ? 0U : size / stride; }

template <typename Surface>
bool ValidBgr888Surface(const Surface& surface) {
    if (surface.pixels == nullptr || surface.format != SurfacePixelFormat::kBgr888 || surface.width == 0U ||
        surface.height == 0U || surface.stride % kBytesPerPixel != 0U) {
        return false;
    }
    const uint64_t row_bytes = static_cast<uint64_t>(surface.origin_x + surface.width) * kBytesPerPixel;
    const uint64_t required =
        static_cast<uint64_t>(surface.stride) * (surface.origin_y + surface.height - 1U) + row_bytes;
    return surface.stride >= row_bytes && required <= surface.size &&
           PhysicalWidth(surface.stride) <= DMA2D_LL_DESC_2D_FIELD_MAX &&
           PhysicalHeight(surface.size, surface.stride) <= DMA2D_LL_DESC_2D_FIELD_MAX;
}

template <typename Surface>
bool RectInside(const Surface& surface, SurfaceRect rect) {
    return rect.x >= 0 && rect.y >= 0 && rect.width > 0 && rect.height > 0 &&
           static_cast<int64_t>(rect.x) + rect.width <= surface.width &&
           static_cast<int64_t>(rect.y) + rect.height <= surface.height;
}

void SetupDescriptor(dma2d_descriptor_t& descriptor, const uint8_t* buffer, uint32_t picture_width,
                     uint32_t picture_height, uint32_t x, uint32_t y, uint32_t width, uint32_t height, bool last,
                     dma2d_descriptor_t* next) {
    std::memset(&descriptor, 0, sizeof(descriptor));
    descriptor.owner = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
    descriptor.suc_eof = last ? 1U : 0U;
    descriptor.dma2d_en = 1U;
    descriptor.ha_length = picture_width;
    descriptor.va_size = picture_height;
    descriptor.hb_length = width;
    descriptor.vb_size = height;
    descriptor.x = x;
    descriptor.y = y;
    descriptor.pbyte = DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
    descriptor.mode = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
    descriptor.buffer = const_cast<uint8_t*>(buffer);
    descriptor.next = last ? nullptr : next;
}

}  // namespace

Dma2dCopyEngine::~Dma2dCopyEngine() { Release(); }

esp_err_t Dma2dCopyEngine::Initialize() {
    if (pool_ != nullptr) {
        return ESP_OK;
    }
    if (done_ == nullptr) {
        done_ = xSemaphoreCreateBinaryStatic(&done_storage_);
    }
    std::size_t alignment = 0U;
    esp_cache_get_alignment(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, &alignment);
    descriptor_stride_bytes_ = alignment > sizeof(dma2d_descriptor_t) ? alignment : sizeof(dma2d_descriptor_t);
    if (descriptor_stride_bytes_ % DMA2D_LL_DESC_ALIGNMENT != 0U) {
        descriptor_stride_bytes_ += DMA2D_LL_DESC_ALIGNMENT - descriptor_stride_bytes_ % DMA2D_LL_DESC_ALIGNMENT;
    }
    const std::size_t descriptor_alignment =
        alignment > DMA2D_LL_DESC_ALIGNMENT ? alignment : static_cast<std::size_t>(DMA2D_LL_DESC_ALIGNMENT);
    tx_descriptors_ = static_cast<dma2d_descriptor_t*>(
        heap_caps_aligned_calloc(descriptor_alignment, kMaxBlocks, descriptor_stride_bytes_,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    rx_descriptors_ = static_cast<dma2d_descriptor_t*>(
        heap_caps_aligned_calloc(descriptor_alignment, kMaxBlocks, descriptor_stride_bytes_,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    transaction_placeholder_ = static_cast<dma2d_trans_t*>(
        heap_caps_calloc(1U, dma2d_get_trans_elm_size(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (tx_descriptors_ == nullptr || rx_descriptors_ == nullptr || transaction_placeholder_ == nullptr) {
        Release();
        return ESP_ERR_NO_MEM;
    }
    dma2d_pool_config_t pool_config{};
    pool_config.pool_id = 0U;
    const esp_err_t status = dma2d_acquire_pool(&pool_config, &pool_);
    if (status != ESP_OK) {
        pool_ = nullptr;
        Release();
        return status;
    }
    transaction_config_ = {
        .tx_channel_num = 1U,
        .rx_channel_num = 1U,
        .channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING,
        .specified_tx_channel_mask = 0U,
        .specified_rx_channel_mask = 0U,
        .on_job_picked = OnJobPicked,
        .user_config = this,
    };
    return ESP_OK;
}

void Dma2dCopyEngine::Release() {
    if (pool_ != nullptr) {
        (void)dma2d_release_pool(pool_);
        pool_ = nullptr;
    }
    heap_caps_free(tx_descriptors_);
    heap_caps_free(rx_descriptors_);
    heap_caps_free(transaction_placeholder_);
    tx_descriptors_ = nullptr;
    rx_descriptors_ = nullptr;
    transaction_placeholder_ = nullptr;
}

bool Dma2dCopyEngine::CopyBlocks(const Dma2dCopyBlock* blocks, std::size_t count) {
    if (pool_ == nullptr || blocks == nullptr || count == 0U || count > kMaxBlocks) {
        return false;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        const Dma2dCopyBlock& block = blocks[index];
        if (!ValidBgr888Surface(block.source) || !ValidBgr888Surface(block.destination) ||
            !RectInside(block.source, block.source_rect) || !RectInside(block.destination, block.destination_rect) ||
            block.source_rect.width != block.destination_rect.width ||
            block.source_rect.height != block.destination_rect.height) {
            ReportFailure(FailureStage::kValidate, ESP_ERR_INVALID_ARG, &block);
            return false;
        }
    }
    auto descriptor_at = [this](dma2d_descriptor_t* base, std::size_t index) {
        return reinterpret_cast<dma2d_descriptor_t*>(reinterpret_cast<uint8_t*>(base) +
                                                     index * descriptor_stride_bytes_);
    };
    const int64_t sync_started_us = esp_timer_get_time();
    uint64_t pixels = 0U;
    for (std::size_t index = 0U; index < count; ++index) {
        const Dma2dCopyBlock& block = blocks[index];
        const bool last = index + 1U == count;
        const uint32_t source_x = block.source.origin_x + static_cast<uint32_t>(block.source_rect.x);
        const uint32_t source_y = block.source.origin_y + static_cast<uint32_t>(block.source_rect.y);
        const uint32_t destination_x = block.destination.origin_x + static_cast<uint32_t>(block.destination_rect.x);
        const uint32_t destination_y = block.destination.origin_y + static_cast<uint32_t>(block.destination_rect.y);
        const uint32_t width = static_cast<uint32_t>(block.destination_rect.width);
        const uint32_t height = static_cast<uint32_t>(block.destination_rect.height);
        pixels += static_cast<uint64_t>(width) * height;
        const std::size_t width_bin = width < 16U ? 0U : width < 64U ? 1U : width < 256U ? 2U : 3U;
        ++blocks_by_width_[width_bin];
        pixels_by_width_[width_bin] += static_cast<uint64_t>(width) * height;
        SetupDescriptor(*descriptor_at(tx_descriptors_, index), block.source.pixels, PhysicalWidth(block.source.stride),
                        PhysicalHeight(block.source.size, block.source.stride), source_x, source_y, width, height, last,
                        descriptor_at(tx_descriptors_, index + 1U));
        SetupDescriptor(*descriptor_at(rx_descriptors_, index), block.destination.pixels,
                        PhysicalWidth(block.destination.stride),
                        PhysicalHeight(block.destination.size, block.destination.stride), destination_x, destination_y,
                        width, height, last, descriptor_at(rx_descriptors_, index + 1U));
        if (!SyncBlockRows(block.source.pixels, block.source.stride, source_x, source_y, width, height,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M) ||
            !SyncBlockRows(block.destination.pixels, block.destination.stride, destination_x, destination_y, width,
                           height, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE)) {
            ReportFailure(FailureStage::kRowSync, ESP_FAIL, &block);
            return false;
        }
    }
    // Descriptors live in internal SRAM, which sits behind the L2 cache on
    // ESP32-P4 but is not cached at all on ESP32-S31; msync rejects addresses
    // that have no cache line, so only sync when there is one.
    const std::size_t descriptor_bytes = count * descriptor_stride_bytes_;
    const bool descriptors_cached = esp_cache_get_line_size_by_addr(tx_descriptors_) != 0U;
    if (descriptors_cached &&
        (esp_cache_msync(tx_descriptors_, descriptor_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK ||
         esp_cache_msync(rx_descriptors_, descriptor_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK)) {
        ReportFailure(FailureStage::kDescriptorSync, ESP_FAIL, nullptr);
        return false;
    }
    (void)xSemaphoreTake(done_, 0);
    const int64_t transfer_started_us = esp_timer_get_time();
    cache_sync_us_ += static_cast<uint64_t>(transfer_started_us - sync_started_us);
    picked_us_ = 0;
    eof_us_ = 0;
    const esp_err_t enqueue_status = dma2d_enqueue(pool_, &transaction_config_, transaction_placeholder_);
    if (enqueue_status != ESP_OK) {
        ReportFailure(FailureStage::kEnqueue, enqueue_status, nullptr);
        return false;
    }
    // A lost completion would otherwise wedge the compositor forever; the copy
    // of a full 720x720 surface takes well under 100 ms.
    const bool completed = xSemaphoreTake(done_, pdMS_TO_TICKS(1000)) == pdTRUE;
    const int64_t resumed_us = esp_timer_get_time();
    const uint64_t waited_us = static_cast<uint64_t>(resumed_us - transfer_started_us);
    transfer_wait_us_ += waited_us;
    if (completed) {
        const int64_t picked = picked_us_;
        const int64_t eof = eof_us_;
        if (picked >= transfer_started_us && eof >= picked && resumed_us >= eof) {
            channel_wait_us_ += static_cast<uint64_t>(picked - transfer_started_us);
            hardware_us_ += static_cast<uint64_t>(eof - picked);
            wakeup_us_ += static_cast<uint64_t>(resumed_us - eof);
        }
    }
    if (waited_us > max_transfer_wait_us_) {
        max_transfer_wait_us_ = waited_us;
        max_transfer_wait_pixels_ = pixels;
    }
    if (!completed) {
        ReportFailure(FailureStage::kTimeout, ESP_ERR_TIMEOUT, nullptr);
        return false;
    }
    ++transactions_;
    blocks_copied_ += static_cast<uint32_t>(count);
    pixels_copied_ += pixels;
    return true;
}

bool Dma2dCopyEngine::OnJobPicked(uint32_t channel_count, const dma2d_trans_channel_info_t* channels,
                                  void* user_config) {
    auto* engine = static_cast<Dma2dCopyEngine*>(user_config);
    dma2d_channel_handle_t tx_channel = nullptr;
    dma2d_channel_handle_t rx_channel = nullptr;
    for (uint32_t index = 0U; index < channel_count; ++index) {
        if (channels[index].dir == DMA2D_CHANNEL_DIRECTION_TX) {
            tx_channel = channels[index].chan;
        } else {
            rx_channel = channels[index].chan;
        }
    }
    dma2d_trigger_t trigger{};
    trigger.periph = DMA2D_TRIG_PERIPH_M2M;
    trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX;
    (void)dma2d_connect(tx_channel, &trigger);
    trigger.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
    (void)dma2d_connect(rx_channel, &trigger);

    dma2d_transfer_ability_t ability{};
    ability.desc_burst_en = true;
    ability.data_burst_length = kDataBurstLength;
    ability.access_ext_mem = true;
    ability.mb_size = DMA2D_MACRO_BLOCK_SIZE_NONE;
    (void)dma2d_set_transfer_ability(tx_channel, &ability);
    (void)dma2d_set_transfer_ability(rx_channel, &ability);

    dma2d_csc_config_t tx_csc{};
    tx_csc.tx_csc_option = DMA2D_CSC_TX_NONE;
    tx_csc.pre_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
    tx_csc.post_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
    dma2d_csc_config_t rx_csc{};
    rx_csc.rx_csc_option = DMA2D_CSC_RX_NONE;
    rx_csc.pre_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
    rx_csc.post_scramble = DMA2D_SCRAMBLE_ORDER_BYTE2_1_0;
    (void)dma2d_configure_color_space_conversion(tx_channel, &tx_csc);
    (void)dma2d_configure_color_space_conversion(rx_channel, &rx_csc);

    dma2d_rx_event_callbacks_t callbacks{};
    callbacks.on_recv_eof = OnReceiveEof;
    (void)dma2d_register_rx_event_callbacks(rx_channel, &callbacks, engine);

    (void)dma2d_set_desc_addr(rx_channel, reinterpret_cast<intptr_t>(engine->rx_descriptors_));
    (void)dma2d_set_desc_addr(tx_channel, reinterpret_cast<intptr_t>(engine->tx_descriptors_));
    engine->picked_us_ = esp_timer_get_time();
    (void)dma2d_start(rx_channel);
    (void)dma2d_start(tx_channel);
    return false;
}

bool Dma2dCopyEngine::OnReceiveEof(dma2d_channel_handle_t, dma2d_event_data_t*, void* user_data) {
    auto* engine = static_cast<Dma2dCopyEngine*>(user_data);
    engine->eof_us_ = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    (void)xSemaphoreGiveFromISR(engine->done_, &woken);
    return woken == pdTRUE;
}

}  // namespace micropixel::platform::graphics
