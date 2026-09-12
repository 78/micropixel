// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/hall_cover_cache.hpp"
#include "work/background_executor.hpp"

namespace ui = micropixel::host_ui;
namespace cache_ui = ui::lvgl::square_common;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
struct ReadGate final {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    bool released{};
};
struct Source final {
    ReadGate* gate{};
    unsigned reads{};
    unsigned active{};
    bool fail{};
    cache_ui::HallCoverCache* cancel_cache{};
    std::array<uint8_t, 3U> bytes{42U, 43U, 44U};
    static bool Read(const void* opaque, ui::HallCoverConsumer consume, void* context) {
        auto& source = *const_cast<Source*>(static_cast<const Source*>(opaque));
        ++source.reads;
        if (source.fail) return false;
        ++source.active;
        if (source.gate != nullptr) {
            std::unique_lock lock(source.gate->mutex);
            source.gate->entered = true;
            source.gate->condition.notify_all();
            source.gate->condition.wait(lock, [&] { return source.gate->released; });
        }
        if (source.cancel_cache != nullptr) {
            (void)esp_lv_adapter_lock(-1);
            source.cancel_cache->RequestWindow(10U, 16U);
            esp_lv_adapter_unlock();
        }
        const bool result =
            consume(context, {.data = source.bytes.data(), .size = 3U, .width = 1U, .height = 1U, .stride = 3U});
        --source.active;
        return result;
    }
};
struct View final {
    std::array<const uint8_t*, ui::kMaxHallApps> images{};
    unsigned attachments{};
    static void Placeholder(void* context, uint32_t index) { static_cast<View*>(context)->images[index] = nullptr; }
    static void Attach(void* context, uint32_t index, const ui::HallCoverModel& image) {
        auto& view = *static_cast<View*>(context);
        view.images[index] = image.data;
        ++view.attachments;
    }
};
void LazyWindowAndReturn() {
    micropixel::work::BackgroundExecutor executor;
    View view;
    cache_ui::HallCoverCache cache({.target_size = 2U});
    cache.BindBackgroundExecutor(executor);
    cache.BindUi(
        {.context = &view, .show_placeholder = View::Placeholder, .attach = View::Attach, .detach = View::Placeholder});
    std::array<Source, 20U> sources{};
    ui::HallModel model{};
    model.app_count = sources.size();
    for (size_t i = 0U; i < sources.size(); ++i) {
        model.apps[i].cover = {.cache_key = i + 1U, .reader_context = &sources[i], .read_source = Source::Read};
    }
    cache.BeginCatalog(model, model.app_count, 1U);
    cache.RequestWindow(0U, 6U);
    for (const auto& source : sources) Check(source.reads == 0U, "showing Hall must not read synchronously");
    executor.RunAll();
    for (size_t i = 0U; i < sources.size(); ++i) {
        Check(sources[i].reads == (i < 6U ? 1U : 0U), "only the requested six covers may be read");
        Check(sources[i].active == 0U, "decoded images must not retain original source bytes");
    }
    cache.RequestWindow(6U, 12U);
    executor.RunAll();
    Check(test_cover_allocations == 12U, "scrolling may retain decoded covers while memory is sufficient");
    const auto retained = view.images;
    cache.Pause();
    cache.TrimForLaunchLocked(nullptr);
    Check(test_cover_allocations == 6U, "launch must release all decoded covers outside the current six");
    cache.BeginCatalog(model, model.app_count, 1U);
    cache.RequestWindow(6U, 12U);
    executor.RunAll();
    for (size_t i = 6U; i < 12U; ++i) {
        Check(sources[i].reads == 1U && view.images[i] == retained[i],
              "return must reuse the same pixels without any source read or placeholder reload");
    }
    cache.RequestWindow(0U, 6U);
    Check(sources[0].reads == 1U && view.images[0] == nullptr,
          "scrolling to an evicted cover must show a placeholder before asynchronous loading");
    executor.RunAll();
    Check(sources[0].reads == 2U && view.images[0] != nullptr, "worker must restore the evicted cover");
    cache.RequestWindow(14U, 20U);
    cache.Pause();
    cache.RequestWindow(14U, 20U, true);
    executor.RunAll();
    Check(sources[14].reads == 0U, "pause must cancel pending reads and block new ones before catalog mutation");
    model.apps[0].cover.cache_key = 101U;
    cache.BeginCatalog(model, model.app_count, 2U);
    cache.RequestWindow(0U, 6U);
    executor.RunAll();
    Check(sources[0].reads == 3U && sources[1].reads == 2U,
          "changed content must reload only its own cached thumbnail");
    sources[14].fail = true;
    cache.RequestWindow(14U, 20U);
    executor.RunAll();
    Check(view.images[14] == nullptr && sources[14].active == 0U,
          "failed reads must leave a placeholder and release source ownership");
    cache.Pause();
    cache.TrimForLaunchLocked(nullptr);
    cache.BeginCatalog(model, model.app_count, 2U);
    sources[0].cancel_cache = &cache;
    cache.RequestWindow(0U, 6U);
    executor.RunAll();
    Check(view.images[0] == nullptr && sources[0].active == 0U,
          "a window change during source loading must discard the stale result and release bytes");
    cache.Pause();
    cache.Release();
    executor.RunAll();
    Check(test_cover_allocations == 0U, "cache shutdown must release every decoded allocation");
}
void PauseDrainsActiveRead() {
    micropixel::work::BackgroundExecutor executor;
    View view;
    cache_ui::HallCoverCache cache({.target_size = 2U});
    cache.BindBackgroundExecutor(executor);
    cache.BindUi(
        {.context = &view, .show_placeholder = View::Placeholder, .attach = View::Attach, .detach = View::Placeholder});
    ReadGate gate;
    Source source{.gate = &gate};
    ui::HallModel model{};
    model.app_count = 1U;
    model.apps[0].cover = {.cache_key = 1U, .reader_context = &source, .read_source = Source::Read};
    cache.BeginCatalog(model, 1U, 1U);
    cache.RequestWindow(0U, 1U);
    std::thread worker([&] { executor.RunAll(); });
    {
        std::unique_lock lock(gate.mutex);
        gate.condition.wait(lock, [&] { return gate.entered; });
    }
    std::atomic_bool pause_started{};
    std::atomic_bool pause_finished{};
    std::thread supervisor([&] {
        pause_started.store(true);
        cache.Pause();
        pause_finished.store(true);
    });
    while (!pause_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(!pause_finished.load(), "pause must wait until an active source reader releases its bytes");
    {
        std::lock_guard lock(gate.mutex);
        gate.released = true;
        gate.condition.notify_all();
    }
    worker.join();
    supervisor.join();
    Check(source.active == 0U && view.attachments == 0U && test_cover_allocations == 0U,
          "canceled active read must finish cleanup without publishing stale pixels");
    source.gate = nullptr;
    cache.BeginCatalog(model, 1U, 1U);
    test_cover_fail_allocation = true;
    cache.RequestWindow(0U, 1U);
    executor.RunAll();
    Check(source.reads == 1U && test_cover_allocations == 0U,
          "failed decode buffer allocation must not read or retain compressed cover bytes");
    test_cover_fail_allocation = false;
}
}  // namespace

namespace micropixel::host_ui::lvgl::square_common {
uint32_t HallCoverStride(uint32_t size) { return size * 3U; }
uint32_t HallCoverBytes(uint32_t size) { return size * size * 3U; }
bool DecodeHallCoverRgb888(const HallCoverModel& source, uint32_t size, uint32_t, uint32_t, uint8_t* pixels) {
    std::memset(pixels, source.data[0], HallCoverBytes(size));
    return true;
}
}  // namespace micropixel::host_ui::lvgl::square_common
int main() {
    LazyWindowAndReturn();
    PauseDrainsActiveRead();
    std::puts("Hall cover cache: lazy reads, launch retention, return reuse, cancellation and invalidation passed");
}
