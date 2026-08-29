#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ui/system_ui.hpp"

namespace micropixel::work {
class BackgroundExecutor;
}

namespace micropixel::host_ui::lvgl::square_common {

struct HallCoverCacheConfig final {
    uint32_t target_size{};
    uint32_t corner_radius{};
    uint32_t top_background_rgb{};
    uint32_t bottom_background_rgb{};
};

struct HallCoverCacheUi final {
    void* context{};
    void (*show_placeholder)(void* context, uint32_t app_index){};
    void (*attach)(void* context, uint32_t app_index, const host_ui::HallCoverModel& cover){};
    void (*detach)(void* context, uint32_t app_index){};
    void (*request_refresh)(void* context){};
};

class HallCoverCache final {
   public:
    static constexpr size_t kCapacity = 6U;
    static constexpr uint32_t kJobQueueCapacity = 8U;

    explicit HallCoverCache(HallCoverCacheConfig config) : config_(config) {}
    HallCoverCache(const HallCoverCache&) = delete;
    HallCoverCache& operator=(const HallCoverCache&) = delete;
    ~HallCoverCache();

    void BindBackgroundExecutor(work::BackgroundExecutor& executor);
    void BindUi(HallCoverCacheUi ui);
    void BeginCatalog(const host_ui::HallModel& model, uint32_t app_count, uint64_t catalog_signature);
    void RequestWindow(uint32_t first, uint32_t last, bool force = false);
    [[nodiscard]] bool PrepareSource(const host_ui::HallCoverModel& source, host_ui::HallCoverModel& prepared);
    void Pause();
    // Pause() must complete before this is called. The LVGL lock must be held
    // because releasing entries may detach visible image descriptors.
    void SetBackgroundColorsLocked(uint32_t top_background_rgb, uint32_t bottom_background_rgb);
    void Release();

   private:
    struct Entry final {
        uint8_t* pixels{};
        uint64_t key{};
        uint32_t app_index{host_ui::kMaxHallApps};
    };

    struct Job final {
        host_ui::HallCoverModel source{};
        uint64_t catalog_signature{};
        uint32_t app_index{};
        uint32_t request_generation{};
    };

    static void DispatchEntry(void* context);
    void Dispatch();
    void Process(const Job& job);
    [[nodiscard]] bool EnsureQueue();
    [[nodiscard]] bool Schedule();
    [[nodiscard]] bool ValidSource(const host_ui::HallCoverModel& source) const;
    [[nodiscard]] bool Prepared(uint32_t app_index, host_ui::HallCoverModel& cover);
    void ShowPlaceholder(uint32_t app_index);
    void Attach(uint32_t app_index, const host_ui::HallCoverModel& cover);
    void ReleaseEntry(Entry& entry);

    HallCoverCacheConfig config_{};
    HallCoverCacheUi ui_{};
    work::BackgroundExecutor* executor_{};
    std::array<host_ui::HallCoverModel, host_ui::kMaxHallApps> sources_{};
    std::array<Entry, kCapacity> entries_{};
    QueueHandle_t queue_{};
    StaticQueue_t queue_storage_{};
    std::array<uint8_t, sizeof(Job) * kJobQueueCapacity> queue_bytes_{};
    std::atomic_bool dispatch_scheduled_{};
    std::atomic_bool worker_active_{};
    std::atomic_uint32_t request_generation_{1U};
    uint64_t catalog_signature_{};
    uint32_t app_count_{};
    uint32_t window_first_{host_ui::kMaxHallApps};
    uint32_t window_last_{host_ui::kMaxHallApps};
};

}  // namespace micropixel::host_ui::lvgl::square_common
