#include <cstring>

#include "device/graphics.hpp"
#include "device/input.hpp"
#include "platform/audio_backend.hpp"
#include "platform/configured_backends.hpp"
#include "platform/platform.hpp"

namespace micropixel::platform {
namespace {

class NullGraphicsBackend final : public device::GraphicsBackend {
   public:
    [[nodiscard]] bool Available() const override { return false; }

    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t& info) override {
        info = {};
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t BeginFrame() override {
        if (graphics_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        graphics_frame_active_ = true;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t Submit(const uint8_t* bytes, uint32_t length, device::BitmapResolver resolver,
                                 void* resolver_context) override {
        (void)bytes;
        (void)length;
        (void)resolver;
        (void)resolver_context;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t CommitFrame(device::BitmapResolver resolver, void* resolver_context) override {
        (void)resolver;
        (void)resolver_context;
        if (!graphics_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        graphics_frame_active_ = false;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    [[nodiscard]] int32_t BeginBitmapUpdateFrame() override {
        if (bitmap_update_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        bitmap_update_frame_active_ = true;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t UpdateBitmap(const device::BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                       uint32_t height, const uint8_t* pixels, uint32_t stride) override {
        const uint32_t bytes_per_pixel = bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB888
                                             ? 3U
                                             : (bitmap.pixel_format == MICROPIXEL_PIXEL_FORMAT_ARGB8888 ? 4U : 0U);
        if (bitmap.data == nullptr || pixels == nullptr || width == 0U || height == 0U || bytes_per_pixel == 0U ||
            (bitmap.flags & MICROPIXEL_BITMAP_FLAG_MUTABLE) == 0U || x + width > bitmap.width ||
            y + height > bitmap.height || stride != width * bytes_per_pixel) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        auto* destination = const_cast<uint8_t*>(bitmap.data) + y * bitmap.stride + x * bytes_per_pixel;
        for (uint32_t row = 0U; row < height; ++row) {
            std::memcpy(destination + row * bitmap.stride, pixels + row * stride, stride);
        }
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t CommitBitmapUpdateFrame() override {
        if (!bitmap_update_frame_active_) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        bitmap_update_frame_active_ = false;
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t ShowLaunchBitmap(const device::BitmapView& bitmap) override {
        (void)bitmap;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    void DismissLaunchBitmap() override {}
    void ReleaseGuestResources() override {
        bitmap_update_frame_active_ = false;
        graphics_frame_active_ = false;
    }

   private:
    bool bitmap_update_frame_active_{};
    bool graphics_frame_active_{};
};

class NullInputBackend final : public device::InputBackend {
   public:
    [[nodiscard]] int32_t GetInfo(micropixel_input_info_t& info) override {
        info = {};
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    void BindTouchSink(device::TouchSink sink, void* context) override {
        (void)sink;
        (void)context;
    }

    void UnbindTouchSink(void* context) override { (void)context; }
};

class NullPlatform final : public Platform {
   public:
    [[nodiscard]] esp_err_t Initialize() override { return ESP_OK; }
    [[nodiscard]] device::GraphicsBackend& graphics() override { return graphics_; }
    [[nodiscard]] device::InputBackend& input() override { return input_; }
    [[nodiscard]] device::AudioBackend& audio() override { return ConfiguredAudioBackend(); }
    [[nodiscard]] device::RandomBackend& random() override { return ConfiguredRandomBackend(); }

   private:
    NullGraphicsBackend graphics_{};
    NullInputBackend input_{};
};

}  // namespace

Platform& ConfiguredPlatform() {
    static NullPlatform platform;
    return platform;
}

}  // namespace micropixel::platform
