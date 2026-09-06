#include "sdk/graphics.hpp"

#include "runtime/display_context.hpp"
#include "runtime/service_binding.hpp"
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
                        raw.max_scene_nodes,
                        raw.max_batch_instances,
                        raw.max_containers,
                        raw.max_sprite_batches,
                        raw.max_scene_bytes,
                        (raw.native_flags & MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT) != 0U,
                        (raw.native_flags & MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED) != 0U,
                        raw.max_full_frame_fps,
                        raw.raster_pool_bytes};
}

namespace {

Result<TextMetrics> MeasureTextWithHandle(const char* text, uint16_t font_handle) {
    if (text == nullptr || font_handle == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    uint32_t text_length = 0U;
    while (text_length <= MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES && text[text_length] != '\0') {
        ++text_length;
    }
    if (text_length == 0U || text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }

    int32_t status = OpenService(GraphicsService(), MICROPIXEL_SERVICE_GRAPHICS, MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
                                 MICROPIXEL_GRAPHICS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    alignas(4)
        uint8_t request[sizeof(micropixel_graphics_measure_text_request_t) + MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES]{};
    const uint32_t request_size = sizeof(micropixel_graphics_measure_text_request_t) + text_length;
    micropixel_graphics_measure_text_request_t header{};
    header.size = static_cast<uint16_t>(request_size);
    header.font = font_handle;
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
    const uint16_t font_handle = static_cast<uint16_t>(font);
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
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        if (OpenResourceService() == MICROPIXEL_STATUS_OK) {
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_RELEASE, &request, sizeof(request));
        }
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
            (void)CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_FONT_RELEASE, &request, sizeof(request));
        }
        handle_ = 0U;
        size_ = 0U;
        line_height_ = 0U;
        ascent_ = 0;
        descent_ = 0;
    }
}

Result<void> StreamingTexture::Update(Rect dirty, const uint8_t* pixels, uint32_t byte_length, uint32_t pitch) {
    const uint32_t bytes_per_pixel =
        pixel_format_ == PixelFormat::kBgr888
            ? 3U
            : (pixel_format_ == PixelFormat::kBgra8888 ? 4U : (pixel_format_ == PixelFormat::kRgb565 ? 2U : 0U));
    if (!valid() || dirty.x < 0 || dirty.y < 0 || dirty.width <= 0 || dirty.height <= 0 || pixels == nullptr ||
        bytes_per_pixel == 0U || static_cast<int64_t>(dirty.x) + dirty.width > static_cast<int64_t>(width()) ||
        static_cast<int64_t>(dirty.y) + dirty.height > static_cast<int64_t>(height())) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    const uint32_t row_bytes = static_cast<uint32_t>(dirty.width) * bytes_per_pixel;
    const uint64_t required_bytes = static_cast<uint64_t>(dirty.height - 1) * pitch + row_bytes;
    if (pitch < row_bytes || required_bytes > byte_length) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    constexpr uint32_t kHeaderBytes = sizeof(micropixel_streaming_texture_update_request_t);
    static_assert(kHeaderBytes < MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES,
                  "streaming texture update header exceeds ABI request");
    const uint32_t rows_per_request = (MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES - kHeaderBytes) / row_bytes;
    if (rows_per_request == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }

    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    alignas(4) uint8_t request[MICROPIXEL_STREAMING_TEXTURE_MAX_UPDATE_BYTES]{};
    uint32_t row = 0U;
    while (row < static_cast<uint32_t>(dirty.height)) {
        uint32_t row_count = static_cast<uint32_t>(dirty.height) - row;
        if (row_count > rows_per_request) {
            row_count = rows_per_request;
        }
        const uint32_t pixel_bytes = row_count * row_bytes;
        const uint32_t request_size = kHeaderBytes + pixel_bytes;
        micropixel_streaming_texture_update_request_t header{};
        header.size = static_cast<uint16_t>(request_size);
        header.texture = texture_.handle_;
        header.x = static_cast<uint32_t>(dirty.x);
        header.y = static_cast<uint32_t>(dirty.y) + row;
        header.width = static_cast<uint32_t>(dirty.width);
        header.height = row_count;
        header.pitch = row_bytes;
        CopyBytes(request, &header, sizeof(header));
        for (uint32_t source_row = 0U; source_row < row_count; ++source_row) {
            CopyBytes(request + kHeaderBytes + source_row * row_bytes, pixels + (row + source_row) * pitch, row_bytes);
        }
        status = CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_UPDATE, request, request_size);
        if (status != MICROPIXEL_STATUS_OK) {
            return unexpected(ErrorFromStatus(status));
        }
        row += row_count;
    }
    return {};
}

