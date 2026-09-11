#pragma once

#include <atomic>
#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "platform/lvgl/display/display_pipeline.hpp"
#include "platform/lvgl/display/frame_timing.hpp"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
#include "platform/graphics/dma2d_copy_engine.hpp"
#include "platform/lvgl/display/ppa_srm_blitter.hpp"
#endif

namespace micropixel::platform::lvgl {

// Composited fallback owned by the Guest graphics engine. `composite` converts
// one Guest frame into the App Surface and publishes it; it runs on the
// presenter task and must not take the LVGL lock unless the engine composes
// under it anyway.
struct DirectSurfaceCompositeSink final {
    void* context{};
    bool (*composite)(void* context, const device::DirectSurfacePresentation& frame, bool source_byte_swapped){};
};

// Small Host-owned ARGB8888 image (LVGL memory order B,G,R,A, straight alpha)
// that the presenter blends into every frame it scans out, e.g. the
// performance HUD. An LVGL object above the Guest frame would instead force
// the full-frame composited path.
struct ScanoutOverlayImage final {
    const uint8_t* pixels{};
    uint32_t width{};
    uint32_t height{};
    uint32_t stride{};
    int32_t x{};
    int32_t y{};
};

// Independent overlay layers. Each is set and cleared on its own; every
// active layer is blended into each scanned-out frame.
enum class ScanoutOverlayLayer : uint8_t {
    kPerformanceHud = 0,
    kGestureHint = 1,
};
inline constexpr int kScanoutOverlayLayers = 2;

// Panel-space rectangle of a App Surface frame that changed since the previous one.
struct AppSurfaceFrameRect final {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

// A Host-owned, panel-sized frame: the composited App Surface of a Scene /
// streaming-bitmap App, either canonical RGB565 (2 bytes per pixel) or the
// packed BGR888 the DPI framebuffers hold (3 bytes per pixel). Only the damage
// rectangles reach the panel; `whole` sends everything and an empty list
// without it means nothing visible changed.
struct AppSurfaceFrame final {
    static constexpr uint32_t kMaxRects = 16U;
    const uint8_t* pixels{};
    uint32_t stride{};
    uint32_t length{};
    uint8_t bytes_per_pixel{2U};
    AppSurfaceFrameRect rects[kMaxRects]{};
    uint32_t rect_count{};
    bool whole{};
};

// Owner of the App Surface frames (the Guest graphics engine). Both hooks run on the
// presenter task and must not take the LVGL lock.
struct AppSurfaceFrameSource final {
    void* context{};
    // Pops the newest published frame (`pending_only`) or returns the frame
    // the panel currently shows with whole-frame damage. False: nothing to
    // show. The returned buffer stays untouched by the owner until the next
    // acquire, which is the in-flight protection for the panel transfer.
    bool (*acquire)(void* context, bool pending_only, AppSurfaceFrame& frame_out){};
    // The frame the owner published goes through LVGL after all. With
    // `panel_stale` the presenter had been scanning frames out behind LVGL's
    // back (Host UI appeared, the App was suspended, a blit failed) and LVGL
    // has to redraw the whole frame so its state matches the panel; otherwise
    // LVGL's buffers are current and its own damage tracking suffices.
    void (*handed_to_lvgl)(void* context, bool panel_stale){};
};

// Scans Guest-rendered RGB565 frames out to the panel without going through
// LVGL. One presenter per display, created at board init and alive for the
// whole firmware lifetime; Create/Destroy bracket a single Guest surface.
//
// Threads:
//   Guest task ......... Create, Present, Destroy (through the Graphics contract)
//                        PresentAppSurfaceFrame after publishing an App Surface frame
//   Host task .......... Suspend (App pause), ScanoutArbiter yield
//   LVGL task .......... SetHostUiVisible from LV_EVENT_REFR_START, overlays
//   presenter task ..... everything that touches the panel or the App Surface
//
// While a frame is on the panel the presenter keeps that buffer ("front") and
// only returns it to the Guest when the next frame starts scanning out, so a
// composited copy of the displayed frame can always be produced when the
// panel has to be handed back to LVGL. Two Guest buffers therefore pipeline
// render and transfer without ever stalling on the panel.
//
// App Surface frames (App Surface direct scanout): on RGB565 blit panels the same
// exclusive path also takes the composited App Surface of ordinary Scene Apps.
// Only the damage rectangles are converted (byte swap) into a stage and sent
// to the panel, so a scrolling 2D game costs one window transfer per frame
// instead of LVGL's render + full-window flush. Host UI above the Guest frame
// hands the panel back to LVGL exactly like it does for a Direct Surface.
class DirectSurfacePresenter final {
   public:
    DirectSurfacePresenter() = default;
    DirectSurfacePresenter(const DirectSurfacePresenter&) = delete;
    DirectSurfacePresenter& operator=(const DirectSurfacePresenter&) = delete;
    ~DirectSurfacePresenter();

