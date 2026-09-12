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
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    ++catalog_generation_;
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
    Resume();
    esp_lv_adapter_unlock();
}

bool HallCoverCache::ValidSource(const host_ui::HallCoverModel& source) const {
    if (source.data == nullptr || (!esp_ptr_in_drom(source.data) && !esp_ptr_external_ram(source.data)) ||
        source.width == 0U || source.height == 0U || source.size == 0U) {
        return false;
    }
    // Compressed covers may be flash-mapped (NOR) or staged in PSRAM by a non-mappable Bundle source
    // (e.g. NAND); the owner keeps the source alive until PauseHallCoverLoading() drains the worker.
    if (source.format == host_ui::HallCoverFormat::kJpeg || source.format == host_ui::HallCoverFormat::kPng) {
        return source.stride == 0U;
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
        return entry.pixels != nullptr && entry.identity.Matches({source.cache_key, source.data, catalog_generation_});
    });
    if (cached != entries_.end()) {
        cached->app_index = app_index;
    }
    return true;
}

bool HallCoverCache::PrepareSource(const host_ui::HallCoverModel& source, host_ui::HallCoverModel& cover) {
    const uint32_t stride = HallCoverStride(config_.target_size);
    const uint32_t bytes = HallCoverBytes(config_.target_size);
    if (HallCoverCachePolicy::CanUseSourceDirectly(source, config_.target_size)) {
        cover = source;
        return true;
    }
    const auto cached = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
        return entry.pixels != nullptr && entry.identity.Matches({source.cache_key, source.data, catalog_generation_});
    });
    if (cached == entries_.end()) {
        return false;
    }
    cached->last_used = ++use_sequence_;
    cover = {.data = cached->pixels,
             .size = bytes,
             .width = config_.target_size,
             .height = config_.target_size,
             .stride = stride,
             .cache_key = cached->identity.key};
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

bool HallCoverCache::EvictOutsideWindow() {
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return false;
    }
    std::array<HallCoverCacheSlot, kCapacity> slots{};
    for (size_t index = 0U; index < slots.size(); ++index) {
        slots[index] = {.occupied = entries_[index].pixels != nullptr,
                        .key = entries_[index].identity.key,
                        .app_index = entries_[index].app_index,
                        .last_used = entries_[index].last_used};
    }
    const size_t index = HallCoverCachePolicy::OldestOutsideWindow(slots, window_first_, window_last_);
    if (index != HallCoverCachePolicy::kNoSlot) {
        ReleaseEntry(entries_[index]);
    }
    esp_lv_adapter_unlock();
    return index != HallCoverCachePolicy::kNoSlot;
}

void HallCoverCache::TrimForLaunchLocked(const uint8_t* retained_pixels) {
    for (Entry& entry : entries_) {
        if (!HallCoverCachePolicy::RetainForLaunch(entry.app_index, window_first_, window_last_,
                                                   entry.pixels != nullptr && entry.pixels == retained_pixels)) {
            ReleaseEntry(entry);
        }
    }
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
    // Mark active before removing a job so Pause cannot miss a dequeued read.
    worker_active_.store(true);
    Job job{};
    if (xQueueReceive(queue_, &job, 0U) == pdTRUE) {
        Process(job);
    }
    worker_active_.store(false);
    dispatch_scheduled_.store(false, std::memory_order_release);
    if (uxQueueMessagesWaiting(queue_) != 0U) {
        (void)Schedule();
    }
}