TextureUpdateBatch::TextureUpdateBatch(TextureUpdateBatch&& other) noexcept : active_(other.active_) {
    other.active_ = false;
}

TextureUpdateBatch::~TextureUpdateBatch() {
    if (active_) {
        (void)Finish();
    }
}

Result<void> TextureUpdateBatch::Finish() {
    if (!active_) {
        return {};
    }
    active_ = false;
    int32_t status = OpenResourceService();
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_FINISH, nullptr, 0U);
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<Texture> Resources::LoadTexture(AssetId asset) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    const micropixel::detail::DisplayTransform& display = LoadDisplayContext();
    micropixel_resource_load_adaptive_texture_request_t request{};
    request.size = sizeof(request);
    request.asset_id = asset.value();
    request.scale_numerator = display.scale_numerator;
    request.scale_denominator = display.scale_denominator;
    micropixel_adaptive_texture_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_LOAD_ADAPTIVE_TEXTURE, &request, sizeof(request),
                         &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_RESOURCE_INTERFACE_MAJOR || response.texture == 0U ||
        response.logical_width == 0U || response.logical_height == 0U || response.physical_width == 0U ||
        response.physical_height == 0U ||
        (response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
         response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888 &&
         response.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) ||
        (response.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) != 0U) {
        runtime::Panic("resources.load_texture.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return Texture{response.texture,        response.logical_width,   response.logical_height,
                   response.physical_width, response.physical_height, true};
}

Result<Texture> Resources::LoadNativeTexture(AssetId asset) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_resource_load_texture_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, asset.value()};
    micropixel_texture_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_LOAD_TEXTURE, &request, sizeof(request),
                         &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_RESOURCE_INTERFACE_MAJOR || response.texture == 0U ||
        response.width == 0U || response.height == 0U ||
        (response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 &&
         response.pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888 &&
         response.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) ||
        (response.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) != 0U) {
        runtime::Panic("resources.load_native_texture.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return Texture{response.texture, response.width, response.height, response.width, response.height, false};
}

Result<Font> Resources::LoadFont(AssetId asset) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    micropixel_resource_load_font_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, asset.value()};
    micropixel_font_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_LOAD_FONT, &request, sizeof(request), &response,
                         sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_RESOURCE_INTERFACE_MAJOR || response.font == 0U ||
        response.font_size == 0U || response.line_height == 0U || response.ascent <= 0 || response.descent < 0 ||
        response.reserved[0] != 0U || response.reserved[1] != 0U) {
        runtime::Panic("resources.load_font.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return Font{response.font, response.font_size, response.line_height, response.ascent, response.descent};
}

Result<StreamingTexture> Renderer::CreateStreamingTexture(Size size, PixelFormat pixel_format) const {
    int32_t status = OpenResourceService();
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (size.width == 0U || size.height == 0U ||
        (pixel_format != PixelFormat::kBgr888 && pixel_format != PixelFormat::kBgra8888 &&
         pixel_format != PixelFormat::kRgb565)) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_streaming_texture_create_request_t request{};
    request.size = sizeof(request);
    request.width = size.width;
    request.height = size.height;
    request.pixel_format = static_cast<uint32_t>(pixel_format);
    micropixel_texture_info_t response{};
    uint32_t response_size = 0U;
    status = CallService(resource_service, MICROPIXEL_RESOURCE_METHOD_STREAMING_TEXTURE_CREATE, &request,
                         sizeof(request), &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) ||
        response.interface_major != MICROPIXEL_RESOURCE_INTERFACE_MAJOR || response.texture == 0U ||
        response.width != size.width || response.height != size.height ||
        response.pixel_format != static_cast<uint32_t>(pixel_format) ||
        (response.flags & MICROPIXEL_TEXTURE_FLAG_STREAMING) == 0U) {
        runtime::Panic("texture.create.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return StreamingTexture{
        Texture{response.texture, response.width, response.height, response.width, response.height, false},
        pixel_format};
}

TextureUpdateBatch Renderer::BeginTextureUpdateBatch() const {
    RequireOk(OpenResourceService(), "texture.batch.begin.open");
    RequireOk(CallVoid(resource_service, MICROPIXEL_RESOURCE_METHOD_TEXTURE_UPDATE_BATCH_BEGIN, nullptr, 0U),
              "texture.batch.begin");
    return TextureUpdateBatch{TextureUpdateBatch::CapabilityToken{}};
}

}  // namespace micropixel
