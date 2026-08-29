#include "host/ui/lvgl/square_common/hall_cover_cache.hpp"

#include <algorithm>
#include <cstddef>

#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "freertos/task.h"
#include "host/ui/lvgl/square_common/hall_cover_cache_policy.hpp"
#include "host/ui/lvgl/square_common/hall_cover_codec.hpp"
#include "work/background_executor.hpp"

namespace micropixel::host_ui::lvgl::square_common {

HallCoverCache::~HallCoverCache() { Release(); }

void HallCoverCache::BindBackgroundExecutor(work::BackgroundExecutor& executor) { executor_ = &executor; }

void HallCoverCache::BindUi(HallCoverCacheUi ui) { ui_ = ui; }

void HallCoverCache::BeginCatalog(const host_ui::HallModel& model, uint32_t app_count, uint64_t catalog_signature) {
    Pause();
    sources_.fill({});
    app_count_ = std::min<uint32_t>(app_count, host_ui::kMaxHallApps);
    for (uint32_t index = 0U; index < app_count_; ++index) {
        sources_[index] = model.apps[index].cover;
    }
    catalog_signature_ = catalog_signature;
    window_first_ = host_ui::kMaxHallApps;
    window_last_ = host_ui::kMaxHallApps;
    for (Entry& entry : entries_) {
        entry.app_index = host_ui::kMaxHallApps;
    }
}

bool HallCoverCache::ValidSource(const host_ui::HallCoverModel& source) const {
    if (source.data == nullptr || (!esp_ptr_in_drom(source.data) && !esp_ptr_external_ram(source.data)) ||
        source.width == 0U || source.height == 0U || source.size == 0U) {
        return false;
    }
    if (source.format == host_ui::HallCoverFormat::kJpeg || source.format == host_ui::HallCoverFormat::kPng) {
        return esp_ptr_in_drom(source.data) && source.stride == 0U;
    }
    return source.stride >= source.width * 3U && source.size >= source.stride * source.height;
}

bool HallCoverCache::Prepared(uint32_t app_index, host_ui::HallCoverModel& cover) {
    if (app_index >= app_count_) {
        return false;
    }
    const host_ui::HallCoverModel& source = sources_[app_index];
    if (!PrepareSource(source, cover)) {
        return false;
    }
    const auto cached = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.pixels != nullptr && entry.key == source.cache_key;
    });
    if (cached != entries_.end()) {
        cached->app_index = app_index;
    }
    return true;
}

bool HallCoverCache::PrepareSource(const host_ui::HallCoverModel& source, host_ui::HallCoverModel& cover) {
    const uint32_t stride = HallCoverStride(config_.target_size);
    const uint32_t bytes = HallCoverBytes(config_.target_size);
    if (source.format == host_ui::HallCoverFormat::kRgb888 && source.width == config_.target_size &&
        source.height == config_.target_size && source.stride == stride && source.size == bytes) {
        cover = source;
        return true;
    }
    const auto cached = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.pixels != nullptr && entry.key == source.cache_key;
    });
    if (cached == entries_.end()) {
        return false;
    }
    cover = {.data = cached->pixels,
             .size = bytes,
             .width = config_.target_size,
             .height = config_.target_size,
             .stride = stride,
             .cache_key = cached->key};
    return true;
}

void HallCoverCache::ShowPlaceholder(uint32_t app_index) {
    if (ui_.show_placeholder != nullptr) {
        ui_.show_placeholder(ui_.context, app_index);
    }
}

void HallCoverCache::Attach(uint32_t app_index, const host_ui::HallCoverModel& cover) {
    if (ui_.attach != nullptr) {
        ui_.attach(ui_.context, app_index, cover);
    }
}

void HallCoverCache::ReleaseEntry(Entry& entry) {
    if (entry.app_index < host_ui::kMaxHallApps && ui_.detach != nullptr) {
        ui_.detach(ui_.context, entry.app_index);
    }
    heap_caps_free(entry.pixels);
    entry = {};
}

bool HallCoverCache::EnsureQueue() {
    if (queue_ == nullptr) {
        queue_ = xQueueCreateStatic(kJobQueueCapacity, sizeof(Job), queue_bytes_.data(), &queue_storage_);
    }
    return queue_ != nullptr && executor_ != nullptr && executor_->valid();
}