void HallCoverCache::Process(const Job& job) {
    if (paused_.load() || job.request_generation != request_generation_.load(std::memory_order_acquire)) {
        return;
    }
    constexpr uint32_t kAlignment = 64U;
    const uint32_t bytes = HallCoverBytes(config_.target_size);
    const uint32_t allocation_bytes = (bytes + kAlignment - 1U) / kAlignment * kAlignment;
    constexpr uint32_t kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    while (!HallCoverCachePolicy::CanGrow(heap_caps_get_free_size(kCaps), allocation_bytes)) {
        if (!EvictOutsideWindow()) {
            // Keep the visible working set usable even below the retention reserve.
            break;
        }
    }
    auto* pixels = static_cast<uint8_t*>(heap_caps_aligned_calloc(kAlignment, allocation_bytes, 1U, kCaps));
    while (pixels == nullptr && EvictOutsideWindow()) {
        pixels = static_cast<uint8_t*>(heap_caps_aligned_calloc(kAlignment, allocation_bytes, 1U, kCaps));
    }
    struct DecodeContext final {
        HallCoverCache* cache;
        const Job* job;
        uint8_t* pixels;
    } context{this, &job, pixels};
    const auto consume = [](void* opaque, const host_ui::HallCoverModel& source) {
        const auto& work = *static_cast<const DecodeContext*>(opaque);
        const auto& cache = *work.cache;
        if (cache.paused_.load() ||
            work.job->request_generation != cache.request_generation_.load(std::memory_order_acquire) ||
            !cache.ValidSource(source)) {
            return false;
        }
        return DecodeHallCoverRgb888(source, cache.config_.target_size, cache.config_.corner_radius,
                                     cache.config_.top_background_rgb, work.pixels);
    };
    // The source reader lends bytes only to consume. It releases compressed
    // bytes/mappings before this task publishes the independent decoded image.
    bool decoded = false;
    if (pixels != nullptr && !paused_.load() &&
        job.request_generation == request_generation_.load(std::memory_order_acquire)) {
        decoded = job.source.read_source != nullptr
                      ? job.source.read_source(job.source.reader_context, consume, &context)
                      : consume(&context, job.source);
    }
    if (decoded) {
        lv_draw_buf_t draw_buf{};
        decoded = lv_draw_buf_init(&draw_buf, config_.target_size, config_.target_size, LV_COLOR_FORMAT_RGB888,
                                   HallCoverStride(config_.target_size), pixels, bytes) == LV_RESULT_OK;
        if (decoded) {
            lv_draw_buf_flush_cache(&draw_buf, nullptr);
        }
    }
    if (decoded && esp_lv_adapter_lock(-1) == ESP_OK) {
        const bool current = !paused_.load() &&
                             job.request_generation == request_generation_.load(std::memory_order_acquire) &&
                             job.catalog_signature == catalog_signature_ && job.app_index >= window_first_ &&
                             job.app_index < window_last_ && job.app_index < app_count_ &&
                             sources_[job.app_index].cache_key == job.source.cache_key;
        if (current) {
            auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
                return entry.pixels != nullptr &&
                       entry.identity.Matches({job.source.cache_key, job.source.data, catalog_generation_});
            });
            if (existing == entries_.end()) {
                std::array<HallCoverCacheSlot, kCapacity> slots{};
                for (size_t index = 0U; index < slots.size(); ++index) {
                    slots[index] = {.occupied = entries_[index].pixels != nullptr,
                                    .key = entries_[index].identity.key,
                                    .app_index = entries_[index].app_index,
                                    .last_used = entries_[index].last_used};
                }
                const size_t slot_index = HallCoverCachePolicy::ReplacementIndex(
                    slots, job.source.cache_key, job.app_index, window_first_, window_last_);
                if (slot_index != HallCoverCachePolicy::kNoSlot) {
                    Entry& slot = entries_[slot_index];
                    ReleaseEntry(slot);
                    slot = {.pixels = pixels,
                            .identity = {job.source.cache_key, job.source.data, catalog_generation_},
                            .app_index = job.app_index};
                    pixels = nullptr;
                    existing = entries_.begin() + static_cast<std::ptrdiff_t>(slot_index);
                }
            }
            if (existing != entries_.end()) {
                existing->app_index = job.app_index;
                existing->last_used = ++use_sequence_;
                Attach(job.app_index, {.data = existing->pixels,
                                       .size = bytes,
                                       .width = config_.target_size,
                                       .height = config_.target_size,
                                       .stride = HallCoverStride(config_.target_size),
                                       .cache_key = existing->identity.key});
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
    if (paused_.load() || app_count_ == 0U || !EnsureQueue()) {
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
        host_ui::HallCoverModel prepared{};
        if (Prepared(index, prepared)) {
            Attach(index, prepared);
            continue;
        }
        ShowPlaceholder(index);
        if (!ValidSource(source) && (source.read_source == nullptr || source.cache_key == 0U)) {
            continue;
        }
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
    paused_.store(true);
    // Serialize cancellation with RequestWindow, which runs under the UI lock.
    // Release the lock before draining: the worker may need it to discard/publish.
    const bool locked = esp_lv_adapter_lock(-1) == ESP_OK;
    request_generation_.fetch_add(1U, std::memory_order_acq_rel);
    if (queue_ != nullptr) {
        (void)xQueueReset(queue_);
    }
    if (locked) {
        esp_lv_adapter_unlock();
    }
    while (worker_active_.load()) {
        vTaskDelay(1U);
    }
}

void HallCoverCache::Resume() { paused_.store(false); }

void HallCoverCache::SetBackgroundColorLocked(uint32_t top_background_rgb) {
    for (Entry& entry : entries_) {
        ReleaseEntry(entry);
    }
    config_.top_background_rgb = top_background_rgb;
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
