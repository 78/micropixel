// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <optional>

#include "device/contracts/graphics.hpp"
#include "freertos/queue.h"
#include "micropixel_mutex.hpp"
#include "platform/graphics/scene_storage.hpp"
#include "platform/lvgl/fonts/bitmap_font_rasterizer.hpp"
namespace mpx_port {
namespace d = micropixel::device;
namespace g = micropixel::platform::graphics;
class Graphics final : public d::Graphics {
    BouffaloMutex mutex_;
    g::SceneStorage storage_;
    std::optional<g::GuestScene> scene_;
    std::optional<g::AppSurfaceCompositor> compositor_;
    g::SoftwarePixelCompositor pixels_;
    micropixel::platform::lvgl::FontRegistry fonts_;
    micropixel::platform::lvgl::BitmapFontRasterizer text_{fonts_};
    d::TextureAccess retained_{};
    micropixel_texture_handle_t textures_[32]{};
    size_t texture_count_{};
    micropixel_font_handle_t font_handles_[12]{};
    size_t font_count_{};
    QueueHandle_t releases_{};
    TaskHandle_t release_task_{};
    d::DirectSurfaceReleaseSink sink_{};
    d::DirectSurfaceConfig direct_{};
    bool ready_{}, suspended_{};
    void (*overlay_)(g::PixelSurface){};
    bool Present(g::PixelSurface surface);
    void ReleaseReferences();
    void Drain();
    static void ReleaseWorker(void*);

   public:
    bool Initialize(void (*overlay)(g::PixelSurface) = nullptr);
    uint32_t Width() const;
    uint32_t Height() const;
    bool Available() const override { return ready_; }
    int32_t GetInfo(micropixel_graphics_info_t&) override;
    int32_t Submit(const uint8_t*, uint32_t, const d::TextureAccess&) override;
    int32_t LoadFont(const d::FontResourceView&, micropixel_font_info_t&) override;
    int32_t ReleaseFont(micropixel_font_handle_t) override;
    int32_t MeasureText(micropixel_font_handle_t, const char*, uint32_t, micropixel_text_metrics_t&) override;
    int32_t DrawText(const d::TextTarget&, int32_t, int32_t, uint32_t, micropixel_font_handle_t, const char*,
                     uint32_t) override;
    int32_t CopyOpaqueBlocks(const d::PixelTarget&, const d::OpaqueCopyBlock*, uint32_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    int32_t ScaleBitmap(const d::BitmapView&, const d::BitmapView&) override;
    int32_t ShowLaunchBitmap(const d::BitmapView&) override;
    void DismissLaunchBitmap() override {}
    void ReleaseGuestResources() override;
    int32_t CreateDirectSurface(const d::DirectSurfaceConfig&, const d::DirectSurfaceReleaseSink&,
                                d::DirectSurfaceInfo&) override;
    int32_t PresentDirectSurface(const d::DirectSurfacePresentation&) override;
    void SuspendDirectSurface() override;
    void ResumeDirectSurface() override;
    int32_t DestroyDirectSurface() override;
    bool CaptureJpeg(uint8_t*&, size_t&);
    void ShowMessage(const char*, uint32_t color = 0x15243a);
    void SetDisplayByteSwap(bool enabled);
    void SetDisplayPanelColors(bool bgr, bool inverted);
    bool ShowColorTest();
};
}  // namespace mpx_port