    [[nodiscard]] esp_err_t Initialize(lv_display_t* display, uint32_t width, uint32_t height,
                                       const DirectScanoutProfile& profile, DirectFramebufferAccess* framebuffers,
                                       const DirectSurfaceCompositeSink& sink);
    void Shutdown();

    [[nodiscard]] bool Ready() const { return task_ != nullptr; }
    // True when every present must land in the App Surface (no panel scanout).
    // Exclusive blit/framebuffer modes only need App Surface for Host-UI fallback.
    [[nodiscard]] bool RequiresAppSurface() const { return profile_.mode == DirectScanoutProfile::Mode::kComposited; }
    // True when frames can bypass LVGL, so Host UI drawn above the Guest frame
    // costs the exclusive path; composited-only boards keep LVGL overlays.
    [[nodiscard]] bool DirectScanoutAvailable() const {
        return profile_.mode != DirectScanoutProfile::Mode::kComposited;
    }
    void FillInfo(device::DirectSurfaceInfo& info) const;

    // LVGL task. Copies `image` (at most the display width by
    // kOverlayMaxHeight rows) into a Host-owned slot of `layer` that the
    // presenter blends into scanned-out frames until ClearOverlay(layer).
    // Returns false when the image does not fit or both slots of the layer
    // are busy; the caller simply retries on its next sample.
    [[nodiscard]] bool SetOverlay(ScanoutOverlayLayer layer, const ScanoutOverlayImage& image);
    void ClearOverlay(ScanoutOverlayLayer layer);
    static constexpr uint32_t kOverlayMaxHeight = 64U;

    [[nodiscard]] int32_t Create(const device::DirectSurfaceConfig& config,
                                 const device::DirectSurfaceReleaseSink& sink, device::DirectSurfaceInfo& info_out);
    [[nodiscard]] int32_t Present(const device::DirectSurfacePresentation& frame);
    // Drains every queued frame, leaves exclusive scanout (compositing the
    // displayed frame into the App Surface) and returns all buffers. Frames
    // presented afterwards are dropped (and released) until Resume(): the Guest
    // keeps running until its WaitEvent safe point, and a frame from that window
    // must never take the panel back from the Host UI.
    void Suspend();
    void Resume();
    [[nodiscard]] int32_t Destroy();
    [[nodiscard]] bool Created() const { return created_.load(std::memory_order_acquire); }
    [[nodiscard]] bool Exclusive() const { return exclusive_.load(std::memory_order_acquire); }
    // Frames shown since Create(), scanned out directly or composited.
    [[nodiscard]] uint32_t FramesPresented() const {
        return frames_scanned_out_.load(std::memory_order_relaxed) + frames_composited_.load(std::memory_order_relaxed);
    }
    // App Surface frame scanout cost since the last Take: copy/blit time, time spent
    // handing the frame to the panel (framebuffer flips wait for the switch)
    // and how many scanouts had to send the whole panel.
    struct AppSurfaceFrameStats final {
        uint32_t frames{};
        uint32_t whole{};
        uint64_t copy_us{};
        uint64_t flip_us{};
        uint64_t pixels{};
        // DMA2D breakdown of copy_us (framebuffer profiles with DMA2D only).
        uint32_t blocks{};
        uint64_t cache_us{};
        uint64_t wait_us{};
    };
    [[nodiscard]] AppSurfaceFrameStats TakeAppSurfaceFrameStats();

    // LVGL task. Host UI drawn above the Guest frame forces the composited
    // path so LVGL can blend it; when it disappears the presenter re-enters
    // exclusive scanout on the next frame.
    void SetHostUiVisible(bool visible);

