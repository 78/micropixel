#include "sdk/graphics.hpp"

#include "runtime/display_context.hpp"
#include "runtime/service_binding.hpp"
#include "runtime/direct_surface_state.hpp"
#include "runtime/texture_state.hpp"
#include "sdk/input.hpp"
#include "sdk/resources.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::GraphicsService;
using micropixel::runtime::LoadDisplayContext;
using micropixel::runtime::LoadInputInfo;
using micropixel::runtime::LoadPhysicalGraphicsInfo;
using micropixel::runtime::OpenService;
using micropixel::runtime::RequireOk;
using micropixel::runtime::ScaleCoordinate;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache resource_service;
int32_t OpenResourceService() {
    const int32_t status = OpenService(resource_service, MICROPIXEL_SERVICE_RESOURCE,
                                       MICROPIXEL_RESOURCE_INTERFACE_MAJOR, MICROPIXEL_RESOURCE_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return status;
    }
    return resource_service.info.interface_minor >= MICROPIXEL_RESOURCE_INTERFACE_MINOR
               ? MICROPIXEL_STATUS_OK
               : MICROPIXEL_STATUS_VERSION_MISMATCH;
}

}  // namespace

namespace micropixel {

RendererInfo Renderer::info() const {
    const micropixel_graphics_info_t& raw = LoadPhysicalGraphicsInfo();
    const micropixel::detail::DisplayTransform& display = LoadDisplayContext();
    const micropixel::detail::LogicalInsets safe = micropixel::detail::MapPhysicalInsets(
        display, raw.safe_inset_top, raw.safe_inset_right, raw.safe_inset_bottom, raw.safe_inset_left);
    if (safe.left + safe.right >= display.logical_width || safe.top + safe.bottom >= display.logical_height) {
        runtime::Panic("graphics.info.safe_area", MICROPIXEL_STATUS_UNSUPPORTED);
    }
    return RendererInfo{display.logical_width,
                        display.logical_height,
                        display.physical_width,
                        display.physical_height,
                        {safe.top, safe.right, safe.bottom, safe.left},
                        (raw.native_flags & MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT) != 0U,
                        (raw.native_flags & MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED) != 0U,
                        raw.max_full_frame_fps,
                        (GraphicsService().info.capabilities & MICROPIXEL_GRAPHICS_CAP_RASTER) != 0U,
                        (GraphicsService().info.capabilities & MICROPIXEL_GRAPHICS_CAP_RASTER_POLYGON) != 0U};
}

namespace {

Result<TextMetrics> MeasureTextWithHandle(const char* text, uint32_t font_handle) {
    if (text == nullptr || font_handle == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    const uint32_t max_text_bytes = runtime::LoadGraphicsLimits().max_text_bytes;
    uint32_t text_length = 0U;
    while (text_length <= max_text_bytes && text[text_length] != '\0') {
        ++text_length;
    }
    if (text_length == 0U || text_length > max_text_bytes) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }

    int32_t status = OpenService(GraphicsService(), MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                                 MICROPIXEL_GRAPHICS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    // Single-threaded Guest: one static staging buffer instead of 1 KiB of stack.
    alignas(
        4) static uint8_t request[sizeof(micropixel_graphics_measure_text_request_t) + runtime::limits::kMaxTextBytes];
    const uint32_t request_size = sizeof(micropixel_graphics_measure_text_request_t) + text_length;
    micropixel_graphics_measure_text_request_t header{};
    header.size = static_cast<uint16_t>(request_size);
    header.font_handle = font_handle;
    header.text_length = static_cast<uint16_t>(text_length);
    CopyBytes(request, &header, sizeof(header));
    CopyBytes(request + sizeof(header), text, text_length);

    micropixel_text_metrics_t response{};
    uint32_t response_size = 0U;
    status = CallService(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_MEASURE_TEXT, request, request_size, &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.reserved0 != 0U) {
        runtime::Panic("graphics.measure_text.response", MICROPIXEL_STATUS_INTERNAL);
    }
    const auto& display = LoadDisplayContext();
    return TextMetrics{
        static_cast<uint32_t>(ScaleCoordinate(response.width, display.logical_width, display.physical_width)),
        static_cast<uint32_t>(ScaleCoordinate(response.height, display.logical_height, display.physical_height)),
        ScaleCoordinate(response.baseline, display.logical_height, display.physical_height)};
}

}  // namespace

Result<TextMetrics> Renderer::MeasureText(const char* text, SystemFont font) const {
    const uint32_t font_handle = static_cast<uint32_t>(font);
    if (font_handle < MICROPIXEL_SYSTEM_FONT_SMALL || font_handle > MICROPIXEL_SYSTEM_FONT_TITLE) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    return MeasureTextWithHandle(text, font_handle);
}

Result<TextMetrics> Renderer::MeasureText(const char* text, const Font& font) const {
    if (!font.valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    return MeasureTextWithHandle(text, font.handle_);
}
InputInfo Input::info() const {
    const micropixel_input_info_t& raw = LoadInputInfo();
    return InputInfo{raw.max_touch_points, raw.capabilities};
}

Texture::Texture(Texture&& other) noexcept
    : handle_(other.handle_),
      width_(other.width_),
      height_(other.height_),
      physical_width_(other.physical_width_),
      physical_height_(other.physical_height_),
      adaptive_(other.adaptive_) {
    other.handle_ = 0U;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        width_ = other.width_;
        height_ = other.height_;
        physical_width_ = other.physical_width_;
        physical_height_ = other.physical_height_;
        adaptive_ = other.adaptive_;
        other.handle_ = 0U;
    }
    return *this;
}

Texture::~Texture() { Reset(); }

void Texture::Reset() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U,
                                            runtime::TextureSnapshot(handle_)};
        if (OpenResourceService() == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_UNLOAD, &request, sizeof(request));
        }
        runtime::ForgetDynamicTexture(handle_);
        handle_ = 0U;
        width_ = 0U;
        height_ = 0U;
        physical_width_ = 0U;
        physical_height_ = 0U;
        adaptive_ = false;
    }
}

