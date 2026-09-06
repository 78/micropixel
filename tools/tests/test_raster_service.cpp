// Graphics 1.6 raster kernels: kernel output against a reference sampler, draw
// list validation, resource quota and the Host-owned target buffers (byte
// order, in-flight veto) shared with DirectSurfaceService.
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>

#include "runtime/event_queue.hpp"
#include "runtime/graphics/raster_kernels.hpp"
#include "runtime/services/direct_surface_service.hpp"
#include "runtime/services/raster_service.hpp"

namespace {

using micropixel::runtime::DirectSurfaceService;
using micropixel::runtime::EventQueue;
using micropixel::runtime::RasterService;
namespace raster = micropixel::runtime::raster;

constexpr uint32_t kWidth = 64U;
constexpr uint32_t kHeight = 48U;
constexpr uint32_t kPitch = kWidth * 2U;
constexpr uint32_t kFrameBytes = kPitch * kHeight;
constexpr uint32_t kTexSize = 16U;
constexpr uint16_t kLightLevels = 4U;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

// Guest linear memory model: a flat buffer; offsets index into it directly.
alignas(64) uint8_t g_guest_memory[256U * 1024U]{};

bool ResolveGuestMemory(void*, uint32_t offset, uint32_t length, uint8_t** host_out) {
    if (length == 0U || offset > sizeof(g_guest_memory) || length > sizeof(g_guest_memory) - offset) {
        return false;
    }
    *host_out = g_guest_memory + offset;
    return true;
}

// Guest layout: texture/palette staging at 192 KiB. Frames are Host buffers.
constexpr uint32_t kStaging = 192U * 1024U;
constexpr uint32_t kFrame0 = 0U;
constexpr uint32_t kFrame1 = 1U;

class FakeGraphics final : public micropixel::device::Graphics {
   public:
    [[nodiscard]] bool Available() const override { return true; }
    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t&) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t Submit(const uint8_t*, uint32_t, const micropixel::device::TextureAccess&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t LoadFont(const micropixel::device::FontResourceView&, micropixel_font_info_t&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t ReleaseFont(micropixel_font_handle_t) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t MeasureText(micropixel_font_handle_t, const char*, uint32_t,
                                      micropixel_text_metrics_t&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t BeginBitmapUpdateFrame() override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t UpdateBitmap(const micropixel::device::BitmapView&, uint32_t, uint32_t, uint32_t, uint32_t,
                                       const uint8_t*, uint32_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t CommitBitmapUpdateFrame() override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t ScaleBitmap(const micropixel::device::BitmapView&,
                                      const micropixel::device::BitmapView&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t ShowLaunchBitmap(const micropixel::device::BitmapView&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    void DismissLaunchBitmap() override {}
    void ReleaseGuestResources() override { (void)DestroyDirectSurface(); }
    [[nodiscard]] int32_t CreateDirectSurface(const micropixel::device::DirectSurfaceConfig&,
                                              const micropixel::device::DirectSurfaceReleaseSink& release_sink,
                                              micropixel::device::DirectSurfaceInfo& info_out) override {
        sink = release_sink;
        info_out = {.native_pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565,
                    .native_flags = byte_swapped ? MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED : 0U,
                    .max_full_frame_fps = 60U};
        return MICROPIXEL_STATUS_OK;
    }
    [[nodiscard]] int32_t PresentDirectSurface(const micropixel::device::DirectSurfacePresentation&) override {
        return MICROPIXEL_STATUS_OK;
    }
    void SuspendDirectSurface() override {}
    void ResumeDirectSurface() override {}
    [[nodiscard]] int32_t DestroyDirectSurface() override { return MICROPIXEL_STATUS_OK; }

    bool byte_swapped{};
    micropixel::device::DirectSurfaceReleaseSink sink{};
};

// Procedural INDEX8 texture: texel(u, v) = 1 + (u * 7 + v * 3) % 200, never 0
// except along u == 0 so transparency has something to skip.
uint8_t Texel(uint32_t u, uint32_t v) { return u == 0U ? 0U : static_cast<uint8_t>(1U + (u * 7U + v * 3U) % 200U); }

std::vector<uint8_t> MakeTexture(bool column_major) {
    std::vector<uint8_t> texels(kTexSize * kTexSize);
    for (uint32_t u = 0U; u < kTexSize; ++u) {
        for (uint32_t v = 0U; v < kTexSize; ++v) {
            texels[column_major ? u * kTexSize + v : v * kTexSize + u] = Texel(u, v);
        }
    }
    return texels;
}

std::vector<uint16_t> MakePalette() {
    std::vector<uint16_t> lit(kLightLevels * 256U);
    for (uint32_t light = 0U; light < kLightLevels; ++light) {
        for (uint32_t index = 0U; index < 256U; ++index) {
            lit[light * 256U + index] = static_cast<uint16_t>(0x8000U | (light << 8U) | index);
        }
    }
    return lit;
}

struct DrawList final {
    std::vector<uint8_t> bytes;
    uint16_t record_count{};

    explicit DrawList(uint32_t target_buffer) {
        micropixel_raster_header_t header{};
        header.magic = MICROPIXEL_GRAPHICS_RASTER_MAGIC;
        header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
        header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
        header.target_buffer = target_buffer;
        header.target_width = kWidth;
        header.target_height = kHeight;
        header.target_pitch = kPitch;
        bytes.resize(sizeof(header));
        std::memcpy(bytes.data(), &header, sizeof(header));
    }
    template <typename Record>
    void Add(const Record& record) {
        const size_t offset = bytes.size();
        bytes.resize(offset + sizeof(record));
        std::memcpy(bytes.data() + offset, &record, sizeof(record));
        ++record_count;
    }
    micropixel_raster_header_t& Header() { return *reinterpret_cast<micropixel_raster_header_t*>(bytes.data()); }
    void Finish() {
        Header().total_size = static_cast<uint32_t>(bytes.size());
        Header().record_count = record_count;
    }
};

micropixel_raster_column_t Column(uint16_t x, int16_t y0, int16_t y1, uint8_t texture, uint8_t light, uint16_t u,
                                  int32_t v_start, int32_t v_step, uint8_t flags = 0U) {
    micropixel_raster_column_t column{};
    column.type = MICROPIXEL_RASTER_RECORD_COLUMN;
    column.flags = flags;
    column.texture = texture;
    column.light = light;
    column.x = x;
    column.y0 = y0;
    column.y1 = y1;
    column.u = u;
    column.v_start = v_start;
    column.v_step = v_step;
    return column;
}

micropixel_raster_span_pair_t SpanPair(uint16_t y_floor, uint16_t y_ceiling, uint16_t x0, uint16_t x1,
                                       uint8_t floor_texture, uint8_t ceiling_texture, uint8_t light, int32_t s,
                                       int32_t t, int32_t ds, int32_t dt) {
    micropixel_raster_span_pair_t span{};
    span.type = MICROPIXEL_RASTER_RECORD_SPAN_PAIR;
    span.floor_texture = floor_texture;
    span.ceiling_texture = ceiling_texture;
    span.y_floor = y_floor;
    span.y_ceiling = y_ceiling;
    span.x0 = x0;
    span.x1 = x1;
    span.light = light;
    span.s = s;
    span.t = t;
    span.ds = ds;
    span.dt = dt;
    return span;
}

micropixel_raster_sprite_t Sprite(int16_t x, int16_t y, uint16_t width, uint16_t height, uint8_t texture,
                                  uint8_t light, uint16_t u0, uint16_t v0, uint16_t src_width, uint16_t src_height,
                                  uint8_t flags = 0U, uint16_t color = 0U) {
    micropixel_raster_sprite_t sprite{};
    sprite.type = MICROPIXEL_RASTER_RECORD_SPRITE;
    sprite.flags = flags;
    sprite.texture = texture;
    sprite.light = light;
    sprite.x = x;
    sprite.y = y;
    sprite.width = width;
    sprite.height = height;
    sprite.u0 = u0;
    sprite.v0 = v0;
    sprite.src_width = src_width;
    sprite.src_height = src_height;
    sprite.color = color;
    return sprite;
}

micropixel_raster_rect_t Rect(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t color,
                              uint8_t alpha = 0xFFU) {
    micropixel_raster_rect_t rect{};
    rect.type = MICROPIXEL_RASTER_RECORD_RECT;
    rect.alpha = alpha;
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    rect.color = color;
    return rect;
}

uint16_t Lit(uint32_t light, uint8_t index) { return static_cast<uint16_t>(0x8000U | (light << 8U) | index); }

uint16_t Swap(uint16_t value) { return static_cast<uint16_t>((value << 8U) | (value >> 8U)); }

uint16_t PixelAt(const uint8_t* pixels, uint32_t pitch, uint32_t x, uint32_t y) {
    uint16_t value = 0U;
    std::memcpy(&value, pixels + y * pitch + x * 2U, sizeof(value));
    return value;
}

uint16_t PixelAt(const micropixel::runtime::HostBufferView& view, uint32_t x, uint32_t y) {
    return PixelAt(view.pixels, view.pitch, x, y);
}

void TestKernelsAgainstReference() {
    const std::vector<uint8_t> column_major = MakeTexture(true);
    const std::vector<uint8_t> row_major = MakeTexture(false);
    const std::vector<uint16_t> lit = MakePalette();
    const raster::Texture textures[2] = {
        {.pixels = column_major.data(),
         .width = kTexSize,
         .height = kTexSize,
         .log2_width = 4U,
         .log2_height = 4U,
         .layout = MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR},
        {.pixels = row_major.data(),
         .width = kTexSize,
         .height = kTexSize,
         .log2_width = 4U,
         .log2_height = 4U,
         .layout = MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR},
    };
    std::vector<uint8_t> frame(kFrameBytes, 0U);
    const raster::Target target{.pixels = frame.data(), .width = kWidth, .height = kHeight, .pitch = kPitch};

    // Column: v walks 16.16 through the texture, wrapping on the height mask.
    const int32_t v_step = (kTexSize << 16) / 10;  // ~1.6 texels per pixel
    const auto column = Column(5U, 3U, 40U, 0U, 2U, 9U, 3 << 16, v_step);
    raster::DrawColumn(target, textures[0], lit.data() + 2U * 256U, column);
    int32_t v = column.v_start;
    for (int32_t y = 3; y <= 40; ++y, v += v_step) {
        uint16_t value = 0U;
        std::memcpy(&value, frame.data() + y * kPitch + 5U * 2U, sizeof(value));
        Require(value == Lit(2U, Texel(9U, static_cast<uint32_t>(v >> 16) & (kTexSize - 1U))));
    }
    // Rows outside y0..y1 and the neighbouring columns stay untouched.
    uint16_t untouched = 0U;
    std::memcpy(&untouched, frame.data() + 2U * kPitch + 5U * 2U, sizeof(untouched));
    Require(untouched == 0U);
    std::memcpy(&untouched, frame.data() + 10U * kPitch + 6U * 2U, sizeof(untouched));
    Require(untouched == 0U);

    // Transparent column over u == 0 (all texel 0) leaves the previous pixels.
    const auto transparent = Column(5U, 3U, 40U, 0U, 1U, 0U, 0, v_step, MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0);
    raster::DrawColumn(target, textures[0], lit.data() + 256U, transparent);
    std::memcpy(&untouched, frame.data() + 3U * kPitch + 5U * 2U, sizeof(untouched));
    Require(untouched == Lit(2U, Texel(9U, 3U)));

    // Span pair: s/t are 16.16 world coordinates; integer part is the tile.
    const auto span = SpanPair(30U, 17U, 4U, 60U, 1U, 1U, 3U, 5 << 16, (2 << 16) + (1 << 15), 3000, -700);
    raster::DrawSpanPair(target, textures[1], textures[1], lit.data() + 3U * 256U, span);
    uint32_t s = static_cast<uint32_t>(span.s);
    uint32_t t = static_cast<uint32_t>(span.t);
    for (uint32_t x = 4U; x <= 60U; ++x, s += 3000U, t -= 700U) {
        const uint32_t tx = (s >> 12U) & 15U;
        const uint32_t ty = (t >> 12U) & 15U;
        uint16_t floor_value = 0U;
        uint16_t ceiling_value = 0U;
        std::memcpy(&floor_value, frame.data() + 30U * kPitch + x * 2U, sizeof(floor_value));
        std::memcpy(&ceiling_value, frame.data() + 17U * kPitch + x * 2U, sizeof(ceiling_value));
        Require(floor_value == Lit(3U, Texel(tx, ty)));
        Require(ceiling_value == Lit(3U, Texel(tx, ty)));
    }
    std::memcpy(&untouched, frame.data() + 30U * kPitch + 61U * 2U, sizeof(untouched));
    Require(untouched == 0U);

    // Sprite: 8x8 texels scaled x2 onto 16x16 at (50, 40), so the right and
    // bottom edges are clipped by the 64x48 target; transparent u == 0 skipped.
    std::fill(frame.begin(), frame.end(), 0U);
    const auto sprite = Sprite(50, 40, 16U, 16U, 0U, 1U, 4U, 2U, 8U, 8U, MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0);
    raster::DrawSprite(target, textures[0], lit.data() + 256U, sprite);
    for (uint32_t y = 40U; y < kHeight; ++y) {
        for (uint32_t x = 50U; x < kWidth; ++x) {
            const uint32_t u = 4U + (x - 50U) / 2U;
            const uint32_t v = 2U + (y - 40U) / 2U;
            Require(PixelAt(frame.data(), kPitch, x, y) == Lit(1U, Texel(u, v)));
        }
    }
    Require(PixelAt(frame.data(), kPitch, 49U, 45U) == 0U && PixelAt(frame.data(), kPitch, 55U, 39U) == 0U);
    // Negative origin clips the start; the sampled texel phase follows.
    const auto offscreen = Sprite(-4, -4, 8U, 8U, 0U, 1U, 0U, 0U, 8U, 8U, MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0);
    raster::DrawSprite(target, textures[0], lit.data() + 256U, offscreen);
    Require(PixelAt(frame.data(), kPitch, 0U, 0U) == Lit(1U, Texel(4U, 4U)));
    Require(PixelAt(frame.data(), kPitch, 3U, 3U) == Lit(1U, Texel(7U, 7U)));
    Require(PixelAt(frame.data(), kPitch, 4U, 4U) == 0U);
    // SOLID_COLOR writes the record color for every opaque texel.
    const auto glyph = Sprite(20, 20, 4U, 4U, 0U, 0U, 0U, 0U, 4U, 4U,
                              MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 | MICROPIXEL_RASTER_SPRITE_SOLID_COLOR,
                              0x1234U);
    raster::DrawSprite(target, textures[0], nullptr, glyph);
    Require(PixelAt(frame.data(), kPitch, 20U, 20U) == 0U);  // u == 0 column is transparent
    Require(PixelAt(frame.data(), kPitch, 21U, 20U) == 0x1234U && PixelAt(frame.data(), kPitch, 23U, 23U) == 0x1234U);

    // Rect: opaque fill clipped at the edge, then a 50% blend over it.
    raster::DrawRect(target, Rect(60, 10, 10U, 2U, 0xF800U));
    Require(PixelAt(frame.data(), kPitch, 60U, 10U) == 0xF800U && PixelAt(frame.data(), kPitch, 63U, 11U) == 0xF800U);
    Require(PixelAt(frame.data(), kPitch, 59U, 10U) == 0U && PixelAt(frame.data(), kPitch, 60U, 12U) == 0U);
    raster::DrawRect(target, Rect(60, 10, 2U, 1U, 0x001FU, 127U));
    const uint16_t blended = PixelAt(frame.data(), kPitch, 60U, 10U);
    Require((blended >> 11U) == 15U && (blended & 0x1FU) == 15U && ((blended >> 5U) & 0x3FU) == 0U);
    Require(PixelAt(frame.data(), kPitch, 62U, 10U) == 0xF800U);

    // Byte-swapped target: colors and blends land in panel order.
    const raster::Target swapped{
        .pixels = frame.data(), .width = kWidth, .height = kHeight, .pitch = kPitch, .byte_swapped = true};
    raster::DrawRect(swapped, Rect(0, 30, 4U, 1U, 0xF800U));
    Require(PixelAt(frame.data(), kPitch, 0U, 30U) == Swap(0xF800U));
    raster::DrawRect(swapped, Rect(0, 30, 1U, 1U, 0x001FU, 127U));
    Require(Swap(PixelAt(frame.data(), kPitch, 0U, 30U)) == blended);
    raster::DrawSprite(swapped, textures[0], nullptr, Sprite(0, 31, 4U, 1U, 0U, 0U, 1U, 0U, 4U, 1U,
                                                             MICROPIXEL_RASTER_SPRITE_SOLID_COLOR, 0x1234U));
    Require(PixelAt(frame.data(), kPitch, 0U, 31U) == Swap(0x1234U));
}

void TestServiceUploadsAndDraws() {
    FakeGraphics backend;
    micropixel::device::GraphicsService graphics{backend, micropixel::device::DisplayInfo{}};
    EventQueue events;
    Require(events.valid());
    DirectSurfaceService surfaces{graphics, events, 0};
    const micropixel::runtime::GuestMemoryAccess access{
        .context = nullptr, .resolve = ResolveGuestMemory, .stable_base = true};
    surfaces.BindGuestMemory(access);

    // Quota: two 256 B textures plus a 2 KiB palette fit; a third texture does not.
    RasterService service{2U * kTexSize * kTexSize + kLightLevels * 256U * 2U};
    service.BindGuestMemory(access);
    Require(service.available());

    const std::vector<uint8_t> column_major = MakeTexture(true);
    const std::vector<uint8_t> row_major = MakeTexture(false);
    const std::vector<uint16_t> lit = MakePalette();
    std::memcpy(g_guest_memory + kStaging, column_major.data(), column_major.size());
    std::memcpy(g_guest_memory + kStaging + 1024U, row_major.data(), row_major.size());
    std::memcpy(g_guest_memory + kStaging + 2048U, lit.data(), lit.size() * sizeof(uint16_t));

    micropixel_raster_texture_upload_request_t upload{};
    upload.size = sizeof(upload);
    upload.slot = 0U;
    upload.width = kTexSize;
    upload.height = kTexSize;
    upload.layout = MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR;
    upload.pixels = kStaging;
    upload.length = kTexSize * kTexSize;

    // Drawing before any palette exists is a state error, not a crash.
    DrawList early{kFrame0};
    early.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
    early.Finish();
    Require(service.Submit(early.bytes.data(), static_cast<uint32_t>(early.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_STALE_STATE);
    // A RECT needs no palette but does need a Host-buffer surface.
    DrawList no_surface{kFrame0};
    no_surface.Add(Rect(0, 0, 1U, 1U, 0xFFFFU));
    no_surface.Finish();
    Require(service.Submit(no_surface.bytes.data(), static_cast<uint32_t>(no_surface.bytes.size()), surfaces)
                .error()
                .status == MICROPIXEL_STATUS_NOT_FOUND);

    // Host-buffer surface with two frames; the kernels write into them.
    micropixel_surface_create_request_t create{};
    create.size = sizeof(create);
    create.width = kWidth;
    create.height = kHeight;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    create.buffer_count = 2U;
    Require(surfaces.Create(create).has_value());
    micropixel::runtime::HostBufferView frame0{};
    micropixel::runtime::HostBufferView frame1{};
    Require(surfaces.HostBuffer(kFrame0, frame0) == MICROPIXEL_STATUS_OK);
    Require(surfaces.HostBuffer(kFrame1, frame1) == MICROPIXEL_STATUS_OK);
    Require(service.Submit(no_surface.bytes.data(), static_cast<uint32_t>(no_surface.bytes.size()), surfaces)
                .has_value());
    Require(PixelAt(frame0, 0U, 0U) == 0xFFFFU && PixelAt(frame0, 1U, 0U) == 0U);

    Require(service.UploadTexture(upload).has_value());
    auto bad = upload;
    bad.width = 24U;  // not a power of two
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = upload;
    bad.slot = MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = upload;
    bad.length = upload.length - 1U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = upload;
    bad.pixels = sizeof(g_guest_memory) - 16U;  // runs past Guest memory
    bad.slot = 5U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);
    // A refused re-upload of an occupied slot keeps the old texture: slot 0
    // must still serve a column afterwards (checked once the palette exists).
    bad.slot = 0U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);

    upload.slot = 1U;
    upload.layout = MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR;
    upload.pixels = kStaging + 1024U;
    Require(service.UploadTexture(upload).has_value());

    micropixel_raster_palette_upload_request_t palette{};
    palette.size = sizeof(palette);
    palette.light_levels = kLightLevels;
    palette.pixels = kStaging + 2048U;
    palette.length = kLightLevels * 256U * 2U;
    Require(service.UploadPalette(palette).has_value());
    auto bad_palette = palette;
    bad_palette.length -= 2U;
    Require(service.UploadPalette(bad_palette).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);

    // Quota exhausted for a third texture; re-uploading an existing slot still works.
    upload.slot = 2U;
    Require(service.UploadTexture(upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    upload.slot = 1U;
    Require(service.UploadTexture(upload).has_value());

    // A valid list renders exactly like the kernels.
    DrawList list{kFrame0};
    list.Add(Column(10U, 0U, kHeight - 1U, 0U, 1U, 3U, 0, 1 << 15));
    list.Add(SpanPair(40U, 7U, 0U, kWidth - 1U, 1U, 1U, 0U, 0, 0, 1 << 12, 1 << 12));
    list.Add(Sprite(30, 20, 4U, 4U, 0U, 2U, 4U, 4U, 4U, 4U));
    list.Add(Rect(50, 20, 2U, 2U, 0x07E0U));
    list.Finish();
    std::memset(frame0.pixels, 0, kFrameBytes);
    Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame0, 10U, 0U) == Lit(1U, Texel(3U, 0U)));
    Require(PixelAt(frame0, 10U, 2U) == Lit(1U, Texel(3U, 1U)));
    Require(PixelAt(frame0, 0U, 40U) == Lit(0U, Texel(0U, 0U)));
    Require(PixelAt(frame0, 1U, 40U) == Lit(0U, Texel(1U, 1U)));
    Require(PixelAt(frame0, 1U, 7U) == Lit(0U, Texel(1U, 1U)));
    Require(PixelAt(frame0, 11U, 0U) == 0U);
    Require(PixelAt(frame0, 30U, 20U) == Lit(2U, Texel(4U, 4U)) && PixelAt(frame0, 33U, 23U) == Lit(2U, Texel(7U, 7U)));
    Require(PixelAt(frame0, 50U, 20U) == 0x07E0U && PixelAt(frame0, 51U, 21U) == 0x07E0U);
    // The other buffer is untouched.
    for (uint32_t index = 0U; index < kFrameBytes; ++index) {
        Require(frame1.pixels[index] == 0U);
    }

    // Every malformed list is rejected before anything is written.
    std::memset(frame0.pixels, 0, kFrameBytes);
    auto reject = [&](DrawList& broken, int32_t status) {
        broken.Finish();
        Require(
            service.Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces).error().status ==
            status);
        for (uint32_t index = 0U; index < kFrameBytes; ++index) {
            Require(frame0.pixels[index] == 0U);
        }
    };
    {
        DrawList broken{kFrame0};
        broken.Add(Column(kWidth, 0U, 1U, 0U, 0U, 0U, 0, 0));  // x outside target
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 5U, 4U, 0U, 0U, 0U, 0, 0));  // y1 < y0
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, kHeight, 0U, 0U, 0U, 0, 0));  // y1 outside target
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, kLightLevels, 0U, 0, 0));  // light too high
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 1U, 0U, 0U, 0, 0));  // row-major slot in a column record
        reject(broken, MICROPIXEL_STATUS_NOT_FOUND);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 7U, 0U, 0U, 0, 0));  // empty slot
        reject(broken, MICROPIXEL_STATUS_NOT_FOUND);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, kTexSize, 0, 0));  // u outside texture
        reject(broken, MICROPIXEL_STATUS_NOT_FOUND);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(SpanPair(0U, 1U, 5U, 4U, 1U, 1U, 0U, 0, 0, 0, 0));  // x1 < x0
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(SpanPair(0U, 1U, 0U, 1U, 0U, 1U, 0U, 0, 0, 0, 0));  // column-major slot in a span
        reject(broken, MICROPIXEL_STATUS_NOT_FOUND);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
        broken.Finish();
        broken.Header().record_count = 2U;  // claims more records than bytes
        Require(
            service.Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
        broken.Finish();
        broken.Header().magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC;
        Require(
            service.Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Sprite(0, 0, 4U, 4U, 0U, 0U, 13U, 0U, 4U, 4U));  // u0 + src_width past the texture
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Sprite(0, 0, 0U, 4U, 0U, 0U, 0U, 0U, 4U, 4U));  // empty destination
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Sprite(0, 0, 4U, 4U, 1U, 0U, 0U, 0U, 4U, 4U));  // row-major slot in a sprite
        reject(broken, MICROPIXEL_STATUS_NOT_FOUND);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Sprite(0, 0, 4U, 4U, 0U, kLightLevels, 0U, 0U, 4U, 4U));  // light too high
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Rect(0, 0, 4U, 4U, 0U, 0U));  // alpha 0 draws nothing: rejected
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
        broken.Finish();
        broken.Header().target_pitch = kPitch - 2U;  // pitch narrower than the row
        Require(
            service.Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
        broken.Finish();
        broken.Header().target_width = kWidth / 2U;  // geometry other than the buffer's
        Require(
            service.Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{2U};  // no such buffer
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
        reject(broken, MICROPIXEL_STATUS_NOT_FOUND);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
        broken.Finish();
        broken.Header().reserved0 = 1U;
        Require(
            service.Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    // A presented buffer is off limits until the panel releases it.
    micropixel_surface_present_request_t present{};
    present.size = sizeof(present);
    present.surface = 1U;
    present.buffer_index = kFrame1;
    present.pitch = kPitch;
    present.src_width = kWidth;
    present.src_height = kHeight;
    Require(surfaces.Present(present).has_value());
    DrawList busy{kFrame1};
    busy.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
    busy.Finish();
    Require(service.Submit(busy.bytes.data(), static_cast<uint32_t>(busy.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_STALE_STATE);
    DrawList free_frame{kFrame0};
    free_frame.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
    free_frame.Finish();
    Require(
        service.Submit(free_frame.bytes.data(), static_cast<uint32_t>(free_frame.bytes.size()), surfaces).has_value());
    backend.sink.release(backend.sink.context, 1U, 10U);
    Require(service.Submit(busy.bytes.data(), static_cast<uint32_t>(busy.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame1, 0U, 0U) == Lit(0U, Texel(0U, 0U)));

    // Shutdown frees everything; drawing afterwards needs a palette again.
    service.Shutdown();
    Require(service.Submit(free_frame.bytes.data(), static_cast<uint32_t>(free_frame.bytes.size()), surfaces)
                .error()
                .status == MICROPIXEL_STATUS_STALE_STATE);
    surfaces.Shutdown();
}

// On a byte-swapped panel the palette is converted once and record colors per
// record, so every pixel the kernels write is already what the panel scans.
void TestSwappedPanel() {
    FakeGraphics backend;
    backend.byte_swapped = true;
    micropixel::device::GraphicsService graphics{backend, micropixel::device::DisplayInfo{}};
    EventQueue events;
    Require(events.valid());
    DirectSurfaceService surfaces{graphics, events, 0};
    const micropixel::runtime::GuestMemoryAccess access{
        .context = nullptr, .resolve = ResolveGuestMemory, .stable_base = false};
    RasterService service{kTexSize * kTexSize + kLightLevels * 256U * 2U};
    service.BindGuestMemory(access);

    const std::vector<uint8_t> column_major = MakeTexture(true);
    const std::vector<uint16_t> lit = MakePalette();
    std::memcpy(g_guest_memory + kStaging, column_major.data(), column_major.size());
    std::memcpy(g_guest_memory + kStaging + 2048U, lit.data(), lit.size() * sizeof(uint16_t));
    micropixel_raster_texture_upload_request_t upload{};
    upload.size = sizeof(upload);
    upload.width = kTexSize;
    upload.height = kTexSize;
    upload.layout = MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR;
    upload.pixels = kStaging;
    upload.length = kTexSize * kTexSize;
    Require(service.UploadTexture(upload).has_value());
    micropixel_raster_palette_upload_request_t palette{};
    palette.size = sizeof(palette);
    palette.light_levels = kLightLevels;
    palette.pixels = kStaging + 2048U;
    palette.length = kLightLevels * 256U * 2U;
    Require(service.UploadPalette(palette).has_value());

    micropixel_surface_create_request_t create{};
    create.size = sizeof(create);
    create.width = kWidth;
    create.height = kHeight;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    create.buffer_count = 1U;
    auto created = surfaces.Create(create);
    Require(created.has_value() && (created->native_flags & MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED) != 0U);
    micropixel::runtime::HostBufferView frame{};
    Require(surfaces.HostBuffer(0U, frame) == MICROPIXEL_STATUS_OK);

    DrawList list{0U};
    list.Add(Column(0U, 0U, 0U, 0U, 3U, 5U, 0, 0));
    list.Add(Rect(1, 0, 1U, 1U, 0xF800U));
    list.Add(Sprite(2, 0, 1U, 1U, 0U, 0U, 1U, 0U, 1U, 1U, MICROPIXEL_RASTER_SPRITE_SOLID_COLOR, 0x07E0U));
    list.Finish();
    Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame, 0U, 0U) == Swap(Lit(3U, Texel(5U, 0U))));
    Require(PixelAt(frame, 1U, 0U) == Swap(0xF800U));
    Require(PixelAt(frame, 2U, 0U) == Swap(0x07E0U));
    // A palette re-upload arrives canonical again and is converted anew.
    Require(service.UploadPalette(palette).has_value());
    Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame, 0U, 0U) == Swap(Lit(3U, Texel(5U, 0U))));
    service.Shutdown();
    surfaces.Shutdown();
}

void TestDisabledPool() {
    FakeGraphics backend;
    micropixel::device::GraphicsService graphics{backend, micropixel::device::DisplayInfo{}};
    EventQueue events;
    DirectSurfaceService surfaces{graphics, events, 0};
    RasterService service{0U};
    Require(!service.available());
    micropixel_raster_palette_upload_request_t palette{};
    palette.size = sizeof(palette);
    palette.light_levels = 1U;
    palette.length = 512U;
    Require(service.UploadPalette(palette).error().status == MICROPIXEL_STATUS_UNSUPPORTED);
    DrawList list{kFrame0};
    list.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
    list.Finish();
    Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_UNSUPPORTED);
}

}  // namespace

int main() {
    Require(raster::ValidTextureDimension(8U) && raster::ValidTextureDimension(128U) &&
            !raster::ValidTextureDimension(4U) && !raster::ValidTextureDimension(256U) &&
            !raster::ValidTextureDimension(96U));
    Require(raster::Log2Exact(64U) == 6U && raster::Log2Exact(8U) == 3U);
    TestKernelsAgainstReference();
    TestServiceUploadsAndDraws();
    TestSwappedPanel();
    TestDisabledPool();
    return 0;
}