    // ---- App Surface frames (App Surface direct scanout) ---------------------------
    //
    // Enables the path; effective on kBlitRgb565 profiles (window blits of
    // the damage) and on kFramebufferRgb888 profiles with direct framebuffer
    // access (damage copied into the free DPI framebuffer, then flipped).
    // Call once before the LVGL task starts.
    void SetAppSurfaceFrameSource(const AppSurfaceFrameSource& source);
    [[nodiscard]] bool AppSurfaceFrameScanoutEnabled() const {
        if (app_surface_frame_source_.acquire == nullptr) {
            return false;
        }
        return profile_.mode == DirectScanoutProfile::Mode::kBlitRgb565 ||
               (profile_.mode == DirectScanoutProfile::Mode::kFramebufferRgb888 && framebuffers_ != nullptr);
    }
    [[nodiscard]] bool AppSurfaceFrameFramebufferMode() const {
        return profile_.mode == DirectScanoutProfile::Mode::kFramebufferRgb888;
    }
    // Guest task, after publishing: whether the next App Surface frame should go to
    // the presenter rather than LVGL. A frame posted while the answer flips
    // to false is handed to LVGL by the presenter task itself.
    [[nodiscard]] bool AppSurfaceFrameScanoutWanted() const {
        return AppSurfaceFrameScanoutEnabled() && !created_.load(std::memory_order_acquire) &&
               !suspended_.load(std::memory_order_acquire) && !host_ui_visible_.load(std::memory_order_acquire);
    }
    // Asks the presenter task to acquire and scan out the newest App Surface frame.
    // Returns false when the request could not be queued; the caller then
    // takes the LVGL path for that frame.
    [[nodiscard]] bool PresentAppSurfaceFrame();
    // True while the panel shows App Surface frames scanned out by the presenter.
    [[nodiscard]] bool AppSurfaceFrameExclusive() const {
        return app_surface_frame_exclusive_.load(std::memory_order_acquire);
    }
    // Guest teardown: hands the panel back to LVGL if App Surface frames own it.
    // Blocks until the presenter task has left dummy draw. Not needed for a
    // Direct Surface, whose Destroy() already drains the presenter.
    void ReleaseAppSurfaceFrames();

    // Screenshot support: copies the Direct Surface frame currently on the
    // panel into `destination` (canonical RGB565, panel size, nearest
    // upscale when the surface is smaller), run on the presenter task so the
    // front buffer cannot be released or replaced mid-copy. False when the
    // presenter is not scanning a Direct Surface out, or the copy timed out.
    [[nodiscard]] bool CaptureFront(uint8_t* destination, uint32_t destination_stride, uint32_t destination_width,
                                    uint32_t destination_height);

   private:
    enum class JobKind : uint8_t { kPresent, kYield, kBarrier, kAppSurfaceFrame, kOverlayRefresh, kCaptureFront };
    struct FrontCapture final {
        uint8_t* destination{};
        uint32_t stride{};
        uint32_t width{};
        uint32_t height{};
        bool captured{};
    };
    struct Job final {
        JobKind kind{};
        device::DirectSurfacePresentation frame{};
        SemaphoreHandle_t done{};
        FrontCapture* capture{};
    };
    static constexpr UBaseType_t kQueueDepth = 8U;
    static constexpr uint32_t kTaskStackBytes = 6 * 1024U;
    static constexpr TickType_t kControlTimeout = pdMS_TO_TICKS(500U);

    static void TaskEntry(void* context);
    static void YieldHook(void* context);
    void Run();
    void HandlePresent(const Job& job);
    void HandleAppSurfaceFrame();
    void HandleOverlayRefresh();
    // Takes the panel for App Surface frames; false when a Direct Surface holds it
    // or the arbiter refused.
    [[nodiscard]] bool EnterAppSurfaceFrameExclusive();
    // Presenter task: converts each rectangle of `frame` into the wire stage
    // (byte order, overlays) and sends it to the panel. `whole_frame` ignores
    // the damage list.
    [[nodiscard]] bool ScanoutAppSurfaceFrame(const AppSurfaceFrame& frame, bool whole_frame);
    [[nodiscard]] bool ScanoutAppSurfaceFrameRect(const AppSurfaceFrame& frame, const AppSurfaceFrameRect& rect);
    // kFramebufferRgb888: copies the damage into the free DPI framebuffer and
    // flips. The free buffer lags one flip behind, so the previous scanout's
    // damage and the overlay footprints it still carries are repainted too.
    [[nodiscard]] bool ScanoutAppSurfaceFrameToFramebuffer(const AppSurfaceFrame& frame, bool whole_frame);
    struct OverlayFootprint final {
        AppSurfaceFrameRect rect{};
        bool valid{};
    };
    struct FramebufferRectList final {
        // Bounded by one DMA2D transaction (Dma2dCopyEngine::kMaxBlocks).
        static constexpr uint32_t kCapacity = 32U;
        AppSurfaceFrameRect rects[kCapacity]{};
        uint32_t count{};
        bool whole{};
        void Add(const AppSurfaceFrameRect& rect);
    };
    // Damage sent by the last two framebuffer scanouts (index 0 = newest).
    struct FramebufferHistory final {
        AppSurfaceFrameRect damage[AppSurfaceFrame::kMaxRects]{};
        uint32_t damage_count{};
        bool whole{};
        OverlayFootprint footprints[kScanoutOverlayLayers]{};
    };
    // Expands to the controller window granularity and clips to the panel.
    [[nodiscard]] AppSurfaceFrameRect AlignRect(const AppSurfaceFrameRect& rect) const;
    // LVGL task: an overlay changed while App Surface frames are on the panel, so the
    // rows under it have to be sent again even without Guest damage.
    void RequestOverlayRefresh();
    bool PostControl(JobKind kind, FrontCapture* capture = nullptr);
    void HandleCaptureFront(FrontCapture& capture) const;
    [[nodiscard]] bool EnterExclusive();
    void LeaveExclusive(bool composite_front);
    [[nodiscard]] bool ScanoutBlit(const device::DirectSurfacePresentation& frame, bool source_byte_swapped);
    [[nodiscard]] bool ScanoutFramebuffer(const device::DirectSurfacePresentation& frame, bool source_byte_swapped);
    [[nodiscard]] bool Composite(const device::DirectSurfacePresentation& frame, bool source_byte_swapped);
    void ReleaseFront();
    void ReleaseBuffer(uint8_t buffer_index);
    void ReleaseStages();
    void ReleaseScanoutStages();
    void ReleaseStage(uint8_t*& stage);
    [[nodiscard]] bool EnsureStage(uint8_t*& stage);