Font::Font(Font&& other) noexcept
    : handle_(other.handle_),
      size_(other.size_),
      line_height_(other.line_height_),
      ascent_(other.ascent_),
      descent_(other.descent_) {
    other.handle_ = 0U;
}

Font& Font::operator=(Font&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        size_ = other.size_;
        line_height_ = other.line_height_;
        ascent_ = other.ascent_;
        descent_ = other.descent_;
        other.handle_ = 0U;
    }
    return *this;
}

Font::~Font() { Reset(); }

void Font::Reset() {
    if (handle_ != 0U) {
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        if (OpenResourceService() == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_FONT_UNLOAD, &request, sizeof(request));
        }
        handle_ = 0U;
        size_ = 0U;
        line_height_ = 0U;
        ascent_ = 0;
        descent_ = 0;
    }
}

namespace {
uint32_t TexturePixelBytes(uint32_t format) {
    switch (format) {
        case MICROPIXEL_PIXEL_FORMAT_RGB565:
            return 2;
        case MICROPIXEL_PIXEL_FORMAT_BGR888:
            return 3;
        case MICROPIXEL_PIXEL_FORMAT_BGRA8888:
            return 4;
        default:
            return 0;
    }
}
bool ValidTexturePixels(uint32_t width, uint32_t height, uint32_t format, std::span<const uint8_t> pixels,
                        uint32_t pitch) {
    const uint32_t bytes = TexturePixelBytes(format);
    const uint64_t row = static_cast<uint64_t>(width) * bytes;
    return width && height && bytes && pixels.data() && pixels.size() <= UINT32_MAX && pitch >= row &&
           static_cast<uint64_t>(height - 1) * pitch + row <= pixels.size();
}
void ReleaseTextureSnapshot(uint32_t handle) {
    micropixel_handle_request_t request{sizeof(request), 0, handle};
    (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_UNLOAD, &request, sizeof(request));
}
}  // namespace

Result<Texture> Resources::CreateDynamicTexture(Size size, PixelFormat format, std::span<const uint8_t> pixels,
                                                uint32_t pitch) const {
    const uint32_t wire_format = static_cast<uint32_t>(format);
    const bool empty = pixels.empty() && pitch == 0;
    if (!size.width || !size.height || size.width > UINT16_MAX || size.height > UINT16_MAX ||
        !TexturePixelBytes(wire_format) ||
        static_cast<uint64_t>(size.width) * TexturePixelBytes(wire_format) > UINT16_MAX ||
        (!empty && !ValidTexturePixels(size.width, size.height, wire_format, pixels, pitch)))
        return unexpected(Error{ErrorCode::kInvalidArgument});
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    micropixel_dynamic_texture_create_request_t request{};
    request.size = sizeof(request);
    request.width = size.width;
    request.height = size.height;
    request.pixel_format = wire_format;
    request.pixels = empty ? 0 : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pixels.data()));
    request.length = empty ? 0 : static_cast<uint32_t>(pixels.size());
    request.pitch = pitch;
    micropixel_texture_info_t response{};
    uint32_t response_size = 0;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE, &request, sizeof(request),
                         &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size != sizeof(response) || response.size != sizeof(response) || !response.texture_handle ||
        response.width != size.width || response.height != size.height || response.physical_width != size.width ||
        response.physical_height != size.height || response.pixel_format != wire_format ||
        response.flags != MICROPIXEL_TEXTURE_FLAG_DYNAMIC)
        return unexpected(Error{ErrorCode::kInternal});
    if (!runtime::RegisterDynamicTexture(response.texture_handle, wire_format)) {
        ReleaseTextureSnapshot(response.texture_handle);
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }
    return Texture{response.texture_handle, size.width, size.height, size.width, size.height, false};
}