bool HallCoverCache::Schedule() {
    if (executor_ == nullptr || !executor_->valid()) {
        return false;
    }
    bool expected = false;
    if (!dispatch_scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }
    if (!executor_->Submit(DispatchEntry, this)) {
        dispatch_scheduled_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void HallCoverCache::DispatchEntry(void* context) { static_cast<HallCoverCache*>(context)->Dispatch(); }

void HallCoverCache::Dispatch() {
    Job job{};
    if (xQueueReceive(queue_, &job, 0U) == pdTRUE) {
        worker_active_.store(true, std::memory_order_release);
        Process(job);
        worker_active_.store(false, std::memory_order_release);
    }
    dispatch_scheduled_.store(false, std::memory_order_release);
    if (uxQueueMessagesWaiting(queue_) != 0U) {
        (void)Schedule();
    }
}

void HallCoverCache::Process(const Job& job) {
    if (job.request_generation != request_generation_.load(std::memory_order_acquire)) {
        return;
    }
    constexpr uint32_t kAlignment = 64U;
    const uint32_t bytes = HallCoverBytes(config_.target_size);
    const uint32_t allocation_bytes = (bytes + kAlignment - 1U) / kAlignment * kAlignment;
    auto* pixels = static_cast<uint8_t*>(
        heap_caps_aligned_calloc(kAlignment, allocation_bytes, 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    const bool decoded =
        pixels != nullptr && DecodeHallCoverRgb888(job.source, config_.target_size, config_.corner_radius,
                                                   config_.top_background_rgb, config_.bottom_background_rgb, pixels);
    if (decoded) {
        lv_draw_buf_t draw_buf{};
        if (lv_draw_buf_init(&draw_buf, config_.target_size, config_.target_size, LV_COLOR_FORMAT_RGB888,
                             HallCoverStride(config_.target_size), pixels, bytes) == LV_RESULT_OK) {
            lv_draw_buf_flush_cache(&draw_buf, nullptr);
        }
    }
    if (decoded && esp_lv_adapter_lock(-1) == ESP_OK) {
        const bool current = job.request_generation == request_generation_.load(std::memory_order_acquire) &&
                             job.catalog_signature == catalog_signature_ && job.app_index >= window_first_ &&
                             job.app_index < window_last_ && job.app_index < app_count_ &&
                             sources_[job.app_index].cache_key == job.source.cache_key;
        if (current) {
            auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
                return entry.pixels != nullptr && entry.key == job.source.cache_key;
            });
            if (existing == entries_.end()) {
                std::array<HallCoverCacheSlot, kCapacity> slots{};
                for (size_t index = 0U; index < slots.size(); ++index) {
                    slots[index] = {.occupied = entries_[index].pixels != nullptr,
                                    .key = entries_[index].key,
                                    .app_index = entries_[index].app_index};
                }
                const size_t slot_index = HallCoverCachePolicy::ReplacementIndex(
                    slots, job.source.cache_key, job.app_index, window_first_, window_last_);
                if (slot_index != HallCoverCachePolicy::kNoSlot) {
                    Entry& slot = entries_[slot_index];
                    ReleaseEntry(slot);
                    slot = {.pixels = pixels, .key = job.source.cache_key, .app_index = job.app_index};
                    pixels = nullptr;
                    existing = entries_.begin() + static_cast<std::ptrdiff_t>(slot_index);
                }
            }
            if (existing != entries_.end()) {
                existing->app_index = job.app_index;
                Attach(job.app_index, {.data = existing->pixels,
                                       .size = bytes,
                                       .width = config_.target_size,
                                       .height = config_.target_size,
                                       .stride = HallCoverStride(config_.target_size),
                                       .cache_key = existing->key});
                if (ui_.request_refresh != nullptr) {
                    ui_.request_refresh(ui_.context);
                }
            }
        }
        esp_lv_adapter_unlock();
    }
    heap_caps_free(pixels);
}

void HallCoverCache::RequestWindow(uint32_t first, uint32_t last, bool force) {
    if (app_count_ == 0U || !EnsureQueue()) {
        return;
    }
    first = std::min(first, app_count_);
    last = std::clamp(last, first, app_count_);
    if (!force && first == window_first_ && last == window_last_) {
        return;
    }
    window_first_ = first;
    window_last_ = last;
    const uint32_t generation = request_generation_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    (void)xQueueReset(queue_);
    for (uint32_t index = 0U; index < app_count_; ++index) {
        if (index < first || index >= last) {
            ShowPlaceholder(index);
        }
    }
    bool queued = false;
    for (uint32_t index = first; index < last; ++index) {
        const host_ui::HallCoverModel& source = sources_[index];
        if (!ValidSource(source)) {
            ShowPlaceholder(index);
            continue;
        }
        host_ui::HallCoverModel prepared{};
        if (Prepared(index, prepared)) {
            Attach(index, prepared);
            continue;
        }
        ShowPlaceholder(index);
        const Job job{.source = source,
                      .catalog_signature = catalog_signature_,
                      .app_index = index,
                      .request_generation = generation};
        queued = xQueueSend(queue_, &job, 0U) == pdTRUE || queued;
    }
    if (queued) {
        (void)Schedule();
    }
}

void HallCoverCache::Pause() {
    request_generation_.fetch_add(1U, std::memory_order_acq_rel);
    if (queue_ != nullptr) {
        (void)xQueueReset(queue_);
    }
    while (worker_active_.load(std::memory_order_acquire)) {
        vTaskDelay(1U);
    }
}

void HallCoverCache::SetBackgroundColorsLocked(uint32_t top_background_rgb, uint32_t bottom_background_rgb) {
    for (Entry& entry : entries_) {
        ReleaseEntry(entry);
    }
    config_.top_background_rgb = top_background_rgb;
    config_.bottom_background_rgb = bottom_background_rgb;
    window_first_ = host_ui::kMaxHallApps;
    window_last_ = host_ui::kMaxHallApps;
}

void HallCoverCache::Release() {
    Pause();
    for (Entry& entry : entries_) {
        ReleaseEntry(entry);
    }
    sources_.fill({});
    ui_ = {};
    executor_ = nullptr;
    app_count_ = 0U;
    catalog_signature_ = 0U;
    window_first_ = host_ui::kMaxHallApps;
    window_last_ = host_ui::kMaxHallApps;
}

}  // namespace micropixel::host_ui::lvgl::square_common