    // Overlay slots: per layer, the LVGL task fills the slot that is neither
    // published nor being read, then publishes it; the presenter pins the
    // published slot for the duration of one blend. Storage is allocated on
    // first use and grows to the largest image a slot has held.
    struct OverlaySlot final {
        uint8_t* pixels{};          // ARGB8888 as delivered, in PSRAM
        uint32_t pixel_capacity{};  // bytes `pixels` can hold
        // RGB565 panels: the same image pre-packed into panel byte order plus
        // a separate alpha plane. The blend that runs in front of the panel DMA
        // reads these (3 B/px, no conversion) instead of the ARGB copy. They stay
        // in PSRAM: on the S31, core 0 reading internal SRAM while core 1 runs
        // the Guest measured slower than PSRAM for this copy-shaped access.
        uint16_t* wire{};
        uint8_t* alpha{};
        uint32_t prepared_capacity{};  // pixels wire/alpha can hold
        uint32_t width{};
        uint32_t height{};
        uint32_t stride{};
        int32_t x{};
        int32_t y{};
    };
    static constexpr int kOverlaySlots = 2;
    static constexpr int kNoOverlaySlot = -1;
    struct OverlayLayer final {
        OverlaySlot slots[kOverlaySlots]{};
        int published{kNoOverlaySlot};
        int pinned{kNoOverlaySlot};
    };
    [[nodiscard]] static bool EnsureOverlaySlot(OverlaySlot& slot, uint32_t bytes);
    [[nodiscard]] bool PrepareOverlaySlot(OverlaySlot& slot, const ScanoutOverlayImage& image);
    [[nodiscard]] int AcquireOverlayForBlend(OverlayLayer& layer);
    void ReleaseOverlayAfterBlend(OverlayLayer& layer);
    [[nodiscard]] bool OverlayActive() const { return overlay_active_mask_.load(std::memory_order_acquire) != 0U; }
    // Blends one pinned overlay into `target`, which holds the panel
    // rectangle `clip` (its first pixel is clip.x/clip.y). `bgr888` selects
    // the DPI framebuffer layout, otherwise RGB565 in panel byte order.
    void BlendOverlay(const OverlaySlot& overlay, uint8_t* target, uint32_t target_stride, bool bgr888,
                      const AppSurfaceFrameRect& clip);
    [[nodiscard]] AppSurfaceFrameRect PanelRect() const {
        return {.x = 0U, .y = 0U, .width = width_, .height = height_};
    }
    // Blends every active layer into a Host-owned target.
    [[nodiscard]] bool BlendOverlayInto(uint8_t* target, uint32_t target_stride, bool bgr888);
    // Zero-copy RGB565 scanout with overlays: the rectangle under each active
    // layer is saved into the stage and the overlay is blended into the
    // in-flight Guest buffer itself, so the panel still gets one full-frame
    // transfer. RestoreOverlayBackup() puts the Guest's pixels back after the
    // transfer completes and before the buffer is released.
    struct OverlayBackup final {
        uint8_t* saved{};      // packed rows inside the stage
        uint32_t offset{};     // byte offset of the rectangle's first pixel in the frame
        uint32_t row_bytes{};  // bytes per saved row
        uint32_t rows{};       // 0 when nothing was saved
    };
    using OverlayBackups = OverlayBackup[kScanoutOverlayLayers];
    [[nodiscard]] bool BlendOverlayInPlace(uint8_t* frame_pixels, OverlayBackups& backups);
    void RestoreOverlayBackup(uint8_t* frame_pixels, const OverlayBackups& backups);

