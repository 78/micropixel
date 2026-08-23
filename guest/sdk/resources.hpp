#ifndef MICROPIXEL_SDK_RESOURCES_HPP
#define MICROPIXEL_SDK_RESOURCES_HPP

#include <stdint.h>

#include "sdk/graphics.hpp"

namespace micropixel {

class Application;
class CommandBuffer;
class Event;
class OffscreenUpdateFrame;
class ResourceReadyEvent;

enum class SurfacePixelFormat : uint32_t {
    // Native LVGL little-endian storage: B, G, R.
    kRgb888 = 1U,
    // Native LVGL little-endian storage: B, G, R, A.
    kArgb8888 = 2U,
};

class AssetId final {
   public:
    explicit constexpr AssetId(uint32_t value) : value_(value) {}
    [[nodiscard]] constexpr uint32_t value() const { return value_; }

   private:
    uint32_t value_{};
};

class ResourceRef final {
   public:
    [[nodiscard]] static constexpr ResourceRef Package(AssetId asset) { return ResourceRef{asset}; }
    [[nodiscard]] constexpr AssetId asset() const { return asset_; }

   private:
    explicit constexpr ResourceRef(AssetId asset) : asset_(asset) {}
    AssetId asset_{0U};
};

class Bitmap final {
   public:
    Bitmap() = default;
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;
    Bitmap(Bitmap&& other) noexcept;
    Bitmap& operator=(Bitmap&& other) noexcept;
    ~Bitmap();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr uint32_t width() const { return width_; }
    [[nodiscard]] constexpr uint32_t height() const { return height_; }
    void Release();

   private:
    constexpr Bitmap(uint32_t handle, uint32_t width, uint32_t height)
        : handle_(handle), width_(width), height_(height) {}

    uint32_t handle_{};
    uint32_t width_{};
    uint32_t height_{};

    friend class CommandBuffer;
    friend class Graphics;
    friend class OffscreenSurface;
    friend class ResourceReadyEvent;
    friend class Resources;
};

// A Guest-controlled Bitmap whose backing pixels live in Host PSRAM. Updates
// upload only a dirty rectangle; the SDK chunks large rectangles to the ABI's
// bounded request size and the Host invalidates only visible references.
class OffscreenSurface final {
   public:
    OffscreenSurface() = default;
    OffscreenSurface(const OffscreenSurface&) = delete;
    OffscreenSurface& operator=(const OffscreenSurface&) = delete;
    OffscreenSurface(OffscreenSurface&&) noexcept = default;
    OffscreenSurface& operator=(OffscreenSurface&&) noexcept = default;

    [[nodiscard]] constexpr bool valid() const { return bitmap_.valid(); }
    [[nodiscard]] constexpr uint32_t width() const { return bitmap_.width(); }
    [[nodiscard]] constexpr uint32_t height() const { return bitmap_.height(); }
    [[nodiscard]] constexpr SurfacePixelFormat pixel_format() const { return pixel_format_; }
    [[nodiscard]] constexpr const Bitmap& bitmap() const { return bitmap_; }

    void Update(Rect dirty, const uint8_t* pixels, uint32_t stride);
    void Release() { bitmap_.Release(); }

   private:
    OffscreenSurface(Bitmap bitmap, SurfacePixelFormat pixel_format)
        : bitmap_(static_cast<Bitmap&&>(bitmap)), pixel_format_(pixel_format) {}

    Bitmap bitmap_{};
    SurfacePixelFormat pixel_format_{SurfacePixelFormat::kRgb888};
    friend class Resources;
};

// Groups multiple OffscreenSurface::Update calls into one atomic display
// transaction. The Host accumulates one damage union per backing Bitmap and
// wakes the compositor only after Commit(). Destruction commits a live frame.
class OffscreenUpdateFrame final {
   public:
    OffscreenUpdateFrame(const OffscreenUpdateFrame&) = delete;
    OffscreenUpdateFrame& operator=(const OffscreenUpdateFrame&) = delete;
    OffscreenUpdateFrame(OffscreenUpdateFrame&& other) noexcept;
    OffscreenUpdateFrame& operator=(OffscreenUpdateFrame&&) = delete;
    ~OffscreenUpdateFrame();

    void Commit();

   private:
    struct CapabilityToken {};
    explicit constexpr OffscreenUpdateFrame(CapabilityToken) : active_(true) {}

    bool active_{};

    friend class Resources;
};

class LoadRequest final {
   public:
    LoadRequest(const LoadRequest&) = delete;
    LoadRequest& operator=(const LoadRequest&) = delete;
    LoadRequest(LoadRequest&& other) noexcept;
    LoadRequest& operator=(LoadRequest&& other) noexcept;
    ~LoadRequest();

    [[nodiscard]] constexpr bool pending() const { return handle_ != 0U; }
    void Cancel();

   private:
    explicit constexpr LoadRequest(uint32_t handle) : handle_(handle) {}
    void MarkComplete() { handle_ = 0U; }
    uint32_t handle_{};

    friend class Event;
    friend class Resources;
};

class ResourceReadyEvent final {
   public:
    [[nodiscard]] constexpr bool succeeded() const { return status_ == 0; }
    [[nodiscard]] constexpr int32_t status() const { return status_; }
    [[nodiscard]] Bitmap TakeBitmap();

   private:
    constexpr ResourceReadyEvent(uint32_t request, uint32_t bitmap, int32_t status)
        : request_(request), bitmap_(bitmap), status_(status) {}

    uint32_t request_{};
    uint32_t bitmap_{};
    int32_t status_{};

    friend class Application;
    friend class Event;
};

class Resources final {
   public:
    [[nodiscard]] LoadRequest Load(ResourceRef resource) const;
    [[nodiscard]] OffscreenSurface CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                          SurfacePixelFormat pixel_format) const;
    [[nodiscard]] OffscreenUpdateFrame BeginOffscreenUpdateFrame() const;

   private:
    struct CapabilityToken {};
    explicit constexpr Resources(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