Result<void> Texture::Update(Rect dirty, std::span<const uint8_t> pixels, uint32_t pitch) {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    auto* dynamic = runtime::FindDynamicTexture(handle_);
    if (!dynamic) return unexpected(Error{ErrorCode::kUnsupported});
    if (dirty.x < 0 || dirty.y < 0 || dirty.width <= 0 || dirty.height <= 0 ||
        static_cast<uint64_t>(dirty.x) + dirty.width > width_ ||
        static_cast<uint64_t>(dirty.y) + dirty.height > height_ ||
        !ValidTexturePixels(dirty.width, dirty.height, dynamic->format, pixels, pitch))
        return unexpected(Error{ErrorCode::kInvalidArgument});
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    micropixel_dynamic_texture_update_request_t request{};
    request.size = sizeof(request);
    request.texture_handle = dynamic->snapshot;
    request.x = dirty.x;
    request.y = dirty.y;
    request.width = dirty.width;
    request.height = dirty.height;
    request.pixels = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pixels.data()));
    request.length = static_cast<uint32_t>(pixels.size());
    request.pitch = pitch;
    micropixel_texture_info_t response{};
    uint32_t response_size = 0;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_UPDATE, &request, sizeof(request),
                         &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size != sizeof(response) || response.size != sizeof(response) || !response.texture_handle ||
        response.texture_handle == dynamic->snapshot || response.width != width_ || response.height != height_ ||
        response.pixel_format != dynamic->format || response.flags != MICROPIXEL_TEXTURE_FLAG_DYNAMIC)
        return unexpected(Error{ErrorCode::kInternal});
    const uint32_t old = dynamic->snapshot;
    dynamic->snapshot = response.texture_handle;
    ++runtime::texture_revision;
    ReleaseTextureSnapshot(old);
    return {};
}

Result<Texture> Resources::LoadTexture(AssetId asset, TextureScale scale) const {
    if (scale != TextureScale::kNative && scale != TextureScale::kDisplay && scale != TextureScale::kSurface) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    const uint32_t surface_upscale = scale == TextureScale::kSurface ? runtime::ActiveSurfaceUpscale() : 1U;
    if (surface_upscale == 0U) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    const bool display_scaled = scale != TextureScale::kNative;
    micropixel_texture_load_request_t request{};
    request.size = sizeof(request);
    request.asset_id = asset.value();
    request.scale_numerator = 1U;
    request.scale_denominator = 1U;
    if (display_scaled) {
        const micropixel::detail::DisplayTransform& display = LoadDisplayContext();
        request.scale_numerator = display.scale_numerator;
        request.scale_denominator = display.scale_denominator * surface_upscale;
    }
    micropixel_texture_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_LOAD, &request, sizeof(request),
                         &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.texture_handle == 0U ||
        response.width == 0U || response.height == 0U || response.physical_width == 0U ||
        response.physical_height == 0U ||
        (response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
         response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888 &&
         response.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) ||
        response.flags != 0U) {
        runtime::Panic("resources.load_texture.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return Texture{response.texture_handle, response.width,           response.height,
                   response.physical_width, response.physical_height, display_scaled};
}

Result<Font> Resources::LoadFont(AssetId asset) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_font_load_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, asset.value()};
    micropixel_font_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_FONT_LOAD, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.font_handle == 0U ||
        response.font_size == 0U || response.line_height == 0U || response.ascent <= 0 || response.descent < 0) {
        runtime::Panic("resources.load_font.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return Font{response.font_handle, response.font_size, response.line_height, response.ascent, response.descent};
}

}  // namespace micropixel