    lv_display_t* display_{};
    uint32_t width_{};
    uint32_t height_{};
    DirectScanoutProfile profile_{};
    DirectFramebufferAccess* framebuffers_{};
    DirectSurfaceCompositeSink composite_{};
    TaskHandle_t task_{};
    StaticQueue_t queue_storage_{};
    uint8_t queue_buffer_[kQueueDepth * sizeof(Job)]{};
    QueueHandle_t queue_{};
    StaticSemaphore_t control_done_storage_{};
    SemaphoreHandle_t control_done_{};
    StaticSemaphore_t control_mutex_storage_{};
    SemaphoreHandle_t control_mutex_{};
    StaticSemaphore_t stopped_storage_{};
    SemaphoreHandle_t stopped_{};
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;

    // Surface state (Guest task writes under lock_, presenter reads).
    device::DirectSurfaceConfig config_{};
    device::DirectSurfaceReleaseSink release_{};
    uint8_t owned_mask_{};  // buffers queued, in flight or held as front
    std::atomic<bool> created_{false};
    std::atomic<bool> exclusive_{false};
    std::atomic<bool> host_ui_visible_{true};
    std::atomic<bool> stop_requested_{false};
    // Set by Suspend: the next frame goes through LVGL so its refresh
    // re-evaluates what the Host UI shows before exclusive scanout resumes.
    std::atomic<bool> composite_next_{false};
    // Set by Suspend, cleared by Resume: presents are released unshown.
    std::atomic<bool> suspended_{false};

    // Presenter-task-only state.
    bool has_front_{};
    bool front_byte_swapped_{};
    device::DirectSurfacePresentation front_{};
    uint8_t* wire_stage_{};
    uint8_t* scale_stage_{};
    uint32_t stage_bytes_{};
    // Written on the presenter task, read by the LVGL task for telemetry.
    std::atomic<uint32_t> frames_scanned_out_{};
    std::atomic<uint32_t> frames_composited_{};
    void RecordCompletedScanout();
#if CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG
    FrameTiming frame_timing_{};
#endif

    // Overlay state; published and pinned slot indices change under lock_.
    OverlayLayer overlay_layers_[kScanoutOverlayLayers]{};
    std::atomic<uint8_t> overlay_active_mask_{0U};  // bit per ScanoutOverlayLayer

    // App Surface frame state. `app_surface_frame_exclusive_` is written on the presenter
    // task and read by the LVGL/Host tasks; the rest is presenter-task-only.
    AppSurfaceFrameSource app_surface_frame_source_{};
    std::atomic<bool> app_surface_frame_exclusive_{false};
    std::atomic<bool> overlay_refresh_queued_{false};
    std::atomic<bool> app_surface_frame_queued_{false};
    // The first frame after taking the panel is sent whole: LVGL's last flush
    // may predate the damage the mailbox accumulated.
    bool app_surface_frame_whole_next_{};
    // Panel rectangle each overlay layer last covered in a App Surface frame blit, so
    // a moved or cleared overlay can be painted over with frame pixels.
    OverlayFootprint overlay_footprints_[kScanoutOverlayLayers]{};
    // kFramebufferRgb888 App Surface frames: history[0] is the frame the panel shows,
    // history[1] the one in the framebuffer about to be reused. Both are sent
    // whole for the first two flips after taking the panel (LVGL's contents).
    FramebufferHistory framebuffer_history_[2]{};
    uint8_t framebuffer_whole_pending_{};
    AppSurfaceFrameStats app_surface_frame_stats_{};  // guarded by lock_
#if defined(CONFIG_SOC_PPA_SUPPORTED) && CONFIG_SOC_PPA_SUPPORTED
    PpaSrmBlitter blitter_{};
    // Presenter-task-only DMA2D engine for App Surface -> DPI framebuffer
    // copies; the compositor's engine belongs to the Guest task.
    graphics::Dma2dCopyEngine framebuffer_copy_{};
#endif
};

}  // namespace micropixel::platform::lvgl
