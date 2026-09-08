#include "platform/lvgl/display/direct_surface_presenter.hpp"

#include <cinttypes>
#include <cstring>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/lvgl/display/scanout_arbiter.hpp"
#include "platform/lvgl/display/scanout_stage_pool.hpp"
#include "work/task_policy.hpp"

#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#endif

namespace micropixel::platform::lvgl {
namespace {

constexpr const char* kTag = "direct_surface";
constexpr uint32_t kStageAlignment = 64U;
constexpr uint32_t kBytesPerRgb565 = 2U;

[[nodiscard]] bool ScaleRequested(const device::DirectSurfacePresentation& frame) {
    return (frame.flags & MICROPIXEL_SURFACE_PRESENT_SCALE_NEAREST) != 0U;
}

[[nodiscard]] uint16_t Swap16(uint16_t value) { return static_cast<uint16_t>((value << 8U) | (value >> 8U)); }

// CPU conversion of one RGB565 frame into a packed RGB565 or BGR888 target with
// optional nearest-neighbour enlargement. `swap_input` undoes a Guest buffer
// written in panel byte order; `swap_output` produces that order. Only the
// PPA/DMA2D profiles fall back to it; composited profiles convert in the engine.
[[maybe_unused]] void ConvertRgb565(const uint8_t* source, uint32_t source_pitch, uint32_t source_width,
                                    uint32_t source_height, bool swap_input, uint8_t* destination,
                                    uint32_t destination_stride, uint32_t destination_width,
                                    uint32_t destination_height, bool destination_bgr888, bool swap_output) {
    const bool same_size = source_width == destination_width && source_height == destination_height;
    if (same_size && !destination_bgr888 && swap_input == swap_output) {
        for (uint32_t y = 0U; y < destination_height; ++y) {
            std::memcpy(destination + y * destination_stride, source + y * source_pitch,
                        destination_width * kBytesPerRgb565);
        }
        return;
    }
    const bool swap = swap_input != swap_output;
    for (uint32_t y = 0U; y < destination_height; ++y) {
        const uint32_t source_y = same_size ? y : y * source_height / destination_height;
        const auto* source_row = reinterpret_cast<const uint16_t*>(source + source_y * source_pitch);
        uint8_t* destination_row = destination + y * destination_stride;
        if (destination_bgr888) {
            for (uint32_t x = 0U; x < destination_width; ++x) {
                const uint32_t source_x = same_size ? x : x * source_width / destination_width;
                uint16_t pixel = source_row[source_x];
                if (swap_input) {
                    pixel = Swap16(pixel);
                }
                const uint8_t red = static_cast<uint8_t>((pixel >> 11U) & 0x1FU);
                const uint8_t green = static_cast<uint8_t>((pixel >> 5U) & 0x3FU);
                const uint8_t blue = static_cast<uint8_t>(pixel & 0x1FU);
                destination_row[x * 3U + 0U] = static_cast<uint8_t>((blue << 3U) | (blue >> 2U));
                destination_row[x * 3U + 1U] = static_cast<uint8_t>((green << 2U) | (green >> 4U));
                destination_row[x * 3U + 2U] = static_cast<uint8_t>((red << 3U) | (red >> 2U));
            }
        } else {
            auto* destination_pixels = reinterpret_cast<uint16_t*>(destination_row);
            for (uint32_t x = 0U; x < destination_width; ++x) {
                const uint32_t source_x = same_size ? x : x * source_width / destination_width;
                const uint16_t pixel = source_row[source_x];
                destination_pixels[x] = swap ? Swap16(pixel) : pixel;
            }
        }
    }
}

#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
// Guest and stage pixels are written through the CPU cache; the panel DMA and
// the PPA read memory. ESP32-S31 internal SRAM reports a zero line size and
// must not be synced.

bool WriteBackForDma(const void* pixels, uint32_t length) {
    auto* base = const_cast<void*>(pixels);
    if (esp_cache_get_line_size_by_addr(base) == 0U) {
        return true;
    }
    return esp_cache_msync(base, length, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
}
#endif

}  // namespace

DirectSurfacePresenter::~DirectSurfacePresenter() { Shutdown(); }

esp_err_t DirectSurfacePresenter::Initialize(lv_display_t* display, uint32_t width, uint32_t height,
                                             const DirectScanoutProfile& profile, DirectFramebufferAccess* framebuffers,
                                             const DirectSurfaceCompositeSink& sink) {
    if (display == nullptr || width == 0U || height == 0U || sink.composite == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (task_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    display_ = display;
    width_ = width;
    height_ = height;
    profile_ = profile;
    framebuffers_ = framebuffers;
    composite_ = sink;
    if (profile_.mode == DirectScanoutProfile::Mode::kFramebufferRgb888 && framebuffers_ == nullptr) {
        profile_.mode = DirectScanoutProfile::Mode::kComposited;
    }
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    if (profile_.mode != DirectScanoutProfile::Mode::kComposited && blitter_.Initialize() != ESP_OK) {
        ESP_LOGW(kTag, "PPA unavailable; Direct Surface byte swap and scaling run on the CPU");
    }
#else
    profile_.mode = DirectScanoutProfile::Mode::kComposited;
#endif
    queue_ = xQueueCreateStatic(kQueueDepth, sizeof(Job), queue_buffer_, &queue_storage_);
    control_done_ = xSemaphoreCreateBinaryStatic(&control_done_storage_);
    control_mutex_ = xSemaphoreCreateMutexStatic(&control_mutex_storage_);
    stopped_ = xSemaphoreCreateBinaryStatic(&stopped_storage_);
    stop_requested_.store(false, std::memory_order_release);
    TaskHandle_t task = nullptr;
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        TaskEntry, "micropixel_scanout", kTaskStackBytes, this, task_policy::kDisplayPriority, &task,
        task_policy::kSystemCore, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t created = xTaskCreatePinnedToCore(TaskEntry, "micropixel_scanout", kTaskStackBytes, this,
                                                       task_policy::kDisplayPriority, &task, task_policy::kSystemCore);
#endif
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    task_ = task;
    ScanoutArbiter::Instance().RegisterPresenter({.context = this, .yield = YieldHook});
    ESP_LOGI(kTag, "presenter ready: mode=%u swapped=%s fps=%u", static_cast<unsigned>(profile_.mode),
             profile_.rgb565_byte_swapped ? "yes" : "no", static_cast<unsigned>(profile_.max_full_frame_fps));
    return ESP_OK;
}

void DirectSurfacePresenter::Shutdown() {
    if (task_ == nullptr) {
        return;
    }
    (void)Destroy();
    ScanoutArbiter::Instance().UnregisterPresenter(this);
    stop_requested_.store(true, std::memory_order_release);
    Job job{.kind = JobKind::kBarrier, .done = nullptr};
    (void)xQueueSend(queue_, &job, kControlTimeout);
    (void)xSemaphoreTake(stopped_, kControlTimeout);
#ifdef CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    vTaskDeleteWithCaps(task_);
#else
    vTaskDelete(task_);
#endif
    task_ = nullptr;
    vQueueDelete(queue_);
    queue_ = nullptr;
    ReleaseStages();
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    framebuffer_copy_.Release();
    blitter_.Release();
#endif
}

void DirectSurfacePresenter::FillInfo(device::DirectSurfaceInfo& info) const {
    info.native_pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    info.native_flags = 0U;
    if (profile_.mode != DirectScanoutProfile::Mode::kComposited) {
        info.native_flags |= MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT;
    }
    if (profile_.mode == DirectScanoutProfile::Mode::kBlitRgb565 && profile_.rgb565_byte_swapped) {
        info.native_flags |= MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED;
    }
    info.max_full_frame_fps = profile_.max_full_frame_fps;
}

int32_t DirectSurfacePresenter::Create(const device::DirectSurfaceConfig& config,
                                       const device::DirectSurfaceReleaseSink& sink,
                                       device::DirectSurfaceInfo& info_out) {
    if (task_ == nullptr) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    // The surface may be the panel divided by one integer in both axes; every
    // present then enlarges it (SCALE_NEAREST validated per frame).
    const bool divides = config.width != 0U && config.height != 0U && width_ % config.width == 0U &&
                         height_ % config.height == 0U && width_ / config.width == height_ / config.height;
    if (sink.release == nullptr || config.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565 || !divides ||
        config.buffer_count == 0U || config.buffer_count > micropixel::device::graphics_limits::kMaxSurfaceBuffers ||
        config.flags != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (created_.load(std::memory_order_acquire)) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    taskENTER_CRITICAL(&lock_);
    config_ = config;
    release_ = sink;
    owned_mask_ = 0U;
    taskEXIT_CRITICAL(&lock_);
    host_ui_visible_.store(true, std::memory_order_release);
    composite_next_.store(true, std::memory_order_release);
    suspended_.store(false, std::memory_order_release);
    frames_scanned_out_ = 0U;
    frames_composited_ = 0U;
    created_.store(true, std::memory_order_release);
    FillInfo(info_out);
    ESP_LOGI(kTag, "surface created: %" PRIu32 "x%" PRIu32 " buffers=%" PRIu32, config.width, config.height,
             config.buffer_count);
    return MICROPIXEL_STATUS_OK;
}

int32_t DirectSurfacePresenter::Present(const device::DirectSurfacePresentation& frame) {
    if (!created_.load(std::memory_order_acquire)) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    const bool scale = ScaleRequested(frame);
    if (frame.pixels == nullptr || frame.buffer_index >= config_.buffer_count || frame.source_width == 0U ||
        frame.source_height == 0U || frame.source_width > width_ || frame.source_height > height_ ||
        frame.pitch < frame.source_width * kBytesPerRgb565 || (frame.pitch % kBytesPerRgb565) != 0U ||
        static_cast<uint64_t>(frame.pitch) * frame.source_height > frame.length ||
        (frame.flags & ~MICROPIXEL_SURFACE_PRESENT_SCALE_NEAREST) != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (scale ? (width_ % frame.source_width != 0U || height_ % frame.source_height != 0U)
              : (frame.source_width != width_ || frame.source_height != height_)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t bit = static_cast<uint8_t>(1U << frame.buffer_index);
    taskENTER_CRITICAL(&lock_);
    const bool busy = (owned_mask_ & bit) != 0U;
    if (!busy) {
        owned_mask_ |= bit;
    }
    taskEXIT_CRITICAL(&lock_);
    if (busy) {
        return MICROPIXEL_STATUS_STALE_STATE;
    }
    Job job{.kind = JobKind::kPresent, .frame = frame, .done = nullptr};
    if (xQueueSend(queue_, &job, 0) != pdTRUE) {
        taskENTER_CRITICAL(&lock_);
        owned_mask_ &= static_cast<uint8_t>(~bit);
        taskEXIT_CRITICAL(&lock_);
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    return MICROPIXEL_STATUS_OK;
}

bool DirectSurfacePresenter::PostControl(JobKind kind) {
    if (task_ == nullptr) {
        return false;
    }
    // Suspend (Host task), Destroy (Guest task) and the arbiter yield (Host
    // task) may overlap; the mutex serializes them on the shared semaphore.
    if (xSemaphoreTake(control_mutex_, kControlTimeout) != pdTRUE) {
        ESP_LOGW(kTag, "control job skipped: another control job is still running");
        return false;
    }
    (void)xSemaphoreTake(control_done_, 0);
    Job job{.kind = kind, .done = control_done_};
    bool completed = false;
    if (xQueueSend(queue_, &job, kControlTimeout) != pdTRUE) {
        ESP_LOGW(kTag, "control job dropped: queue full");
    } else if (xSemaphoreTake(control_done_, kControlTimeout) != pdTRUE) {
        ESP_LOGW(kTag, "control job timed out");
    } else {
        completed = true;
    }
    (void)xSemaphoreGive(control_mutex_);
    return completed;
}

void DirectSurfacePresenter::SetAppSurfaceFrameSource(const AppSurfaceFrameSource& source) {
    if (source.acquire == nullptr || source.handed_to_lvgl == nullptr) {
        app_surface_frame_source_ = {};
        return;
    }
    app_surface_frame_source_ = source;
    if (!AppSurfaceFrameScanoutEnabled()) {
        return;
    }
    if (AppSurfaceFrameFramebufferMode()) {
        bool dma2d = false;
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
        static_assert(FramebufferRectList::kCapacity <= graphics::Dma2dCopyEngine::kMaxBlocks);
        const esp_err_t status = framebuffer_copy_.Initialize();
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "framebuffer DMA2D copy unavailable: %s; CPU copy remains", esp_err_to_name(status));
        }
        dma2d = framebuffer_copy_.Ready();
#endif
        ESP_LOGI(kTag, "App Surface direct scanout enabled: framebuffer flips (%s copy)", dma2d ? "DMA2D" : "CPU");
    } else {
        ESP_LOGI(kTag, "App Surface direct scanout enabled: window blits aligned %ux%u",
                 static_cast<unsigned>(profile_.blit_x_alignment), static_cast<unsigned>(profile_.blit_y_alignment));
    }
}

bool DirectSurfacePresenter::PresentAppSurfaceFrame() {
    if (!AppSurfaceFrameScanoutEnabled() || task_ == nullptr) {
        return false;
    }
    // The Guest can publish faster than the panel accepts frames (60+ Hz vs a
    // 24 ms full-frame blit). One queued request pops whatever is newest in
    // the mailbox when it runs, so a second one is redundant and would only
    // fill the queue until other jobs are refused.
    if (app_surface_frame_queued_.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    Job job{.kind = JobKind::kAppSurfaceFrame, .done = nullptr};
    if (xQueueSend(queue_, &job, 0) != pdTRUE) {
        app_surface_frame_queued_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void DirectSurfacePresenter::RequestOverlayRefresh() {
    if (!AppSurfaceFrameScanoutWanted() && !app_surface_frame_exclusive_.load(std::memory_order_acquire)) {
        return;
    }
    // The gesture hint republishes every 50 ms; one queued refresh covers
    // every overlay change until the presenter task gets to it.
    if (overlay_refresh_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    Job job{.kind = JobKind::kOverlayRefresh, .done = nullptr};
    if (xQueueSend(queue_, &job, 0) != pdTRUE) {
        overlay_refresh_queued_.store(false, std::memory_order_release);
        // Only happens while App Surface frames are streaming, and each of those
        // blits repaints the overlays anyway.
        ESP_LOGD(kTag, "overlay refresh dropped: queue full");
    }
}

void DirectSurfacePresenter::ReleaseAppSurfaceFrames() {
    // The Host UI is about to own the panel; saying so now closes the window
    // in which an overlay refresh from the LVGL task could re-enter exclusive
    // scanout with the departed Guest's last frame before LVGL's next refresh
    // reports the Host UI visible.
    host_ui_visible_.store(true, std::memory_order_release);
    if (app_surface_frame_exclusive_.load(std::memory_order_acquire)) {
        (void)PostControl(JobKind::kBarrier);
    }
}

void DirectSurfacePresenter::Suspend() {
    if (!created_.load(std::memory_order_acquire) && !AppSurfaceFrameScanoutEnabled()) {
        return;
    }
    // Sticky until Resume(): the barrier below composites the displayed frame,
    // and HandlePresent drops whatever the Guest still presents on its way to
    // the safe point instead of re-entering exclusive scanout behind the Host
    // UI transition.
    suspended_.store(true, std::memory_order_release);
    composite_next_.store(true, std::memory_order_release);
    (void)PostControl(JobKind::kBarrier);
}

void DirectSurfacePresenter::Resume() { suspended_.store(false, std::memory_order_release); }

int32_t DirectSurfacePresenter::Destroy() {
    if (!created_.load(std::memory_order_acquire)) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    // Refuse new presents first so the barrier really drains everything.
    created_.store(false, std::memory_order_release);
    (void)PostControl(JobKind::kBarrier);
    taskENTER_CRITICAL(&lock_);
    owned_mask_ = 0U;
    release_ = {};
    taskEXIT_CRITICAL(&lock_);
    ESP_LOGI(kTag, "surface destroyed: scanned-out=%" PRIu32 " composited=%" PRIu32,
             frames_scanned_out_.load(std::memory_order_relaxed), frames_composited_.load(std::memory_order_relaxed));
    return MICROPIXEL_STATUS_OK;
}

void DirectSurfacePresenter::SetHostUiVisible(bool visible) {
    const bool was_visible = host_ui_visible_.exchange(visible, std::memory_order_acq_rel);
    if (visible && !was_visible && exclusive_.load(std::memory_order_acquire)) {
        // Host UI appeared while the panel belongs to the Guest: hand the panel
        // back even if the Guest stops presenting. Fire-and-forget; the LVGL
        // task must not wait on the presenter.
        Job job{.kind = JobKind::kYield, .done = nullptr};
        if (xQueueSend(queue_, &job, 0) != pdTRUE) {
            ESP_LOGW(kTag, "could not queue Host UI yield: queue full");
        }
    }
}

void DirectSurfacePresenter::TaskEntry(void* context) {
    static_cast<DirectSurfacePresenter*>(context)->Run();
    vTaskSuspend(nullptr);
}

void DirectSurfacePresenter::YieldHook(void* context) {
    auto* presenter = static_cast<DirectSurfacePresenter*>(context);
    if (presenter->exclusive_.load(std::memory_order_acquire)) {
        (void)presenter->PostControl(JobKind::kYield);
    }
}

void DirectSurfacePresenter::Run() {
    for (;;) {
        Job job{};
        if (xQueueReceive(queue_, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (job.kind) {
            case JobKind::kPresent:
                HandlePresent(job);
                break;
            case JobKind::kYield:
                LeaveExclusive(true);
                break;
            case JobKind::kBarrier:
                LeaveExclusive(true);
                ReleaseFront();
                break;
            case JobKind::kAppSurfaceFrame:
                HandleAppSurfaceFrame();
                break;
            case JobKind::kOverlayRefresh:
                HandleOverlayRefresh();
                break;
        }
        if (job.done != nullptr) {
            (void)xSemaphoreGive(job.done);
        }
        if (stop_requested_.load(std::memory_order_acquire)) {
            break;
        }
    }
    (void)xSemaphoreGive(stopped_);
}

void DirectSurfacePresenter::HandlePresent(const Job& job) {
    const device::DirectSurfacePresentation& frame = job.frame;
    if (!created_.load(std::memory_order_acquire)) {
        // Destroyed while queued: the barrier that follows returns buffers.
        ReleaseBuffer(frame.buffer_index);
        return;
    }
    if (suspended_.load(std::memory_order_acquire)) {
        // Presented between Suspend() and the Guest safe point (or queued ahead
        // of the barrier): the Host UI owns the panel now, so return the buffer
        // unshown. The barrier already composited the last scanned-out frame.
        ReleaseBuffer(frame.buffer_index);
        return;
    }
    if (app_surface_frame_exclusive_.load(std::memory_order_relaxed)) {
        // The Guest switched from Scene frames to a Direct Surface while its
        // App Surface still owned the panel: LVGL re-adopts that frame and the
        // Direct Surface path below decides on its own whether to take over.
        LeaveExclusive(false);
    }
    const bool composite_only = composite_next_.exchange(false, std::memory_order_acq_rel);
    bool exclusive = exclusive_.load(std::memory_order_relaxed);
    const bool want_exclusive = profile_.mode != DirectScanoutProfile::Mode::kComposited && !composite_only &&
                                !host_ui_visible_.load(std::memory_order_acquire);
    if (want_exclusive && !exclusive) {
        exclusive = EnterExclusive();
    } else if (!want_exclusive && exclusive) {
        // This very frame goes through LVGL, so the front buffer needs no copy.
        LeaveExclusive(false);
        exclusive = false;
    }
    if (exclusive) {
        // The previous front buffer is free as soon as this frame starts to
        // replace it on the panel.
        ReleaseFront();
        const bool scanned = profile_.mode == DirectScanoutProfile::Mode::kBlitRgb565
                                 ? ScanoutBlit(frame, job.frame.byte_swapped)
                                 : ScanoutFramebuffer(frame, job.frame.byte_swapped);
        if (scanned) {
            ++frames_scanned_out_;
            RecordCompletedScanout();
            front_ = frame;
            front_byte_swapped_ = job.frame.byte_swapped;
            has_front_ = true;
            return;
        }
        ESP_LOGW(kTag, "direct scanout failed; falling back to the composited path");
        LeaveExclusive(false);
    }
    if (Composite(frame, job.frame.byte_swapped)) {
        ++frames_composited_;
    }
    ReleaseBuffer(frame.buffer_index);
}

void DirectSurfacePresenter::RecordCompletedScanout() {
#if CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG
    // The blit path waits for transfer completion. A framebuffer flip or
    // composited submission is not a completion timestamp and is excluded.
    if (profile_.mode != DirectScanoutProfile::Mode::kBlitRgb565) return;
    const auto now = static_cast<uint64_t>(esp_timer_get_time());
    frame_timing_.Record(now);
    if (frame_timing_.elapsed_us() < 60000000U) return;
    ESP_LOGI(kTag,
             "scanout-timing: intervals=%" PRIu32 " elapsed-us=%" PRIu64 " fps-milli=%" PRIu32 " p95-upper-us=%" PRIu32
             " bin-us=%" PRIu32,
             frame_timing_.intervals(), frame_timing_.elapsed_us(), frame_timing_.fps_milli(),
             frame_timing_.p95_upper_us(), FrameTiming::kBinWidthUs);
    frame_timing_.Reset();
    frame_timing_.Record(now);
#endif
}

bool DirectSurfacePresenter::EnterExclusive() {
#if CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG
    frame_timing_.Reset();
#endif
    ScanoutArbiter& arbiter = ScanoutArbiter::Instance();
    if (!arbiter.TryEnterPresenterScanout()) {
        return false;
    }
    const esp_err_t status = esp_lv_adapter_set_dummy_draw(display_, true);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not enter dummy draw: %s", esp_err_to_name(status));
        arbiter.LeavePresenterScanout();
        return false;
    }
    exclusive_.store(true, std::memory_order_release);
    return true;
}

void DirectSurfacePresenter::LeaveExclusive(bool composite_front) {
    if (!exclusive_.load(std::memory_order_acquire)) {
        return;
    }
    if (composite_front && has_front_) {
        // Publish the displayed frame into the App Surface first so the full
        // refresh triggered below adopts it instead of a stale surface.
        if (Composite(front_, front_byte_swapped_)) {
            ++frames_composited_;
        }
    }
    const esp_err_t status = esp_lv_adapter_set_dummy_draw(display_, false);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not leave dummy draw: %s", esp_err_to_name(status));
    }
    exclusive_.store(false, std::memory_order_release);
    ScanoutArbiter::Instance().LeavePresenterScanout();
    ReleaseFront();
    // The Host UI owns the panel now; hand the staging frames back so the
    // system transition compositors can use them.
    ReleaseScanoutStages();
    if (app_surface_frame_exclusive_.exchange(false, std::memory_order_acq_rel)) {
        for (OverlayFootprint& footprint : overlay_footprints_) {
            footprint = {};
        }
        // Dummy draw is off: LVGL's full redraw of the frame reaches the panel.
        app_surface_frame_source_.handed_to_lvgl(app_surface_frame_source_.context, true);
    }
}

void DirectSurfacePresenter::HandleAppSurfaceFrame() {
    // Cleared before the mailbox is read: a publish racing with this job gets
    // its own request instead of assuming this one will see its frame.
    app_surface_frame_queued_.store(false, std::memory_order_release);
    if (!AppSurfaceFrameScanoutWanted()) {
        // Host UI appeared, the App is suspended or a Direct Surface took over
        // since the Guest published: the mailbox frame is LVGL's to show.
        if (app_surface_frame_exclusive_.load(std::memory_order_relaxed)) {
            LeaveExclusive(false);
        } else {
            app_surface_frame_source_.handed_to_lvgl(app_surface_frame_source_.context, false);
        }
        return;
    }
    if (!EnterAppSurfaceFrameExclusive()) {
        app_surface_frame_source_.handed_to_lvgl(app_surface_frame_source_.context, false);
        return;
    }
    AppSurfaceFrame frame{};
    if (!app_surface_frame_source_.acquire(app_surface_frame_source_.context, true, frame)) {
        // Several requests for one mailbox entry: the first one took it.
        return;
    }
    if (ScanoutAppSurfaceFrame(frame, app_surface_frame_whole_next_)) {
        app_surface_frame_whole_next_ = false;
        ++frames_scanned_out_;
        RecordCompletedScanout();
        return;
    }
    ESP_LOGW(kTag, "App Surface scanout failed; falling back to LVGL");
    LeaveExclusive(false);
}

bool DirectSurfacePresenter::EnterAppSurfaceFrameExclusive() {
    if (exclusive_.load(std::memory_order_relaxed)) {
        // Either App Surface frames already own the panel or a Direct Surface does.
        return app_surface_frame_exclusive_.load(std::memory_order_relaxed);
    }
    if (!EnterExclusive()) {
        return false;
    }
    app_surface_frame_exclusive_.store(true, std::memory_order_release);
    app_surface_frame_whole_next_ = true;
    for (OverlayFootprint& footprint : overlay_footprints_) {
        footprint = {};
    }
    // Both DPI framebuffers hold whatever LVGL last drew.
    framebuffer_whole_pending_ = 2U;
    for (FramebufferHistory& history : framebuffer_history_) {
        history = {};
    }
    return true;
}

void DirectSurfacePresenter::HandleOverlayRefresh() {
    overlay_refresh_queued_.store(false, std::memory_order_release);
    // Also the way a static App (no further Guest frames) gets its HUD onto
    // the panel: the overlay request takes the panel and sends the frame LVGL
    // last showed together with the overlay.
    if (!AppSurfaceFrameScanoutWanted() || !EnterAppSurfaceFrameExclusive()) {
        return;
    }
    // A frame the Guest published meanwhile is taken first and its damage
    // sent like any other frame: acquiring it makes it the displayed surface,
    // so dropping its damage list here would leave those pixels stale on the
    // panel for good. Only without one is the displayed frame re-read.
    AppSurfaceFrame frame{};
    const bool pending = app_surface_frame_source_.acquire(app_surface_frame_source_.context, true, frame);
    if (!pending && !app_surface_frame_source_.acquire(app_surface_frame_source_.context, false, frame)) {
        LeaveExclusive(false);
        return;
    }
    if (AppSurfaceFrameFramebufferMode()) {
        // A flip always re-blends every overlay into the frame it sends, so
        // one ordinary scanout of the frame (its damage, or none) is the
        // overlay refresh. The re-read displayed frame carries no new damage;
        // the flip repaints the previous damage and footprints on its own.
        if (!pending) {
            frame.whole = false;
            frame.rect_count = 0U;
        }
        if (!ScanoutAppSurfaceFrame(frame, app_surface_frame_whole_next_)) {
            ESP_LOGW(kTag, "App Surface scanout failed; falling back to LVGL");
            LeaveExclusive(false);
            return;
        }
        app_surface_frame_whole_next_ = false;
        if (pending) {
            ++frames_scanned_out_;
            RecordCompletedScanout();
        }
        return;
    }
    if (app_surface_frame_whole_next_) {
        if (ScanoutAppSurfaceFrame(frame, true)) {
            app_surface_frame_whole_next_ = false;
        } else {
            ESP_LOGW(kTag, "App Surface scanout failed; falling back to LVGL");
            LeaveExclusive(false);
        }
        return;
    }
    if (pending) {
        if (!ScanoutAppSurfaceFrame(frame, false)) {
            ESP_LOGW(kTag, "App Surface scanout failed; falling back to LVGL");
            LeaveExclusive(false);
            return;
        }
        ++frames_scanned_out_;
        RecordCompletedScanout();
        if (frame.whole) {
            // Everything, overlays included, has just been sent.
            return;
        }
    }
    // Repaint where each layer was and where it is now; ScanoutAppSurfaceFrameRect
    // blends the current overlay and records the new footprint.
    for (int layer_index = 0; layer_index < kScanoutOverlayLayers; ++layer_index) {
        OverlayLayer& layer = overlay_layers_[layer_index];
        OverlayFootprint& footprint = overlay_footprints_[layer_index];
        AppSurfaceFrameRect area = footprint.rect;
        bool any = footprint.valid;
        const int pinned = AcquireOverlayForBlend(layer);
        if (pinned != kNoOverlaySlot) {
            const OverlaySlot& overlay = layer.slots[pinned];
            const AppSurfaceFrameRect current{.x = static_cast<uint32_t>(overlay.x < 0 ? 0 : overlay.x),
                                              .y = static_cast<uint32_t>(overlay.y < 0 ? 0 : overlay.y),
                                              .width = overlay.width,
                                              .height = overlay.height};
            if (any) {
                const uint32_t x1 = current.x < area.x ? current.x : area.x;
                const uint32_t y1 = current.y < area.y ? current.y : area.y;
                const uint32_t x2 =
                    current.x + current.width > area.x + area.width ? current.x + current.width : area.x + area.width;
                const uint32_t y2 = current.y + current.height > area.y + area.height ? current.y + current.height
                                                                                      : area.y + area.height;
                area = {.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1};
            } else {
                area = current;
            }
            any = true;
        }
        ReleaseOverlayAfterBlend(layer);
        if (!any) {
            continue;
        }
        footprint = {};
        const AppSurfaceFrameRect aligned = AlignRect(area);
        if (aligned.width == 0U || aligned.height == 0U) {
            continue;
        }
        if (!ScanoutAppSurfaceFrameRect(frame, aligned)) {
            ESP_LOGW(kTag, "overlay refresh failed; falling back to LVGL");
            LeaveExclusive(false);
            return;
        }
    }
}

DirectSurfacePresenter::AppSurfaceFrameStats DirectSurfacePresenter::TakeAppSurfaceFrameStats() {
    taskENTER_CRITICAL(&lock_);
    const AppSurfaceFrameStats stats = app_surface_frame_stats_;
    app_surface_frame_stats_ = {};
    taskEXIT_CRITICAL(&lock_);
    return stats;
}

void DirectSurfacePresenter::FramebufferRectList::Add(const AppSurfaceFrameRect& rect) {
    if (whole || rect.width == 0U || rect.height == 0U) {
        return;
    }
    if (count == kCapacity) {
        whole = true;
        return;
    }
    rects[count++] = rect;
}

bool DirectSurfacePresenter::ScanoutAppSurfaceFrameToFramebuffer(const AppSurfaceFrame& frame, bool whole_frame) {
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    if (framebuffers_ == nullptr) {
        return false;
    }
    uint8_t* target = framebuffers_->AcquireFree();
    if (target == nullptr) {
        return false;
    }
    // DPI framebuffers are packed BGR888 (see the system transition compositor).
    constexpr uint32_t kBytesPerRgb888 = 3U;
    const uint32_t stride = width_ * kBytesPerRgb888;
    const uint32_t frame_bytes = stride * height_;

    // What the free framebuffer misses: this frame's damage, the damage the
    // previous flip carried (it went into the other buffer) and the overlay
    // pixels this buffer still holds from two flips ago.
    const FramebufferHistory& previous = framebuffer_history_[0];
    const FramebufferHistory& reused = framebuffer_history_[1];
    // The frame's own change set; what the history remembers for the next
    // flip, independent of how much this flip had to catch up.
    const bool frame_whole = whole_frame || frame.whole || frame.rect_count > AppSurfaceFrame::kMaxRects;
    FramebufferRectList list{};
    list.whole = frame_whole || framebuffer_whole_pending_ > 0U || framebuffers_->Count() != 2U || previous.whole;
    if (!list.whole) {
        for (uint32_t index = 0U; index < frame.rect_count; ++index) {
            list.Add(AlignRect(frame.rects[index]));
        }
        for (uint32_t index = 0U; index < previous.damage_count; ++index) {
            list.Add(previous.damage[index]);
        }
        for (const OverlayFootprint& footprint : reused.footprints) {
            if (footprint.valid) {
                list.Add(footprint.rect);
            }
        }
    }
    if (framebuffer_whole_pending_ > 0U) {
        --framebuffer_whole_pending_;
    }
    const AppSurfaceFrameRect panel = PanelRect();
    const AppSurfaceFrameRect* rects = list.whole ? &panel : list.rects;
    const uint32_t rect_count = list.whole ? 1U : list.count;
    const int64_t copy_started_us = esp_timer_get_time();
    const uint32_t blocks_before = framebuffer_copy_.BlocksCopied();
    const uint64_t cache_before = framebuffer_copy_.CacheSyncMicros();
    const uint64_t wait_before = framebuffer_copy_.TransferWaitMicros();
    uint64_t pixels = 0U;
    for (uint32_t index = 0U; index < rect_count; ++index) {
        pixels += static_cast<uint64_t>(rects[index].width) * rects[index].height;
    }

    bool copied = false;
    if (frame.bytes_per_pixel == kBytesPerRgb888) {
        if (framebuffer_copy_.Ready() && rect_count > 0U) {
            const graphics::ConstPixelSurface source{
                .pixels = frame.pixels,
                .size = frame.length,
                .width = frame.stride / kBytesPerRgb888,
                .height = height_,
                .stride = frame.stride,
                .format = graphics::SurfacePixelFormat::kBgr888,
            };
            const graphics::PixelSurface destination{
                .pixels = target,
                .size = frame_bytes,
                .width = width_,
                .height = height_,
                .stride = stride,
                .format = graphics::SurfacePixelFormat::kBgr888,
            };
            graphics::Dma2dCopyBlock blocks[FramebufferRectList::kCapacity]{};
            for (uint32_t index = 0U; index < rect_count; ++index) {
                const graphics::SurfaceRect area{.x = static_cast<int32_t>(rects[index].x),
                                                 .y = static_cast<int32_t>(rects[index].y),
                                                 .width = static_cast<int32_t>(rects[index].width),
                                                 .height = static_cast<int32_t>(rects[index].height)};
                blocks[index] = {
                    .source = source, .source_rect = area, .destination = destination, .destination_rect = area};
            }
            copied = framebuffer_copy_.CopyBlocks(blocks, rect_count);
        }
        if (!copied) {
            for (uint32_t index = 0U; index < rect_count; ++index) {
                const AppSurfaceFrameRect& rect = rects[index];
                const uint32_t row_bytes = rect.width * kBytesPerRgb888;
                const uint8_t* source_row = frame.pixels + rect.y * frame.stride + rect.x * kBytesPerRgb888;
                uint8_t* target_row = target + rect.y * stride + rect.x * kBytesPerRgb888;
                for (uint32_t row = 0U; row < rect.height; ++row, source_row += frame.stride, target_row += stride) {
                    std::memcpy(target_row, source_row, row_bytes);
                }
                if (!WriteBackForDma(target + rect.y * stride, rect.height * stride)) {
                    return false;
                }
            }
        }
    } else {
        // RGB565 App Surface: PPA expands each rectangle into the framebuffer.
        for (uint32_t index = 0U; index < rect_count; ++index) {
            const AppSurfaceFrameRect& rect = rects[index];
            const uint8_t* source_rows = frame.pixels + rect.y * frame.stride;
            bool converted = false;
            if (blitter_.Ready() && WriteBackForDma(source_rows, rect.height * frame.stride)) {
                const PpaSrmBlit request{
                    .source = frame.pixels,
                    .source_width = frame.stride / kBytesPerRgb565,
                    .source_height = height_,
                    .source_region = {.x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height},
                    .source_mode = PPA_SRM_COLOR_MODE_RGB565,
                    .destination = target,
                    .destination_width = width_,
                    .destination_height = height_,
                    .destination_allocation_bytes = frame_bytes,
                    .destination_x = rect.x,
                    .destination_y = rect.y,
                    .destination_mode = PPA_SRM_COLOR_MODE_RGB888,
                    .scale_x = 1.0F,
                    .scale_y = 1.0F,
                    .input_byte_swap = false,
                };
                converted = blitter_.Blit(request) == ESP_OK;
            }
            if (!converted) {
                ConvertRgb565(frame.pixels + rect.y * frame.stride + rect.x * kBytesPerRgb565, frame.stride, rect.width,
                              rect.height, false, target + rect.y * stride + rect.x * kBytesPerRgb888, stride,
                              rect.width, rect.height, true, false);
                if (!WriteBackForDma(target + rect.y * stride, rect.height * stride)) {
                    return false;
                }
            }
        }
    }

    // Overlays go on top of the finished frame; their footprints are what the
    // flip after next has to paint over.
    OverlayFootprint footprints[kScanoutOverlayLayers]{};
    if (OverlayActive()) {
        for (int layer_index = 0; layer_index < kScanoutOverlayLayers; ++layer_index) {
            OverlayLayer& layer = overlay_layers_[layer_index];
            const int pinned = AcquireOverlayForBlend(layer);
            if (pinned == kNoOverlaySlot) {
                continue;
            }
            const OverlaySlot& overlay = layer.slots[pinned];
            const AppSurfaceFrameRect covered = AlignRect({.x = static_cast<uint32_t>(overlay.x < 0 ? 0 : overlay.x),
                                                           .y = static_cast<uint32_t>(overlay.y < 0 ? 0 : overlay.y),
                                                           .width = overlay.width,
                                                           .height = overlay.height});
            if (covered.width != 0U && covered.height != 0U) {
                BlendOverlay(overlay, target, stride, true, panel);
                footprints[layer_index] = {.rect = covered, .valid = true};
            }
            ReleaseOverlayAfterBlend(layer);
            if (footprints[layer_index].valid &&
                !WriteBackForDma(target + covered.y * stride, covered.height * stride)) {
                return false;
            }
        }
    }

    framebuffer_history_[1] = framebuffer_history_[0];
    FramebufferHistory& current = framebuffer_history_[0];
    current = {};
    current.whole = frame_whole;
    if (!frame_whole) {
        for (uint32_t index = 0U; index < frame.rect_count; ++index) {
            const AppSurfaceFrameRect aligned = AlignRect(frame.rects[index]);
            if (aligned.width != 0U && aligned.height != 0U) {
                current.damage[current.damage_count++] = aligned;
            }
        }
    }
    for (int layer_index = 0; layer_index < kScanoutOverlayLayers; ++layer_index) {
        current.footprints[layer_index] = footprints[layer_index];
        overlay_footprints_[layer_index] = footprints[layer_index];
    }

    const int64_t flip_started_us = esp_timer_get_time();
    const esp_err_t status = framebuffers_->Submit(target);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "framebuffer submit failed: %s", esp_err_to_name(status));
        return false;
    }
    const int64_t flipped_us = esp_timer_get_time();
    taskENTER_CRITICAL(&lock_);
    ++app_surface_frame_stats_.frames;
    app_surface_frame_stats_.whole += list.whole ? 1U : 0U;
    app_surface_frame_stats_.copy_us += static_cast<uint64_t>(flip_started_us - copy_started_us);
    app_surface_frame_stats_.flip_us += static_cast<uint64_t>(flipped_us - flip_started_us);
    app_surface_frame_stats_.pixels += pixels;
    app_surface_frame_stats_.blocks += framebuffer_copy_.BlocksCopied() - blocks_before;
    app_surface_frame_stats_.cache_us += framebuffer_copy_.CacheSyncMicros() - cache_before;
    app_surface_frame_stats_.wait_us += framebuffer_copy_.TransferWaitMicros() - wait_before;
    taskEXIT_CRITICAL(&lock_);
    return true;
#else
    (void)frame;
    (void)whole_frame;
    return false;
#endif
}

AppSurfaceFrameRect DirectSurfacePresenter::AlignRect(const AppSurfaceFrameRect& rect) const {
    const uint32_t align_x = profile_.blit_x_alignment == 0U ? 1U : profile_.blit_x_alignment;
    const uint32_t align_y = profile_.blit_y_alignment == 0U ? 1U : profile_.blit_y_alignment;
    if (rect.width == 0U || rect.height == 0U || rect.x >= width_ || rect.y >= height_) {
        return {};
    }
    const uint32_t x1 = rect.x / align_x * align_x;
    const uint32_t y1 = rect.y / align_y * align_y;
    uint32_t x2 = (rect.x + rect.width + align_x - 1U) / align_x * align_x;
    uint32_t y2 = (rect.y + rect.height + align_y - 1U) / align_y * align_y;
    x2 = x2 > width_ ? width_ : x2;
    y2 = y2 > height_ ? height_ : y2;
    return {.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1};
}

bool DirectSurfacePresenter::ScanoutAppSurfaceFrame(const AppSurfaceFrame& frame, bool whole_frame) {
    const uint32_t bytes_per_pixel = frame.bytes_per_pixel;
    if (frame.pixels == nullptr || (bytes_per_pixel != 2U && bytes_per_pixel != 3U) ||
        frame.stride < width_ * bytes_per_pixel || (frame.stride % bytes_per_pixel) != 0U ||
        static_cast<uint64_t>(frame.stride) * height_ > frame.length) {
        return false;
    }
    if (AppSurfaceFrameFramebufferMode()) {
        return ScanoutAppSurfaceFrameToFramebuffer(frame, whole_frame);
    }
    if (bytes_per_pixel != kBytesPerRgb565) {
        // Window blits carry RGB565 only.
        return false;
    }
    if (whole_frame || frame.whole || frame.rect_count > AppSurfaceFrame::kMaxRects) {
        return ScanoutAppSurfaceFrameRect(frame, PanelRect());
    }
    for (uint32_t index = 0U; index < frame.rect_count; ++index) {
        const AppSurfaceFrameRect aligned = AlignRect(frame.rects[index]);
        if (aligned.width == 0U || aligned.height == 0U) {
            continue;
        }
        if (!ScanoutAppSurfaceFrameRect(frame, aligned)) {
            return false;
        }
    }
    return true;
}

bool DirectSurfacePresenter::ScanoutAppSurfaceFrameRect(const AppSurfaceFrame& frame, const AppSurfaceFrameRect& rect) {
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    if (rect.width == 0U || rect.height == 0U || rect.x + rect.width > width_ || rect.y + rect.height > height_) {
        return false;
    }
    if (!EnsureStage(wire_stage_)) {
        return false;
    }
    const uint32_t rect_stride = rect.width * kBytesPerRgb565;
    const uint32_t rect_bytes = rect_stride * rect.height;
    if (rect_bytes > stage_bytes_) {
        return false;
    }
    // One PPA pass crops the rectangle out of the App Surface, swaps the byte
    // order the panel wants and packs it for the window transfer.
    const uint8_t* source_rows = frame.pixels + rect.y * frame.stride;
    bool staged = false;
    if (blitter_.Ready() && WriteBackForDma(source_rows, rect.height * frame.stride)) {
        const PpaSrmBlit request{
            .source = frame.pixels,
            .source_width = frame.stride / kBytesPerRgb565,
            .source_height = height_,
            .source_region = {.x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height},
            .source_mode = PPA_SRM_COLOR_MODE_RGB565,
            .destination = wire_stage_,
            .destination_width = rect.width,
            .destination_height = rect.height,
            .destination_allocation_bytes = stage_bytes_,
            .destination_x = 0U,
            .destination_y = 0U,
            .destination_mode = PPA_SRM_COLOR_MODE_RGB565,
            .scale_x = 1.0F,
            .scale_y = 1.0F,
            .input_byte_swap = profile_.rgb565_byte_swapped,
        };
        staged = blitter_.Blit(request) == ESP_OK;
    }
    bool cpu_wrote_stage = false;
    if (!staged) {
        ConvertRgb565(frame.pixels + rect.y * frame.stride + rect.x * kBytesPerRgb565, frame.stride, rect.width,
                      rect.height, false, wire_stage_, rect_stride, rect.width, rect.height, false,
                      profile_.rgb565_byte_swapped);
        cpu_wrote_stage = true;
    }
    if (OverlayActive()) {
        for (int layer_index = 0; layer_index < kScanoutOverlayLayers; ++layer_index) {
            OverlayLayer& layer = overlay_layers_[layer_index];
            const int pinned = AcquireOverlayForBlend(layer);
            if (pinned == kNoOverlaySlot) {
                continue;
            }
            const OverlaySlot& overlay = layer.slots[pinned];
            const AppSurfaceFrameRect covered = AlignRect({.x = static_cast<uint32_t>(overlay.x < 0 ? 0 : overlay.x),
                                                           .y = static_cast<uint32_t>(overlay.y < 0 ? 0 : overlay.y),
                                                           .width = overlay.width,
                                                           .height = overlay.height});
            const bool intersects = covered.width != 0U && covered.height != 0U && covered.x < rect.x + rect.width &&
                                    rect.x < covered.x + covered.width && covered.y < rect.y + rect.height &&
                                    rect.y < covered.y + covered.height;
            if (intersects) {
                BlendOverlay(overlay, wire_stage_, rect_stride, false, rect);
                overlay_footprints_[layer_index] = {.rect = covered, .valid = true};
                cpu_wrote_stage = true;
            }
            ReleaseOverlayAfterBlend(layer);
        }
    }
    if (cpu_wrote_stage && !WriteBackForDma(wire_stage_, rect_bytes)) {
        return false;
    }
    const esp_err_t status = esp_lv_adapter_dummy_draw_blit(
        display_, static_cast<int>(rect.x), static_cast<int>(rect.y), static_cast<int>(rect.x + rect.width),
        static_cast<int>(rect.y + rect.height), wire_stage_, true);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "dummy draw window blit failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
#else
    (void)frame;
    (void)rect;
    return false;
#endif
}

bool DirectSurfacePresenter::EnsureStage(uint8_t*& stage) {
    if (stage != nullptr) {
        return true;
    }
    const uint32_t bytes = width_ * height_ * kBytesPerRgb565;
    const uint32_t allocation = (bytes + kStageAlignment - 1U) / kStageAlignment * kStageAlignment;
    // Stages are only needed while this presenter owns the panel, so they come
    // from the shared scanout pool when the board provides one and go back on
    // LeaveExclusive; system compositors then reuse the same frames.
    ScanoutStagePool& pool = ScanoutStagePool::Instance();
    if (pool.Ready()) {
        stage = pool.Acquire(allocation);
        if (stage == nullptr) {
            return false;
        }
        stage_bytes_ = pool.slot_bytes();
        return true;
    }
    stage = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kStageAlignment, allocation, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stage == nullptr) {
        ESP_LOGE(kTag, "could not allocate %" PRIu32 " B scanout stage", allocation);
        return false;
    }
    stage_bytes_ = allocation;
    return true;
}

void DirectSurfacePresenter::ReleaseStage(uint8_t*& stage) {
    if (stage == nullptr) {
        return;
    }
    ScanoutStagePool& pool = ScanoutStagePool::Instance();
    if (pool.Owns(stage)) {
        pool.Release(stage);
    } else {
        heap_caps_free(stage);
    }
    stage = nullptr;
}

void DirectSurfacePresenter::ReleaseScanoutStages() {
    ReleaseStage(wire_stage_);
    ReleaseStage(scale_stage_);
    stage_bytes_ = 0U;
}

void DirectSurfacePresenter::ReleaseStages() {
    ReleaseScanoutStages();
    for (OverlayLayer& layer : overlay_layers_) {
        for (OverlaySlot& slot : layer.slots) {
            heap_caps_free(slot.pixels);
            heap_caps_free(slot.wire);
            heap_caps_free(slot.alpha);
            slot = {};
        }
        layer.published = kNoOverlaySlot;
        layer.pinned = kNoOverlaySlot;
    }
    overlay_active_mask_.store(0U, std::memory_order_release);
}

bool DirectSurfacePresenter::EnsureOverlaySlot(OverlaySlot& slot, uint32_t bytes) {
    if (slot.pixel_capacity >= bytes) {
        return true;
    }
    // The slot is neither published nor pinned when this runs, so the old
    // image can go.
    heap_caps_free(slot.pixels);
    slot.pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    slot.pixel_capacity = slot.pixels != nullptr ? bytes : 0U;
    if (slot.pixels == nullptr) {
        ESP_LOGE(kTag, "could not allocate %" PRIu32 " B overlay slot", bytes);
        return false;
    }
    return true;
}

bool DirectSurfacePresenter::SetOverlay(ScanoutOverlayLayer layer_id, const ScanoutOverlayImage& image) {
    const int layer_index = static_cast<int>(layer_id);
    if (layer_index < 0 || layer_index >= kScanoutOverlayLayers || image.pixels == nullptr || image.width == 0U ||
        image.height == 0U || image.width > width_ || image.height > kOverlayMaxHeight ||
        image.stride < image.width * 4U) {
        return false;
    }
    OverlayLayer& layer = overlay_layers_[layer_index];
    // Pick the slot nobody holds: not published and not pinned by a blend.
    taskENTER_CRITICAL(&lock_);
    int target = kNoOverlaySlot;
    for (int index = 0; index < kOverlaySlots; ++index) {
        if (index != layer.published && index != layer.pinned) {
            target = index;
            break;
        }
    }
    taskEXIT_CRITICAL(&lock_);
    if (target == kNoOverlaySlot) {
        return false;
    }
    OverlaySlot& slot = layer.slots[target];
    const uint32_t stride = image.width * 4U;
    if (!EnsureOverlaySlot(slot, stride * image.height)) {
        return false;
    }
    for (uint32_t row = 0U; row < image.height; ++row) {
        std::memcpy(slot.pixels + row * stride, image.pixels + row * image.stride, stride);
    }
    if (profile_.mode == DirectScanoutProfile::Mode::kBlitRgb565 && !PrepareOverlaySlot(slot, image)) {
        // No fast planes: BlendOverlay falls back to the ARGB copy.
        heap_caps_free(slot.wire);
        heap_caps_free(slot.alpha);
        slot.wire = nullptr;
        slot.alpha = nullptr;
        slot.prepared_capacity = 0U;
    }
    slot.width = image.width;
    slot.height = image.height;
    slot.stride = stride;
    slot.x = image.x;
    slot.y = image.y;
    taskENTER_CRITICAL(&lock_);
    layer.published = target;
    taskEXIT_CRITICAL(&lock_);
    overlay_active_mask_.fetch_or(static_cast<uint8_t>(1U << layer_index), std::memory_order_release);
    // App Surface frames only reach the panel where the Guest changed something, so
    // a new overlay image needs its own window transfer.
    RequestOverlayRefresh();
    return true;
}

bool DirectSurfacePresenter::PrepareOverlaySlot(OverlaySlot& slot, const ScanoutOverlayImage& image) {
    const uint32_t pixels = image.width * image.height;
    if (slot.prepared_capacity < pixels) {
        heap_caps_free(slot.wire);
        heap_caps_free(slot.alpha);
        slot.wire = nullptr;
        slot.alpha = nullptr;
        slot.prepared_capacity = 0U;
        const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        slot.wire = static_cast<uint16_t*>(heap_caps_malloc(pixels * sizeof(uint16_t), caps));
        slot.alpha = static_cast<uint8_t*>(heap_caps_malloc(pixels, caps));
        if (slot.wire == nullptr || slot.alpha == nullptr) {
            return false;
        }
        slot.prepared_capacity = pixels;
    }
    const bool swap_bytes = profile_.rgb565_byte_swapped;
    uint16_t* wire = slot.wire;
    uint8_t* alpha = slot.alpha;
    for (uint32_t row = 0U; row < image.height; ++row) {
        const uint8_t* source = image.pixels + row * image.stride;
        for (uint32_t x = 0U; x < image.width; ++x, source += 4U) {
            const uint16_t packed = static_cast<uint16_t>(((static_cast<uint32_t>(source[2]) >> 3U) << 11U) |
                                                          ((static_cast<uint32_t>(source[1]) >> 2U) << 5U) |
                                                          (static_cast<uint32_t>(source[0]) >> 3U));
            *wire++ = swap_bytes ? Swap16(packed) : packed;
            *alpha++ = source[3];
        }
    }
    return true;
}

void DirectSurfacePresenter::ClearOverlay(ScanoutOverlayLayer layer_id) {
    const int layer_index = static_cast<int>(layer_id);
    if (layer_index < 0 || layer_index >= kScanoutOverlayLayers) {
        return;
    }
    overlay_active_mask_.fetch_and(static_cast<uint8_t>(~(1U << layer_index)), std::memory_order_release);
    taskENTER_CRITICAL(&lock_);
    overlay_layers_[layer_index].published = kNoOverlaySlot;
    taskEXIT_CRITICAL(&lock_);
    RequestOverlayRefresh();
}

int DirectSurfacePresenter::AcquireOverlayForBlend(OverlayLayer& layer) {
    taskENTER_CRITICAL(&lock_);
    layer.pinned = layer.published;
    const int pinned = layer.pinned;
    taskEXIT_CRITICAL(&lock_);
    return pinned;
}

void DirectSurfacePresenter::ReleaseOverlayAfterBlend(OverlayLayer& layer) {
    taskENTER_CRITICAL(&lock_);
    layer.pinned = kNoOverlaySlot;
    taskEXIT_CRITICAL(&lock_);
}

void DirectSurfacePresenter::BlendOverlay(const OverlaySlot& overlay, uint8_t* target, uint32_t target_stride,
                                          bool bgr888, const AppSurfaceFrameRect& clip) {
    // Clip to the target rectangle; the overlay is small so a straight
    // per-pixel blend is cheaper than setting up the PPA for it.
    const int32_t clip_left = static_cast<int32_t>(clip.x);
    const int32_t clip_top = static_cast<int32_t>(clip.y);
    const int32_t clip_right = static_cast<int32_t>(clip.x + clip.width);
    const int32_t clip_bottom = static_cast<int32_t>(clip.y + clip.height);
    const int32_t left = overlay.x < clip_left ? clip_left : overlay.x;
    const int32_t top = overlay.y < clip_top ? clip_top : overlay.y;
    const int32_t right = overlay.x + static_cast<int32_t>(overlay.width) > clip_right
                              ? clip_right
                              : overlay.x + static_cast<int32_t>(overlay.width);
    const int32_t bottom = overlay.y + static_cast<int32_t>(overlay.height) > clip_bottom
                               ? clip_bottom
                               : overlay.y + static_cast<int32_t>(overlay.height);
    if (left >= right || top >= bottom) {
        return;
    }
    const bool swap_bytes = profile_.rgb565_byte_swapped;
    for (int32_t y = top; y < bottom; ++y) {
        const uint8_t* source = overlay.pixels + static_cast<uint32_t>(y - overlay.y) * overlay.stride +
                                static_cast<uint32_t>(left - overlay.x) * 4U;
        // `row` starts at the clip's first column; x below is a panel column.
        uint8_t* row = target + static_cast<uint32_t>(y - clip_top) * target_stride;
        if (bgr888) {
            for (int32_t x = left; x < right; ++x, source += 4U) {
                const uint32_t alpha = source[3];
                if (alpha == 0U) {
                    continue;
                }
                const uint32_t inverse = 255U - alpha;
                uint8_t* pixel = row + static_cast<uint32_t>(x - clip_left) * 3U;
                pixel[0] = static_cast<uint8_t>((source[0] * alpha + pixel[0] * inverse + 127U) / 255U);
                pixel[1] = static_cast<uint8_t>((source[1] * alpha + pixel[1] * inverse + 127U) / 255U);
                pixel[2] = static_cast<uint8_t>((source[2] * alpha + pixel[2] * inverse + 127U) / 255U);
            }
            continue;
        }
        // RGB565 runs on the zero-copy scanout path in front of the panel DMA,
        // so every microsecond here is frame time. Opaque and clear texels
        // (the bulk of a HUD box) skip the math; partial alpha blends in 5/6-bit
        // space with a 1/256 scale, which is within one LSB of the exact result.
        // Clip-relative columns: pixels[x - clip_left] is panel column x.
        auto* pixels = reinterpret_cast<uint16_t*>(row);
        const uint32_t prepared_index =
            static_cast<uint32_t>(y - overlay.y) * overlay.width + static_cast<uint32_t>(left - overlay.x);
        const uint16_t* wire = overlay.wire != nullptr ? overlay.wire + prepared_index : nullptr;
        const uint8_t* alpha_plane = overlay.alpha != nullptr ? overlay.alpha + prepared_index : nullptr;
        for (int32_t x = left; x < right; ++x, source += 4U) {
            uint32_t alpha;
            uint32_t source_wire;  // already in panel byte order
            if (wire != nullptr) {
                alpha = *alpha_plane;
                if (alpha == 0U || alpha == 255U) {
                    // Runs of clear or opaque texels are the bulk of a HUD box:
                    // skip or memcpy them instead of stepping pixel by pixel.
                    int32_t run = 1;
                    while (x + run < right && alpha_plane[run] == alpha) {
                        ++run;
                    }
                    if (alpha == 255U) {
                        std::memcpy(pixels + (x - clip_left), wire, static_cast<size_t>(run) * sizeof(uint16_t));
                    }
                    x += run - 1;
                    source += static_cast<uint32_t>(run - 1) * 4U;
                    wire += run;
                    alpha_plane += run;
                    continue;
                }
                source_wire = *wire++;
                ++alpha_plane;
            } else {
                alpha = source[3];
                const uint32_t canonical = ((static_cast<uint32_t>(source[2]) >> 3U) << 11U) |
                                           ((static_cast<uint32_t>(source[1]) >> 2U) << 5U) |
                                           (static_cast<uint32_t>(source[0]) >> 3U);
                source_wire = swap_bytes ? Swap16(static_cast<uint16_t>(canonical)) : canonical;
            }
            if (alpha == 0U) {
                continue;
            }
            if (alpha == 255U) {
                pixels[x - clip_left] = static_cast<uint16_t>(source_wire);
                continue;
            }
            uint32_t src = source_wire;
            uint32_t dst = pixels[x - clip_left];
            if (swap_bytes) {
                src = Swap16(static_cast<uint16_t>(src));
                dst = Swap16(static_cast<uint16_t>(dst));
            }
            const uint32_t inverse = 256U - alpha;
            const uint32_t red = (((src >> 11U) & 0x1FU) * alpha + ((dst >> 11U) & 0x1FU) * inverse) >> 8U;
            const uint32_t green = (((src >> 5U) & 0x3FU) * alpha + ((dst >> 5U) & 0x3FU) * inverse) >> 8U;
            const uint32_t blue = ((src & 0x1FU) * alpha + (dst & 0x1FU) * inverse) >> 8U;
            const uint16_t packed = static_cast<uint16_t>((red << 11U) | (green << 5U) | blue);
            pixels[x - clip_left] = swap_bytes ? Swap16(packed) : packed;
        }
    }
}

bool DirectSurfacePresenter::BlendOverlayInto(uint8_t* target, uint32_t target_stride, bool bgr888) {
    bool blended = false;
    for (OverlayLayer& layer : overlay_layers_) {
        const int pinned = AcquireOverlayForBlend(layer);
        if (pinned == kNoOverlaySlot) {
            continue;
        }
        const OverlaySlot& overlay = layer.slots[pinned];
        BlendOverlay(overlay, target, target_stride, bgr888, PanelRect());
        const int32_t top = overlay.y < 0 ? 0 : overlay.y;
        const int32_t bottom = overlay.y + static_cast<int32_t>(overlay.height) > static_cast<int32_t>(height_)
                                   ? static_cast<int32_t>(height_)
                                   : overlay.y + static_cast<int32_t>(overlay.height);
        ReleaseOverlayAfterBlend(layer);
        blended = true;
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
        // The rows touched by the CPU must reach memory before the panel DMA reads them.
        if (top < bottom) {
            (void)WriteBackForDma(target + static_cast<uint32_t>(top) * target_stride,
                                  static_cast<uint32_t>(bottom - top) * target_stride);
        }
#else
        (void)top;
        (void)bottom;
#endif
    }
    return blended;
}

bool DirectSurfacePresenter::ScanoutBlit(const device::DirectSurfacePresentation& frame, bool source_byte_swapped) {
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    const bool scale = ScaleRequested(frame);
    const bool packed = frame.pitch == width_ * kBytesPerRgb565;
    const bool order_matches = source_byte_swapped == profile_.rgb565_byte_swapped;
    // Staged paths blend the overlay into the Host-owned stage; the zero-copy
    // path blends into the Guest buffer and restores it after the transfer.
    const bool overlay = OverlayActive();
    const uint8_t* wire = nullptr;
    if (!scale && packed && order_matches) {
        // Zero-copy: the Guest buffer is already what the panel wants. The
        // overlay, when shown, is blended straight into the in-flight Guest
        // buffer so the panel still sees a single full-frame transfer; the
        // covered rows are saved first and put back once the transfer is done.
        // A second small transfer for the overlay costs ~3 ms of QSPI time on
        // a 480x480 panel, enough to drop from 42 to 37 fps.
        OverlayBackups backups{};
        if (overlay) {
            (void)BlendOverlayInPlace(frame.pixels, backups);
        }
        if (!WriteBackForDma(frame.pixels, frame.length)) {
            RestoreOverlayBackup(frame.pixels, backups);
            return false;
        }
        const esp_err_t status = esp_lv_adapter_dummy_draw_blit(display_, 0, 0, static_cast<int>(width_),
                                                                static_cast<int>(height_), frame.pixels, true);
        RestoreOverlayBackup(frame.pixels, backups);
        if (status != ESP_OK) {
            ESP_LOGW(kTag, "dummy draw blit failed: %s", esp_err_to_name(status));
            return false;
        }
        return true;
    }
    {
        if (!EnsureStage(wire_stage_)) {
            return false;
        }
        bool staged = false;
        if (blitter_.Ready() && WriteBackForDma(frame.pixels, frame.length)) {
            if (!scale && !order_matches) {
                // One PPA pass materializes the swapped byte order.
                staged = blitter_.SwapRgb565Bytes(frame.pixels, wire_stage_, width_, height_, stage_bytes_) == ESP_OK;
            } else {
                // PPA scales into canonical order; a second pass swaps when the
                // panel needs it. Unpacked (padded pitch) sources take this path too.
                uint8_t* canonical = profile_.rgb565_byte_swapped ? scale_stage_ : wire_stage_;
                if (profile_.rgb565_byte_swapped && !EnsureStage(scale_stage_)) {
                    return false;
                }
                const PpaSrmBlit request{
                    .source = frame.pixels,
                    .source_width = frame.pitch / kBytesPerRgb565,
                    .source_height = frame.source_height,
                    .source_region = {.x = 0U, .y = 0U, .width = frame.source_width, .height = frame.source_height},
                    .source_mode = PPA_SRM_COLOR_MODE_RGB565,
                    .destination = canonical,
                    .destination_width = width_,
                    .destination_height = height_,
                    .destination_allocation_bytes = stage_bytes_,
                    .destination_x = 0U,
                    .destination_y = 0U,
                    .destination_mode = PPA_SRM_COLOR_MODE_RGB565,
                    .scale_x = static_cast<float>(width_) / static_cast<float>(frame.source_width),
                    .scale_y = static_cast<float>(height_) / static_cast<float>(frame.source_height),
                    .input_byte_swap = source_byte_swapped,
                };
                staged = blitter_.Blit(request) == ESP_OK;
                if (staged && profile_.rgb565_byte_swapped) {
                    staged = blitter_.SwapRgb565Bytes(canonical, wire_stage_, width_, height_, stage_bytes_) == ESP_OK;
                }
            }
        }
        if (!staged) {
            ConvertRgb565(frame.pixels, frame.pitch, frame.source_width, frame.source_height, source_byte_swapped,
                          wire_stage_, width_ * kBytesPerRgb565, width_, height_, false, profile_.rgb565_byte_swapped);
            if (!WriteBackForDma(wire_stage_, width_ * height_ * kBytesPerRgb565)) {
                return false;
            }
        }
        if (overlay) {
            (void)BlendOverlayInto(wire_stage_, width_ * kBytesPerRgb565, false);
        }
        wire = wire_stage_;
    }
    const esp_err_t status =
        esp_lv_adapter_dummy_draw_blit(display_, 0, 0, static_cast<int>(width_), static_cast<int>(height_), wire, true);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "dummy draw blit failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
#else
    (void)frame;
    (void)source_byte_swapped;
    return false;
#endif
}

bool DirectSurfacePresenter::BlendOverlayInPlace(uint8_t* frame_pixels, OverlayBackups& backups) {
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    for (OverlayBackup& backup : backups) {
        backup = {};
    }
    if (!EnsureStage(wire_stage_)) {
        return false;
    }
    // Each layer's rectangle is saved back to back inside the stage; the stage
    // is a whole frame, so any set of on-panel rectangles fits.
    const uint32_t stride = width_ * kBytesPerRgb565;
    uint8_t* saved_cursor = wire_stage_;
    for (int layer_index = 0; layer_index < kScanoutOverlayLayers; ++layer_index) {
        OverlayLayer& layer = overlay_layers_[layer_index];
        const int pinned = AcquireOverlayForBlend(layer);
        if (pinned == kNoOverlaySlot) {
            continue;
        }
        const OverlaySlot& overlay = layer.slots[pinned];
        const int32_t top = overlay.y < 0 ? 0 : overlay.y;
        const int32_t bottom = overlay.y + static_cast<int32_t>(overlay.height) > static_cast<int32_t>(height_)
                                   ? static_cast<int32_t>(height_)
                                   : overlay.y + static_cast<int32_t>(overlay.height);
        const int32_t left = overlay.x < 0 ? 0 : overlay.x;
        const int32_t right = overlay.x + static_cast<int32_t>(overlay.width) > static_cast<int32_t>(width_)
                                  ? static_cast<int32_t>(width_)
                                  : overlay.x + static_cast<int32_t>(overlay.width);
        if (top >= bottom || left >= right) {
            ReleaseOverlayAfterBlend(layer);
            continue;
        }
        // Save just the covered rectangle, packed row by row, then blend the
        // overlay straight into the Guest buffer. RestoreOverlayBackup() puts
        // the rectangle back once the panel has read it, so the Guest never
        // sees an overlay in a returned buffer. A 200x20 HUD costs ~0.2 ms per
        // copy here; both copies sit in front of the panel DMA.
        OverlayBackup& backup = backups[layer_index];
        backup.saved = saved_cursor;
        backup.offset = static_cast<uint32_t>(top) * stride + static_cast<uint32_t>(left) * kBytesPerRgb565;
        backup.row_bytes = static_cast<uint32_t>(right - left) * kBytesPerRgb565;
        backup.rows = static_cast<uint32_t>(bottom - top);
        const uint8_t* source = frame_pixels + backup.offset;
        uint8_t* saved = backup.saved;
        for (uint32_t row = 0U; row < backup.rows; ++row, source += stride, saved += backup.row_bytes) {
            std::memcpy(saved, source, backup.row_bytes);
        }
        saved_cursor = saved;
        BlendOverlay(overlay, frame_pixels, stride, false, PanelRect());
        ReleaseOverlayAfterBlend(layer);
    }
    return true;
#else
    (void)frame_pixels;
    for (OverlayBackup& backup : backups) {
        backup = {};
    }
    return false;
#endif
}

void DirectSurfacePresenter::RestoreOverlayBackup(uint8_t* frame_pixels, const OverlayBackups& backups) {
    // The Guest reads this buffer through the same cache hierarchy, so no
    // writeback is needed here; the panel DMA has already completed. Layers
    // are restored in reverse so an overlap ends up with the original pixels.
    const uint32_t stride = width_ * kBytesPerRgb565;
    for (int layer_index = kScanoutOverlayLayers - 1; layer_index >= 0; --layer_index) {
        const OverlayBackup& backup = backups[layer_index];
        if (backup.rows == 0U || backup.saved == nullptr) {
            continue;
        }
        uint8_t* target = frame_pixels + backup.offset;
        const uint8_t* saved = backup.saved;
        for (uint32_t row = 0U; row < backup.rows; ++row, target += stride, saved += backup.row_bytes) {
            std::memcpy(target, saved, backup.row_bytes);
        }
    }
}

bool DirectSurfacePresenter::ScanoutFramebuffer(const device::DirectSurfacePresentation& frame,
                                                bool source_byte_swapped) {
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    if (framebuffers_ == nullptr) {
        return false;
    }
    uint8_t* target = framebuffers_->AcquireFree();
    if (target == nullptr) {
        return false;
    }
    // DPI framebuffers are packed RGB888 (see the system transition compositor).
    constexpr uint32_t kBytesPerRgb888 = 3U;
    const uint32_t stride = width_ * kBytesPerRgb888;
    const uint32_t frame_bytes = stride * height_;
    bool converted = false;
    if (blitter_.Ready() && WriteBackForDma(frame.pixels, frame.length)) {
        const PpaSrmBlit request{
            .source = frame.pixels,
            .source_width = frame.pitch / kBytesPerRgb565,
            .source_height = frame.source_height,
            .source_region = {.x = 0U, .y = 0U, .width = frame.source_width, .height = frame.source_height},
            .source_mode = PPA_SRM_COLOR_MODE_RGB565,
            .destination = target,
            .destination_width = width_,
            .destination_height = height_,
            .destination_allocation_bytes = frame_bytes,
            .destination_x = 0U,
            .destination_y = 0U,
            .destination_mode = PPA_SRM_COLOR_MODE_RGB888,
            .scale_x = static_cast<float>(width_) / static_cast<float>(frame.source_width),
            .scale_y = static_cast<float>(height_) / static_cast<float>(frame.source_height),
            .input_byte_swap = source_byte_swapped,
        };
        converted = blitter_.Blit(request) == ESP_OK;
    }
    if (!converted) {
        ConvertRgb565(frame.pixels, frame.pitch, frame.source_width, frame.source_height, source_byte_swapped, target,
                      stride, width_, height_, true, false);
        if (!WriteBackForDma(target, frame_bytes)) {
            return false;
        }
    }
    if (OverlayActive()) {
        (void)BlendOverlayInto(target, stride, true);
    }
    const esp_err_t status = framebuffers_->Submit(target);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "framebuffer submit failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
#else
    (void)frame;
    (void)source_byte_swapped;
    return false;
#endif
}

bool DirectSurfacePresenter::Composite(const device::DirectSurfacePresentation& frame, bool source_byte_swapped) {
    return composite_.composite != nullptr && composite_.composite(composite_.context, frame, source_byte_swapped);
}

void DirectSurfacePresenter::ReleaseFront() {
    if (!has_front_) {
        return;
    }
    has_front_ = false;
    ReleaseBuffer(front_.buffer_index);
}

void DirectSurfacePresenter::ReleaseBuffer(uint8_t buffer_index) {
    const uint8_t bit = static_cast<uint8_t>(1U << buffer_index);
    taskENTER_CRITICAL(&lock_);
    const bool owned = (owned_mask_ & bit) != 0U;
    owned_mask_ &= static_cast<uint8_t>(~bit);
    const device::DirectSurfaceReleaseSink sink = release_;
    taskEXIT_CRITICAL(&lock_);
    if (owned && sink.release != nullptr) {
        sink.release(sink.context, buffer_index, static_cast<uint64_t>(esp_timer_get_time()));
    }
}

}  // namespace micropixel::platform::lvgl
