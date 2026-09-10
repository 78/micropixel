// Graphics 1.6 raster kernels: kernel output against a reference sampler, draw
// list validation, resource quota and the Host-owned target buffers (byte
// order, in-flight veto) shared with DirectSurfaceService.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/event_queue.hpp"
#include "runtime/graphics/raster_kernels.hpp"
#include "runtime/services/direct_surface_service.hpp"
#include "runtime/services/raster_service.hpp"

namespace {
std::unordered_map<void*, size_t> allocations;
size_t allocation_attempts{};
size_t fail_at{};
}  // namespace
void* micropixel_test_psram_allocate(size_t bytes) {
    ++allocation_attempts;
    // Keep the heap exhausted for every allocation after the threshold,
    // including fallback attempts when internal SRAM is enabled.
    if (fail_at != 0U && allocation_attempts >= fail_at) return nullptr;
    void* p = std::malloc(bytes);
    if (p != nullptr) allocations.emplace(p, bytes);
    return p;
}
void micropixel_test_psram_free(void* p) {
    allocations.erase(p);
    std::free(p);
}
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

void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "raster assertion line %u\n", location.line());
        std::abort();
    }
}

// Guest linear memory model: a flat buffer; offsets index into it directly.
alignas(64) uint8_t g_guest_memory[2U * 1024U * 1024U]{};

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
    [[nodiscard]] int32_t DrawText(const micropixel::device::TextTarget&, int32_t, int32_t, uint32_t,
                                   micropixel_font_handle_t, const char*, uint32_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t CopyOpaqueBlocks(const micropixel::device::PixelTarget&,
                                           const micropixel::device::OpaqueCopyBlock*, uint32_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
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

    explicit DrawList(uint32_t buffer_index) {
        micropixel_raster_header_t header{};
        header.magic = MICROPIXEL_GRAPHICS_RASTER_MAGIC;
        header.surface_handle = 1U;
        header.buffer_index = buffer_index;
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
    column.texture_slot = texture;
    column.light_level = light;
    column.x = x;
    column.y0 = y0;
    column.y1 = y1;
    column.u = u;
    column.v_start = v_start;
    column.v_step = v_step;
    return column;
}

micropixel_raster_span_pair_t SpanPair(uint16_t y_floor, uint16_t y_ceiling, uint16_t x0, uint16_t x1,
                                       uint8_t floor_texture_slot, uint8_t ceiling_texture_slot, uint8_t light,
                                       int32_t s, int32_t t, int32_t ds, int32_t dt) {
    micropixel_raster_span_pair_t span{};
    span.type = MICROPIXEL_RASTER_RECORD_SPAN_PAIR;
    span.floor_texture_slot = floor_texture_slot;
    span.ceiling_texture_slot = ceiling_texture_slot;
    span.y_floor = y_floor;
    span.y_ceiling = y_ceiling;
    span.x0 = x0;
    span.x1 = x1;
    span.light_level = light;
    span.s = s;
    span.t = t;
    span.ds = ds;
    span.dt = dt;
    return span;
}

micropixel_raster_span_t Span(uint16_t y, uint16_t x0, uint16_t x1, uint8_t texture_slot, uint8_t light, int32_t s,
                              int32_t t, int32_t ds, int32_t dt) {
    micropixel_raster_span_t span{};
    span.type = MICROPIXEL_RASTER_RECORD_SPAN;
    span.texture_slot = texture_slot;
    span.light_level = light;
    span.y = y;
    span.x0 = x0;
    span.x1 = x1;
    span.s = s;
    span.t = t;
    span.ds = ds;
    span.dt = dt;
    return span;
}

// TEXT header plus its zero-padded UTF-8 payload as raw list bytes.
std::vector<uint8_t> TextRecord(int16_t x, int16_t y, uint16_t color, micropixel_font_handle_t font,
                                std::string_view text, uint16_t declared_length = 0U) {
    micropixel_raster_text_t header{};
    header.type = MICROPIXEL_RASTER_RECORD_TEXT;
    header.text_length = declared_length != 0U ? declared_length : static_cast<uint16_t>(text.size());
    header.x = x;
    header.y = y;
    header.color = color;
    header.font_handle = font;
    std::vector<uint8_t> bytes(sizeof(header) + ((text.size() + 3U) & ~3U), 0U);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), text.data(), text.size());
    return bytes;
}

micropixel_raster_sprite_t Sprite(int16_t x, int16_t y, uint16_t width, uint16_t height, uint8_t texture, uint8_t light,
                                  uint16_t u0, uint16_t v0, uint16_t source_width, uint16_t source_height,
                                  uint8_t flags = 0U, uint16_t color = 0U) {
    micropixel_raster_sprite_t sprite{};
    sprite.type = MICROPIXEL_RASTER_RECORD_SPRITE;
    sprite.flags = flags;
    sprite.texture_slot = texture;
    sprite.light_level = light;
    sprite.x = x;
    sprite.y = y;
    sprite.width = width;
    sprite.height = height;
    sprite.source_x = u0;
    sprite.source_y = v0;
    sprite.source_width = source_width;
    sprite.source_height = source_height;
    sprite.color = color;
    return sprite;
}

micropixel_raster_rect_t Rect(int16_t x, int16_t y, uint16_t width, uint16_t height, uint16_t color,
                              uint8_t alpha = 0xFFU) {
    micropixel_raster_rect_t rect{};
    rect.type = MICROPIXEL_RASTER_RECORD_RECT;
    rect.opacity = alpha;
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    rect.color = color;
    return rect;
}

// Polygon corner from pixel / texel / level coordinates (12.4 and 8.8 wire fixed point).
micropixel_raster_vertex_t Vertex(double x, double y, double u, double v, uint8_t light) {
    micropixel_raster_vertex_t vertex{};
    vertex.x = static_cast<int16_t>(std::lround(x * 16.0));
    vertex.y = static_cast<int16_t>(std::lround(y * 16.0));
    vertex.u = static_cast<uint16_t>(std::lround(u * 256.0));
    vertex.v = static_cast<uint16_t>(std::lround(v * 256.0));
    vertex.light = light;
    return vertex;
}

micropixel_raster_quad_t Quad(uint8_t texture, const micropixel_raster_vertex_t (&corners)[4], uint8_t flags = 0U) {
    micropixel_raster_quad_t quad{};
    quad.type = MICROPIXEL_RASTER_RECORD_QUAD;
    quad.flags = flags;
    quad.texture_slot = texture;
    std::copy_n(corners, 4U, quad.vertices);
    return quad;
}

micropixel_raster_triangle_t Triangle(uint8_t texture, const micropixel_raster_vertex_t (&corners)[3],
                                      uint8_t flags = 0U) {
    micropixel_raster_triangle_t triangle{};
    triangle.type = MICROPIXEL_RASTER_RECORD_TRIANGLE;
    triangle.flags = flags;
    triangle.texture_slot = texture;
    std::copy_n(corners, 3U, triangle.vertices);
    return triangle;
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

    // Warp with two u fraction bits: entries hold u in quarter texels and
    // u_offset advances in quarter texels, so texel column is (u4 + off) >> 2.
    {
        std::vector<uint32_t> entries(4U);
        for (uint32_t x = 0U; x < 4U; ++x) entries[x] = (2U << 24U) | (5U << 12U) | ((x * 4U) + 3U);
        const raster::WarpMap map{.entries = entries.data(), .width = 4U, .height = 1U, .max_light = 2U};
        const raster::Palette palette{.entries = lit.data(), .light_levels = kLightLevels};
        micropixel_raster_warp_t warp{};
        warp.type = MICROPIXEL_RASTER_RECORD_WARP;
        warp.x = 10;
        warp.y = 10;
        warp.u_offset = 4U * 15U + 2U;  // 15.5 texels: wraps around the 16-wide texture
        warp.u_fraction_bits = 2U;
        raster::DrawWarp(target, map, textures[1], palette, warp);
        for (uint32_t x = 0U; x < 4U; ++x) {
            // (4x + 3 + 62) / 4 = x + 16.25 -> x + 16, wrapped to x.
            const uint32_t u = (((x * 4U + 3U) + (4U * 15U + 2U)) >> 2U) & 15U;
            Require(u == x);
            Require(PixelAt(frame.data(), kPitch, 10U + x, 10U) == Lit(2U, row_major[5U * kTexSize + u]));
        }
        std::fill(frame.begin(), frame.end(), 0U);
    }

    // Warp: a 6x5 map whose row 0 is skipped, row 1 solid palette indices,
    // rows 2..4 textured with u/v that the offsets wrap around the 16x16
    // texture; drawn at (-2, 44) so the left two columns and the bottom row
    // are clipped.
    {
        std::vector<uint32_t> entries(6U * 5U, MICROPIXEL_RASTER_WARP_ENTRY_SKIP);
        for (uint32_t x = 0U; x < 6U; ++x) {
            entries[6U + x] = MICROPIXEL_RASTER_WARP_ENTRY_SOLID | (1U << 24U) | (10U + x);
            for (uint32_t y = 2U; y < 5U; ++y) {
                entries[y * 6U + x] = (static_cast<uint32_t>(y) << 24U) | ((y + 13U) << 12U) | (x + 14U);
            }
        }
        Require(raster::WarpMaxLight(entries.data(), entries.size()) == 4U);
        const raster::WarpMap map{.entries = entries.data(), .width = 6U, .height = 5U, .max_light = 4U};
        const raster::Palette palette{.entries = lit.data(), .light_levels = kLightLevels};
        micropixel_raster_warp_t warp{};
        warp.type = MICROPIXEL_RASTER_RECORD_WARP;
        warp.flags = MICROPIXEL_RASTER_WARP_FILL_SKIPPED;
        warp.x = -2;
        warp.y = 44;
        warp.u_offset = 3U;
        warp.v_offset = 1U;
        warp.fill_color = 0x1234U;
        raster::DrawWarp(target, map, textures[1], palette, warp);
        for (uint32_t x = 0U; x < 4U; ++x) {
            Require(PixelAt(frame.data(), kPitch, x, 44U) == 0x1234U);
            Require(PixelAt(frame.data(), kPitch, x, 45U) == Lit(1U, static_cast<uint8_t>(12U + x)));
            for (uint32_t y = 2U; y < 4U; ++y) {
                const uint32_t u = (x + 2U + 14U + 3U) & 15U;
                const uint32_t v = (y + 13U + 1U) & 15U;
                Require(PixelAt(frame.data(), kPitch, x, 44U + y) == Lit(y, Texel(u, v)));
            }
        }
        Require(PixelAt(frame.data(), kPitch, 4U, 44U) == 0U && PixelAt(frame.data(), kPitch, 0U, 43U) == 0U);
        // Row spans let the kernel skip the empty parts of a row; the result
        // must not change, fill included.
        std::vector<uint8_t> reference = frame;
        uint16_t spans[10];
        for (uint32_t row = 0U; row < 5U; ++row) {
            raster::WarpRowSpan(entries.data() + row * 6U, 6U, spans[row * 2U], spans[row * 2U + 1U]);
        }
        Require(spans[0] == spans[1] && spans[2] == 0U && spans[3] == 6U);
        entries[2U * 6U] = MICROPIXEL_RASTER_WARP_ENTRY_SKIP;  // row 2 now starts at x = 1
        entries[2U * 6U + 5U] = MICROPIXEL_RASTER_WARP_ENTRY_SKIP;
        raster::WarpRowSpan(entries.data() + 12U, 6U, spans[4], spans[5]);
        Require(spans[4] == 1U && spans[5] == 5U);
        // The fused upload scan agrees with the per-row helpers.
        uint16_t scanned[10];
        uint8_t scanned_light = 0U;
        Require(raster::WarpScanRows(entries.data(), 6U, 5U, scanned, scanned_light));
        Require(scanned_light == raster::WarpMaxLight(entries.data(), entries.size()));
        for (uint32_t i = 0U; i < 10U; ++i) Require(scanned[i] == spans[i]);
        std::fill(frame.begin(), frame.end(), 0U);
        raster::DrawWarp(target, map, textures[1], palette, warp);
        reference = frame;
        std::fill(frame.begin(), frame.end(), 0U);
        const raster::WarpMap spanned{
            .entries = entries.data(), .row_spans = spans, .width = 6U, .height = 5U, .max_light = 4U};
        raster::DrawWarp(target, spanned, textures[1], palette, warp);
        Require(frame == reference);
        // Map x = 5 (target x = 3) is outside row 2's span: filled, not sampled.
        Require(PixelAt(frame.data(), kPitch, 3U, 46U) == 0x1234U);
        Require(PixelAt(frame.data(), kPitch, 2U, 46U) == Lit(2U, Texel((2U + 2U + 14U + 3U) & 15U, (2U + 14U) & 15U)));
        // Without FILL_SKIPPED the skipped row keeps whatever was there.
        warp.flags = 0U;
        warp.fill_color = 0x4321U;
        raster::DrawWarp(target, map, textures[1], palette, warp);
        Require(PixelAt(frame.data(), kPitch, 0U, 44U) == 0x1234U);
        // A reserved bit poisons the whole upload.
        entries[8] |= MICROPIXEL_RASTER_WARP_ENTRY_RESERVED;
        Require(raster::WarpMaxLight(entries.data(), entries.size()) == UINT8_MAX);
        Require(!raster::WarpScanRows(entries.data(), 6U, 5U, scanned, scanned_light));
        std::fill(frame.begin(), frame.end(), 0U);
    }

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

    // Span: the single-row form samples like SpanPair. dt == 0 takes the
    // hoisted-row unrolled path (61 pixels: 15 groups of four plus one), a
    // non-zero dt the general one; both agree with the SpanPair walk.
    std::fill(frame.begin(), frame.end(), 0U);
    for (const int32_t dt : {0, -700}) {
        const auto single = Span(33U, 3U, 63U, 1U, 3U, (5 << 16) + 3000, (2 << 16) + (1 << 15), 3000, dt);
        raster::DrawSpan(target, textures[1], lit.data() + 3U * 256U, single);
        const auto pair = SpanPair(34U, 34U, 3U, 63U, 1U, 1U, 3U, (5 << 16) + 3000, (2 << 16) + (1 << 15), 3000, dt);
        raster::DrawSpanPair(target, textures[1], textures[1], lit.data() + 3U * 256U, pair);
        for (uint32_t x = 3U; x <= 63U; ++x) {
            Require(PixelAt(frame.data(), kPitch, x, 33U) == PixelAt(frame.data(), kPitch, x, 34U));
        }
        Require(PixelAt(frame.data(), kPitch, 3U, 33U) == Lit(3U, Texel(0U, 8U)));
        Require(PixelAt(frame.data(), kPitch, 4U, 33U) == Lit(3U, Texel(1U, dt == 0 ? 8U : 7U)));
        Require(PixelAt(frame.data(), kPitch, 2U, 33U) == 0U && PixelAt(frame.data(), kPitch, 3U, 32U) == 0U);
    }
    // Non-power-of-two texture: fract(s) * width selects the texel.
    {
        std::vector<uint8_t> odd(5U * 3U);
        for (uint32_t index = 0U; index < odd.size(); ++index) odd[index] = static_cast<uint8_t>(index + 1U);
        const raster::Texture texture{.pixels = odd.data(),
                                      .width = 5U,
                                      .height = 3U,
                                      .log2_width = UINT8_MAX,
                                      .log2_height = UINT8_MAX,
                                      .layout = MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR};
        const auto single = Span(35U, 0U, 9U, 1U, 0U, 0, (1 << 16) / 3 * 2 + 1, (1 << 16) / 10, 0);
        raster::DrawSpan(target, texture, lit.data(), single);
        uint32_t s = 0U;
        for (uint32_t x = 0U; x <= 9U; ++x, s += static_cast<uint32_t>(single.ds)) {
            const uint32_t tx = ((s & 0xffffU) * 5U) >> 16U;
            Require(PixelAt(frame.data(), kPitch, x, 35U) == Lit(0U, odd[2U * 5U + tx]));
        }
    }

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
    const auto glyph =
        Sprite(20, 20, 4U, 4U, 0U, 0U, 0U, 0U, 4U, 4U,
               MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 | MICROPIXEL_RASTER_SPRITE_SOLID_COLOR, 0x1234U);
    raster::DrawSprite(target, textures[0], nullptr, glyph);
    Require(PixelAt(frame.data(), kPitch, 20U, 20U) == 0U);  // u == 0 column is transparent
    Require(PixelAt(frame.data(), kPitch, 21U, 20U) == 0x1234U && PixelAt(frame.data(), kPitch, 23U, 23U) == 0x1234U);

    // Rect: opaque fill clipped at the edge, then a 50% blend over it.
    raster::DrawRect(target, Rect(60, 10, 10U, 2U, 0xF800U));
    Require(PixelAt(frame.data(), kPitch, 60U, 10U) == 0xF800U && PixelAt(frame.data(), kPitch, 63U, 11U) == 0xF800U);
    Require(PixelAt(frame.data(), kPitch, 59U, 10U) == 0U && PixelAt(frame.data(), kPitch, 60U, 12U) == 0U);
    raster::DrawRect(target, Rect(60, 10, 2U, 1U, 0x001FU, 127U));
    const uint16_t blended = PixelAt(frame.data(), kPitch, 60U, 10U);
    // 31 * 0.5 rounds to 16 (the blend works in 1/64 coverage steps, nearest).
    Require((blended >> 11U) == 16U && (blended & 0x1FU) == 16U && ((blended >> 5U) & 0x3FU) == 0U);
    Require(PixelAt(frame.data(), kPitch, 62U, 10U) == 0xF800U);

    // Byte-swapped target: colors and blends land in panel order.
    const raster::Target swapped{
        .pixels = frame.data(), .width = kWidth, .height = kHeight, .pitch = kPitch, .byte_swapped = true};
    raster::DrawRect(swapped, Rect(0, 30, 4U, 1U, 0xF800U));
    Require(PixelAt(frame.data(), kPitch, 0U, 30U) == Swap(0xF800U));
    raster::DrawRect(swapped, Rect(0, 30, 1U, 1U, 0x001FU, 127U));
    Require(Swap(PixelAt(frame.data(), kPitch, 0U, 30U)) == blended);
    raster::DrawSprite(swapped, textures[0], nullptr,
                       Sprite(0, 31, 4U, 1U, 0U, 0U, 1U, 0U, 4U, 1U, MICROPIXEL_RASTER_SPRITE_SOLID_COLOR, 0x1234U));
    Require(PixelAt(frame.data(), kPitch, 0U, 31U) == Swap(0x1234U));
}

// Signed distance (pixels, positive inside) from a point to a convex polygon
// given in either winding; the reference for coverage checks.
double InsideMargin(const micropixel_raster_vertex_t* vertices, uint32_t count, double px, double py) {
    double area = 0.0;
    for (uint32_t i = 0U; i < count; ++i) {
        const auto& a = vertices[i];
        const auto& b = vertices[(i + 1U) % count];
        area += a.x / 16.0 * (b.y / 16.0) - b.x / 16.0 * (a.y / 16.0);
    }
    const double sign = area > 0.0 ? 1.0 : -1.0;
    double margin = 1e9;
    for (uint32_t i = 0U; i < count; ++i) {
        const double ax = vertices[i].x / 16.0, ay = vertices[i].y / 16.0;
        const double bx = vertices[(i + 1U) % count].x / 16.0, by = vertices[(i + 1U) % count].y / 16.0;
        const double length = std::hypot(bx - ax, by - ay);
        if (length == 0.0) continue;
        // With y down, a clockwise polygon (area > 0) has its inside on the right of every edge.
        const double cross = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        margin = std::min(margin, sign * cross / length);
    }
    return margin;
}

void TestPolygons() {
    // 16x16 row-major texture whose index encodes its own coordinates, so a
    // pixel tells which texel it sampled: index = v * 16 + u.
    std::vector<uint8_t> coordinate_texture(kTexSize * kTexSize);
    for (uint32_t v = 0U; v < kTexSize; ++v) {
        for (uint32_t u = 0U; u < kTexSize; ++u)
            coordinate_texture[v * kTexSize + u] = static_cast<uint8_t>(v * 16U + u);
    }
    const raster::Texture texture{.pixels = coordinate_texture.data(),
                                  .width = kTexSize,
                                  .height = kTexSize,
                                  .log2_width = 4U,
                                  .log2_height = 4U,
                                  .layout = MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR};
    const std::vector<uint16_t> lit = MakePalette();
    const raster::Palette palette{.entries = lit.data(), .light_levels = kLightLevels};
    std::vector<uint8_t> frame(kFrameBytes, 0U);
    const raster::Target target{.pixels = frame.data(), .width = kWidth, .height = kHeight, .pitch = kPitch};
    const auto pixel = [&](uint32_t x, uint32_t y) { return PixelAt(frame.data(), kPitch, x, y); };
    const auto clear = [&] { std::fill(frame.begin(), frame.end(), 0U); };

    // Axis-aligned quad: 16 texels across 16 pixels and 10 texels down 10
    // rows, flat light 2. Every covered pixel samples the texel under its
    // centre; the surrounding pixels stay untouched.
    {
        const micropixel_raster_vertex_t corners[4] = {Vertex(10, 10, 0, 0, 2), Vertex(26, 10, 16, 0, 2),
                                                       Vertex(26, 20, 16, 10, 2), Vertex(10, 20, 0, 10, 2)};
        raster::DrawPolygon(target, &texture, palette, 0U, corners, 4U);
        for (uint32_t y = 10U; y < 20U; ++y) {
            for (uint32_t x = 10U; x < 26U; ++x) {
                Require(pixel(x, y) == Lit(2U, static_cast<uint8_t>((y - 10U) * 16U + (x - 10U))));
            }
        }
        Require(pixel(9U, 10U) == 0U && pixel(26U, 10U) == 0U && pixel(10U, 9U) == 0U && pixel(10U, 20U) == 0U);
        // The opposite winding draws exactly the same pixels.
        std::vector<uint8_t> reference = frame;
        clear();
        const micropixel_raster_vertex_t reversed[4] = {corners[3], corners[2], corners[1], corners[0]};
        raster::DrawPolygon(target, &texture, palette, 0U, reversed, 4U);
        Require(frame == reference);
        clear();
    }

    // Gouraud light 0 -> 3 across 16 pixels, textured and flat colour: each
    // pixel takes the level under its centre and never leaves 0..3.
    {
        const micropixel_raster_vertex_t corners[4] = {Vertex(10, 10, 0, 0, 0), Vertex(26, 10, 16, 0, 3),
                                                       Vertex(26, 14, 16, 4, 3), Vertex(10, 14, 0, 4, 0)};
        raster::DrawPolygon(target, &texture, palette, 0U, corners, 4U);
        for (uint32_t x = 10U; x < 26U; ++x) {
            const auto level = static_cast<uint32_t>(std::floor((x - 10U + 0.5) * 3.0 / 16.0));
            Require(pixel(x, 10U) == Lit(level, static_cast<uint8_t>(x - 10U)));
        }
        clear();
        const micropixel_raster_vertex_t flat[4] = {Vertex(10, 10, 77, 0, 0), Vertex(26, 10, 0, 0, 3),
                                                    Vertex(26, 14, 0, 0, 3), Vertex(10, 14, 0, 0, 0)};
        raster::DrawPolygon(target, nullptr, palette, MICROPIXEL_RASTER_POLYGON_FLAT_COLOR, flat, 4U);
        for (uint32_t x = 10U; x < 26U; ++x) {
            const auto level = static_cast<uint32_t>(std::floor((x - 10U + 0.5) * 3.0 / 16.0));
            Require(pixel(x, 12U) == Lit(level, 77U));
        }
        Require(pixel(26U, 12U) == 0U);
        clear();
    }

    // Transparent index 0: the texel at (0, 0) is skipped, its neighbours drawn.
    {
        const micropixel_raster_vertex_t corners[3] = {Vertex(0, 0, 0, 0, 1), Vertex(4, 0, 4, 0, 1),
                                                       Vertex(0, 4, 0, 4, 1)};
        raster::DrawPolygon(target, &texture, palette, MICROPIXEL_RASTER_POLYGON_TRANSPARENT_INDEX0, corners, 3U);
        Require(pixel(0U, 0U) == 0U && pixel(1U, 0U) == Lit(1U, 1U) && pixel(0U, 1U) == Lit(1U, 16U));
        // Centre (3.5, 0.5) lies on the hypotenuse: the right edge is exclusive.
        Require(pixel(2U, 0U) == Lit(1U, 2U) && pixel(3U, 0U) == 0U && pixel(2U, 1U) == 0U);
        clear();
    }

    // Degenerate and out-of-range inputs draw nothing.
    {
        const micropixel_raster_vertex_t line[3] = {Vertex(0, 0, 0, 0, 0), Vertex(10, 10, 0, 0, 0),
                                                    Vertex(20, 20, 0, 0, 0)};
        raster::DrawPolygon(target, &texture, palette, 0U, line, 3U);
        const micropixel_raster_vertex_t above[3] = {Vertex(0, -30, 0, 0, 0), Vertex(60, -30, 0, 0, 0),
                                                     Vertex(30, -1, 0, 0, 0)};
        raster::DrawPolygon(target, &texture, palette, 0U, above, 3U);
        const micropixel_raster_vertex_t too_bright[3] = {Vertex(0, 0, 0, 0, 0), Vertex(10, 0, 0, 0, kLightLevels),
                                                          Vertex(0, 10, 0, 0, 0)};
        raster::DrawPolygon(target, &texture, palette, 0U, too_bright, 3U);
        for (uint8_t byte : frame) Require(byte == 0U);
    }

    // Random convex polygons: coverage follows pixel centres (within one pixel
    // of the exact edge), triangles sample the affine texel within one texel,
    // the level stays inside the corner range, both windings agree and a
    // polygon straddling the target edge matches its unclipped rendering.
    std::mt19937 rng{1234U};
    std::uniform_real_distribution<double> coordinate(-20.0, 80.0);
    std::uniform_real_distribution<double> texel(0.0, 48.0);
    std::uniform_int_distribution<int> level(0, kLightLevels - 1);
    std::vector<uint8_t> big_frame(kFrameBytes * 4U, 0U);
    const raster::Target big{
        .pixels = big_frame.data(), .width = kWidth * 2U, .height = kHeight * 2U, .pitch = kPitch * 2U};
    for (uint32_t iteration = 0U; iteration < 400U; ++iteration) {
        const bool quad = (iteration & 1U) != 0U;
        const uint32_t count = quad ? 4U : 3U;
        micropixel_raster_vertex_t corners[4]{};
        if (quad) {
            // Four angles in order around a centre give a simple polygon;
            // resample until every turn has the same sign (convex).
            for (;;) {
                const double cx = coordinate(rng), cy = coordinate(rng);
                double angles[4];
                for (double& angle : angles) angle = std::uniform_real_distribution<double>(0.0, 6.2831)(rng);
                std::sort(angles, angles + 4);
                for (uint32_t i = 0U; i < 4U; ++i) {
                    const double radius = std::uniform_real_distribution<double>(2.0, 40.0)(rng);
                    corners[i] = Vertex(cx + radius * std::cos(angles[i]), cy + radius * std::sin(angles[i]),
                                        texel(rng), texel(rng), static_cast<uint8_t>(level(rng)));
                }
                bool positive = false, negative = false;
                for (uint32_t i = 0U; i < 4U; ++i) {
                    const auto& a = corners[i];
                    const auto& b = corners[(i + 1U) % 4U];
                    const auto& c = corners[(i + 2U) % 4U];
                    const int64_t cross =
                        static_cast<int64_t>(b.x - a.x) * (c.y - b.y) - static_cast<int64_t>(b.y - a.y) * (c.x - b.x);
                    positive |= cross > 0;
                    negative |= cross < 0;
                }
                if (positive != negative) break;
            }
        } else {
            for (uint32_t i = 0U; i < 3U; ++i) {
                corners[i] =
                    Vertex(coordinate(rng), coordinate(rng), texel(rng), texel(rng), static_cast<uint8_t>(level(rng)));
            }
        }
        uint8_t low = 255U, high = 0U;
        for (uint32_t i = 0U; i < count; ++i) {
            low = std::min(low, corners[i].light);
            high = std::max(high, corners[i].light);
        }
        clear();
        raster::DrawPolygon(target, &texture, palette, 0U, corners, count);
        for (uint32_t y = 0U; y < kHeight; ++y) {
            for (uint32_t x = 0U; x < kWidth; ++x) {
                const double margin = InsideMargin(corners, count, x + 0.5, y + 0.5);
                const uint16_t value = pixel(x, y);
                if (margin > 1.0) {
                    Require(value != 0U);
                } else if (margin < -1.0) {
                    Require(value == 0U);
                }
                if (value == 0U) continue;
                const uint32_t light = (value >> 8U) & 0x7FU;
                Require(light >= low && light <= high);
                if (!quad && margin > 1.0) {
                    // Barycentric reference for the affine mapping.
                    const double x0 = corners[0].x / 16.0, y0 = corners[0].y / 16.0;
                    const double x1 = corners[1].x / 16.0, y1 = corners[1].y / 16.0;
                    const double x2 = corners[2].x / 16.0, y2 = corners[2].y / 16.0;
                    const double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                    const double px = x + 0.5, py = y + 0.5;
                    const double w1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / det;
                    const double w2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / det;
                    const double w0 = 1.0 - w1 - w2;
                    const double u = (w0 * corners[0].u + w1 * corners[1].u + w2 * corners[2].u) / 256.0;
                    const double v = (w0 * corners[0].v + w1 * corners[1].v + w2 * corners[2].v) / 256.0;
                    const double l = w0 * corners[0].light + w1 * corners[1].light + w2 * corners[2].light;
                    const auto wrapped_distance = [](int a, int b) {
                        const int d = std::abs(a - b) & 15;
                        return std::min(d, 16 - d);
                    };
                    const int sampled_u = value & 15, sampled_v = (value >> 4U) & 15;
                    Require(wrapped_distance(sampled_u, static_cast<int>(std::floor(u))) <= 1);
                    Require(wrapped_distance(sampled_v, static_cast<int>(std::floor(v))) <= 1);
                    Require(std::abs(static_cast<int>(light) - static_cast<int>(std::floor(l))) <= 1);
                }
            }
        }
        // Reversed winding: identical pixels.
        std::vector<uint8_t> reference = frame;
        micropixel_raster_vertex_t reversed[4]{};
        for (uint32_t i = 0U; i < count; ++i) reversed[i] = corners[count - 1U - i];
        clear();
        raster::DrawPolygon(target, &texture, palette, 0U, reversed, count);
        Require(frame == reference);
        // Shifted onto a target twice the size, the overlapping region matches.
        micropixel_raster_vertex_t shifted[4]{};
        for (uint32_t i = 0U; i < count; ++i) {
            shifted[i] = corners[i];
            shifted[i].x = static_cast<int16_t>(shifted[i].x + 32 * 16);
            shifted[i].y = static_cast<int16_t>(shifted[i].y + 24 * 16);
        }
        std::fill(big_frame.begin(), big_frame.end(), 0U);
        raster::DrawPolygon(big, &texture, palette, 0U, shifted, count);
        for (uint32_t y = 0U; y < kHeight; ++y) {
            for (uint32_t x = 0U; x < kWidth; ++x) {
                Require(pixel(x, y) == PixelAt(big_frame.data(), kPitch * 2U, x + 32U, y + 24U));
            }
        }
    }
}

// Polygon records through the service: validation of flags, padding, lights
// and texture requirements, then a rendered quad.
void TestPolygonRecords() {
    FakeGraphics backend;
    micropixel::device::GraphicsService graphics{backend, micropixel::device::DisplayInfo{}};
    EventQueue events;
    DirectSurfaceService surfaces{graphics, events, 0};
    const micropixel::runtime::GuestMemoryAccess access{.resolve = ResolveGuestMemory, .stable_base = true};
    surfaces.BindGuestMemory(access);
    RasterService service{true};
    service.BindGuestMemory(access);

    const std::vector<uint8_t> column_major = MakeTexture(true);
    const std::vector<uint8_t> row_major = MakeTexture(false);
    const std::vector<uint16_t> lit = MakePalette();
    std::memcpy(g_guest_memory + kStaging, column_major.data(), column_major.size());
    std::memcpy(g_guest_memory + kStaging + 1024U, row_major.data(), row_major.size());
    std::memcpy(g_guest_memory + kStaging + 2048U, lit.data(), lit.size() * sizeof(uint16_t));
    micropixel_raster_texture_upload_request_t upload{};
    upload.size = sizeof(upload);
    upload.width = kTexSize;
    upload.height = kTexSize;
    upload.layout = MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR;
    upload.pixels = kStaging;
    upload.length = kTexSize * kTexSize;
    Require(service.UploadTexture(upload).has_value());
    upload.texture_slot = 1U;
    upload.layout = MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR;
    upload.pixels = kStaging + 1024U;
    Require(service.UploadTexture(upload).has_value());
    upload.texture_slot = 2U;  // 12 x 16 row-major: not a power of two
    upload.width = 12U;
    upload.length = 12U * kTexSize;
    Require(service.UploadTexture(upload).has_value());
    micropixel_raster_palette_upload_request_t palette{};
    palette.size = sizeof(palette);
    palette.light_levels = kLightLevels;
    palette.entries = kStaging + 2048U;
    palette.length = kLightLevels * 256U * 2U;
    Require(service.UploadPalette(palette).has_value());

    micropixel_surface_create_request_t create{};
    create.size = sizeof(create);
    create.width = kWidth;
    create.height = kHeight;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    create.buffer_count = 1U;
    Require(surfaces.Create(create).has_value());
    micropixel::runtime::HostBufferView frame{};
    Require(surfaces.HostBuffer(1U, 0U, frame) == MICROPIXEL_STATUS_OK);

    const micropixel_raster_vertex_t corners[4] = {Vertex(4, 4, 0, 0, 1), Vertex(12, 4, 8, 0, 1),
                                                   Vertex(12, 12, 8, 8, 1), Vertex(4, 12, 0, 8, 1)};
    const micropixel_raster_vertex_t triangle_corners[3] = {corners[0], corners[1], corners[2]};
    const auto submit = [&](auto record) {
        DrawList list{0U};
        list.Add(record);
        list.Finish();
        return service.Submit(list.bytes.data(), list.bytes.size(), surfaces);
    };
    std::memset(frame.pixels, 0, kFrameBytes);
    Require(submit(Quad(1U, corners)).has_value());
    Require(PixelAt(frame, 4U, 4U) == Lit(1U, Texel(0U, 0U)) && PixelAt(frame, 11U, 11U) == Lit(1U, Texel(7U, 7U)));
    Require(PixelAt(frame, 12U, 4U) == 0U && PixelAt(frame, 4U, 12U) == 0U);
    std::memset(frame.pixels, 0, kFrameBytes);
    Require(submit(Triangle(1U, triangle_corners)).has_value());
    Require(PixelAt(frame, 11U, 5U) == Lit(1U, Texel(7U, 1U)) && PixelAt(frame, 4U, 11U) == 0U);
    // FLAT_COLOR needs no texture at all.
    Require(submit(Quad(200U, corners, MICROPIXEL_RASTER_POLYGON_FLAT_COLOR)).has_value());
    Require(PixelAt(frame, 5U, 5U) == Lit(1U, 0U));
    // A zero-area polygon is valid and draws nothing.
    const micropixel_raster_vertex_t line[3] = {Vertex(0, 0, 0, 0, 0), Vertex(5, 5, 0, 0, 0), Vertex(9, 9, 0, 0, 0)};
    std::memset(frame.pixels, 0, kFrameBytes);
    Require(submit(Triangle(1U, line)).has_value());
    for (uint32_t index = 0U; index < kFrameBytes; ++index) Require(frame.pixels[index] == 0U);

    const auto rejects = [&](auto record, int32_t status) {
        Require(submit(record).error().status == status);
        for (uint32_t index = 0U; index < kFrameBytes; ++index) Require(frame.pixels[index] == 0U);
    };
    rejects(Quad(0U, corners), MICROPIXEL_STATUS_NOT_FOUND);         // column-major slot
    rejects(Quad(7U, corners), MICROPIXEL_STATUS_NOT_FOUND);         // empty slot
    rejects(Quad(2U, corners), MICROPIXEL_STATUS_INVALID_ARGUMENT);  // non power-of-two
    rejects(Quad(1U, corners, 0x80U), MICROPIXEL_STATUS_INVALID_ARGUMENT);
    {
        micropixel_raster_vertex_t bright[4];
        std::copy_n(corners, 4U, bright);
        bright[2].light = kLightLevels;
        rejects(Quad(1U, bright), MICROPIXEL_STATUS_INVALID_ARGUMENT);
        micropixel_raster_vertex_t padded[4];
        std::copy_n(corners, 4U, padded);
        padded[1].reserved0 = 1U;
        rejects(Quad(1U, padded), MICROPIXEL_STATUS_INVALID_ARGUMENT);
        auto triangle = Triangle(1U, triangle_corners);
        triangle.reserved0 = 1U;
        rejects(triangle, MICROPIXEL_STATUS_INVALID_ARGUMENT);
        auto stale = Quad(1U, corners);
        stale.palette_slot = 5U;
        rejects(stale, MICROPIXEL_STATUS_STALE_STATE);
    }
    service.Shutdown();
    surfaces.Shutdown();
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

    RasterService service{true};
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
    upload.texture_slot = 0U;
    upload.width = kTexSize;
    upload.height = kTexSize;
    upload.layout = MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR;
    upload.pixels = kStaging;
    upload.length = kTexSize * kTexSize;

    // The real Host target must exist before records can be validated.
    DrawList early{kFrame0};
    early.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
    early.Finish();
    Require(service.Submit(early.bytes.data(), static_cast<uint32_t>(early.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_NOT_FOUND);
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
    Require(surfaces.HostBuffer(1U, kFrame0, frame0) == MICROPIXEL_STATUS_OK);
    Require(surfaces.HostBuffer(1U, kFrame1, frame1) == MICROPIXEL_STATUS_OK);
    Require(service.Submit(early.bytes.data(), static_cast<uint32_t>(early.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_STALE_STATE);
    Require(
        service.Submit(no_surface.bytes.data(), static_cast<uint32_t>(no_surface.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame0, 0U, 0U) == 0xFFFFU && PixelAt(frame0, 1U, 0U) == 0U);

    Require(service.UploadTexture(upload).has_value());
    auto bad = upload;
    bad.width = 24U;  // byte length does not match dimensions
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = upload;
    bad.width = 0U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = upload;
    bad.length = upload.length - 1U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = upload;
    bad.pixels = sizeof(g_guest_memory) - 16U;  // runs past Guest memory
    bad.texture_slot = 5U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);
    // A refused re-upload of an occupied slot keeps the old texture: slot 0
    // must still serve a column afterwards (checked once the palette exists).
    bad.texture_slot = 0U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);

    upload.texture_slot = 1U;
    upload.layout = MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR;
    upload.pixels = kStaging + 1024U;
    Require(service.UploadTexture(upload).has_value());

    micropixel_raster_palette_upload_request_t palette{};
    palette.size = sizeof(palette);
    palette.light_levels = kLightLevels;
    palette.entries = kStaging + 2048U;
    palette.length = kLightLevels * 256U * 2U;
    Require(service.UploadPalette(palette).has_value());
    auto bad_palette = palette;
    bad_palette.length -= 2U;
    Require(service.UploadPalette(bad_palette).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad_palette = palette;
    bad_palette.reserved0 = 1U;
    Require(service.UploadPalette(bad_palette).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    // A second palette slot with a single level, entries offset by 0x4000 so
    // the two slots are distinguishable in the frame.
    std::vector<uint16_t> flat(256U);
    for (uint32_t index = 0U; index < 256U; ++index) flat[index] = static_cast<uint16_t>(0x4000U | index);
    std::memcpy(g_guest_memory + kStaging + 8192U, flat.data(), flat.size() * sizeof(uint16_t));
    micropixel_raster_palette_upload_request_t second{};
    second.size = sizeof(second);
    second.palette_slot = 3U;
    second.light_levels = 1U;
    second.entries = kStaging + 8192U;
    second.length = 512U;
    Require(service.UploadPalette(second).has_value());
    {
        DrawList list{kFrame0};
        auto column = Column(20U, 0U, 3U, 0U, 0U, 3U, 0, 1 << 16);
        column.palette_slot = 3U;
        list.Add(column);
        auto sprite = Sprite(22, 0, 2U, 2U, 0U, 0U, 4U, 4U, 2U, 2U);
        sprite.palette_slot = 3U;
        list.Add(sprite);
        list.Finish();
        std::memset(frame0.pixels, 0, kFrameBytes);
        Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).has_value());
        Require(PixelAt(frame0, 20U, 1U) == (0x4000U | Texel(3U, 1U)));
        Require(PixelAt(frame0, 22U, 0U) == (0x4000U | Texel(4U, 4U)));
        // Light 1 does not exist in the flat palette; an empty slot is stale.
        DrawList too_bright{kFrame0};
        column.light_level = 1U;
        too_bright.Add(column);
        too_bright.Finish();
        Require(service.Submit(too_bright.bytes.data(), too_bright.bytes.size(), surfaces).error().status ==
                MICROPIXEL_STATUS_INVALID_ARGUMENT);
        DrawList empty_slot{kFrame0};
        column.light_level = 0U;
        column.palette_slot = 9U;
        empty_slot.Add(column);
        empty_slot.Finish();
        Require(service.Submit(empty_slot.bytes.data(), empty_slot.bytes.size(), surfaces).error().status ==
                MICROPIXEL_STATUS_STALE_STATE);
    }

    // Warp maps: a partial upload into an empty slot allocates the map with
    // the other rows skipped, later row updates land in place, and the map's
    // light ceiling is checked against the palette the record names.
    {
        std::vector<uint32_t> entries(8U * 4U);
        for (uint32_t y = 0U; y < 4U; ++y) {
            for (uint32_t x = 0U; x < 8U; ++x) entries[y * 8U + x] = (y << 24U) | (y << 12U) | x;
        }
        std::memcpy(g_guest_memory + kStaging + 12288U, entries.data(), entries.size() * sizeof(uint32_t));
        micropixel_raster_warp_upload_request_t warp_upload{};
        warp_upload.size = sizeof(warp_upload);
        warp_upload.warp_slot = 2U;
        warp_upload.width = 8U;
        warp_upload.height = 4U;
        warp_upload.row0 = 1U;
        warp_upload.row_count = 1U;
        warp_upload.entries = kStaging + 12288U + 32U;
        warp_upload.length = 32U;
        // Partial into an empty slot: row 1 lands, rows 0, 2, 3 are skipped.
        // The map allocation may fail first, leaving the slot empty.
        fail_at = allocation_attempts + 1U;
        Require(service.UploadWarp(warp_upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        fail_at = 0U;
        Require(service.UploadWarp(warp_upload).has_value());
        {
            micropixel_raster_warp_t probe{};
            probe.type = MICROPIXEL_RASTER_RECORD_WARP;
            probe.warp_slot = 2U;
            probe.texture_slot = 1U;
            probe.y = 40;
            DrawList probe_list{kFrame0};
            probe_list.Add(probe);
            probe_list.Finish();
            std::memset(frame0.pixels, 0, kFrameBytes);
            Require(service.Submit(probe_list.bytes.data(), probe_list.bytes.size(), surfaces).has_value());
            Require(PixelAt(frame0, 3U, 40U) == 0U && PixelAt(frame0, 3U, 42U) == 0U);
            Require(PixelAt(frame0, 3U, 41U) == Lit(1U, Texel(3U, 1U)));
        }
        warp_upload.row0 = 0U;
        warp_upload.row_count = 4U;
        warp_upload.entries = kStaging + 12288U;
        warp_upload.length = 128U;
        auto bad_warp = warp_upload;
        bad_warp.length = 127U;
        Require(service.UploadWarp(bad_warp).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
        bad_warp = warp_upload;
        bad_warp.entries = sizeof(g_guest_memory) - 64U;
        Require(service.UploadWarp(bad_warp).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);
        // Same size: the full upload lands in place without allocating.
        const size_t before_full = allocation_attempts;
        Require(service.UploadWarp(warp_upload).has_value());
        Require(allocation_attempts == before_full);

        micropixel_raster_warp_t warp{};
        warp.type = MICROPIXEL_RASTER_RECORD_WARP;
        warp.warp_slot = 2U;
        warp.texture_slot = 1U;
        warp.x = 0;
        warp.y = 10;
        warp.u_offset = 5U;
        DrawList list{kFrame0};
        list.Add(warp);
        list.Finish();
        std::memset(frame0.pixels, 0, kFrameBytes);
        Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
        for (uint32_t y = 0U; y < 4U; ++y) {
            for (uint32_t x = 0U; x < 8U; ++x) Require(PixelAt(frame0, x, 10U + y) == Lit(y, Texel(x + 5U, y)));
        }
        Require(PixelAt(frame0, 8U, 10U) == 0U && PixelAt(frame0, 0U, 14U) == 0U);
        // Row 2 replaced in place with light 0 entries pointing at u = 1.
        std::vector<uint32_t> row(8U, 1U);
        std::memcpy(g_guest_memory + kStaging + 16384U, row.data(), 32U);
        warp_upload.row0 = 2U;
        warp_upload.row_count = 1U;
        warp_upload.entries = kStaging + 16384U;
        warp_upload.length = 32U;
        const size_t before = allocation_attempts;
        Require(service.UploadWarp(warp_upload).has_value());
        Require(allocation_attempts == before);  // in place
        Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
        Require(PixelAt(frame0, 3U, 12U) == Lit(0U, Texel(6U, 0U)) &&
                PixelAt(frame0, 3U, 13U) == Lit(3U, Texel(8U, 3U)));
        // Fraction bits beyond the ABI limit are rejected.
        auto too_fine = warp;
        too_fine.u_fraction_bits = MICROPIXEL_RASTER_WARP_MAX_U_FRACTION_BITS + 1U;
        DrawList fine_list{kFrame0};
        fine_list.Add(too_fine);
        fine_list.Finish();
        Require(service.Submit(fine_list.bytes.data(), fine_list.bytes.size(), surfaces).error().status ==
                MICROPIXEL_STATUS_INVALID_ARGUMENT);
        // A record for a palette with too few levels for the map's ceiling.
        auto dim = warp;
        dim.palette_slot = 3U;
        DrawList dim_list{kFrame0};
        dim_list.Add(dim);
        dim_list.Finish();
        Require(service.Submit(dim_list.bytes.data(), dim_list.bytes.size(), surfaces).error().status ==
                MICROPIXEL_STATUS_INVALID_ARGUMENT);
        // Column-major (slot 0) or missing textures, empty warp slots and
        // stray flags are refused.
        auto wrong = warp;
        wrong.texture_slot = 0U;
        DrawList wrong_list{kFrame0};
        wrong_list.Add(wrong);
        wrong_list.Finish();
        Require(service.Submit(wrong_list.bytes.data(), wrong_list.bytes.size(), surfaces).error().status ==
                MICROPIXEL_STATUS_NOT_FOUND);
        wrong = warp;
        wrong.warp_slot = 7U;
        DrawList no_map{kFrame0};
        no_map.Add(wrong);
        no_map.Finish();
        Require(service.Submit(no_map.bytes.data(), no_map.bytes.size(), surfaces).error().status ==
                MICROPIXEL_STATUS_STALE_STATE);
        wrong = warp;
        wrong.flags = 0x80U;
        DrawList bad_flags{kFrame0};
        bad_flags.Add(wrong);
        bad_flags.Finish();
        Require(service.Submit(bad_flags.bytes.data(), bad_flags.bytes.size(), surfaces).error().status ==
                MICROPIXEL_STATUS_INVALID_ARGUMENT);
        // Entries with a reserved bit or an out-of-range light never land.
        row[0] = MICROPIXEL_RASTER_WARP_ENTRY_RESERVED;
        std::memcpy(g_guest_memory + kStaging + 16384U, row.data(), 32U);
        Require(service.UploadWarp(warp_upload).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
        Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
        Require(PixelAt(frame0, 0U, 12U) == Lit(0U, Texel(6U, 0U)));
        // Resizing the slot allocates anew and keeps the old map on failure.
        warp_upload.width = 4U;
        warp_upload.height = 8U;
        warp_upload.row0 = 0U;
        warp_upload.row_count = 8U;
        warp_upload.entries = kStaging + 12288U;
        warp_upload.length = 128U;
        fail_at = allocation_attempts + 1U;
        Require(service.UploadWarp(warp_upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        fail_at = 0U;
        Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
        Require(PixelAt(frame0, 7U, 10U) == Lit(0U, Texel(12U, 0U)));
        Require(service.UploadWarp(warp_upload).has_value());
        std::memset(frame0.pixels, 0, kFrameBytes);
        Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
        Require(PixelAt(frame0, 4U, 10U) == 0U && PixelAt(frame0, 3U, 17U) != 0U);
    }

    // Inject OOM for a new resource and for atomic replacement.
    upload.texture_slot = 2U;
    fail_at = allocation_attempts + 1U;
    Require(service.UploadTexture(upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    upload.texture_slot = 1U;
    fail_at = allocation_attempts + 1U;
    Require(service.UploadTexture(upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);

    fail_at = 0U;

    // A valid list renders exactly like the kernels.
    DrawList list{kFrame0};
    list.Add(Column(10U, 0U, kHeight - 1U, 0U, 1U, 3U, 0, 1 << 15));
    list.Add(SpanPair(40U, 7U, 0U, kWidth - 1U, 1U, 1U, 0U, 0, 0, 1 << 12, 1 << 12));
    list.Add(Span(41U, 0U, kWidth - 1U, 1U, 2U, 0, 3 << 12, 1 << 12, 0));
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
    Require(PixelAt(frame0, 0U, 41U) == Lit(2U, Texel(0U, 3U)) && PixelAt(frame0, 5U, 41U) == Lit(2U, Texel(5U, 3U)));
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
        broken.Add(Span(0U, 5U, 4U, 1U, 0U, 0, 0, 0, 0));  // x1 < x0
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Span(kHeight, 0U, 1U, 1U, 0U, 0, 0, 0, 0));  // row outside target
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Span(0U, 0U, kWidth, 1U, 0U, 0, 0, 0, 0));  // x1 outside target
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Span(0U, 0U, 1U, 0U, 0U, 0, 0, 0, 0));  // column-major slot in a span
        reject(broken, MICROPIXEL_STATUS_NOT_FOUND);
    }
    {
        DrawList broken{kFrame0};
        auto span = Span(0U, 0U, 1U, 1U, 0U, 0, 0, 0, 0);
        span.reserved0 = 1U;
        broken.Add(span);
        reject(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    // TEXT needs the device's text rasterizer; the plain service has none.
    auto add_bytes = [](DrawList& target_list, const std::vector<uint8_t>& record) {
        target_list.bytes.insert(target_list.bytes.end(), record.begin(), record.end());
        ++target_list.record_count;
    };
    {
        DrawList broken{kFrame0};
        add_bytes(broken, TextRecord(1, 2, 0xFFFFU, MICROPIXEL_SYSTEM_FONT_SMALL, "Hi"));
        reject(broken, MICROPIXEL_STATUS_UNSUPPORTED);
    }
    // With one bound, the record reaches it validated and its payload intact;
    // malformed headers never do.
    struct TextCalls final {
        uint32_t validated{};
        uint32_t drawn{};
        micropixel_font_handle_t font{};
        std::string text;
        int32_t x{};
        int32_t y{};
        uint16_t color{};
        bool accept{true};
    } calls;
    const raster::TextBinding binding{
        .validate =
            [](void* context, micropixel_font_handle_t font, const char* text, uint32_t length) {
                auto& record = *static_cast<TextCalls*>(context);
                ++record.validated;
                record.font = font;
                record.text.assign(text, length);
                return record.accept;
            },
        .draw =
            [](void* context, const raster::Target& target, int32_t x, int32_t y, uint16_t color,
               micropixel_font_handle_t, const char*, uint32_t) {
                auto& record = *static_cast<TextCalls*>(context);
                ++record.drawn;
                record.x = x;
                record.y = y;
                record.color = color;
                // Mark the origin so the test sees the draw reached the buffer.
                std::memcpy(target.pixels + static_cast<uint32_t>(y) * target.pitch + static_cast<uint32_t>(x) * 2U,
                            &color, sizeof(color));
                return true;
            },
        .context = &calls,
    };
    {
        DrawList drawn{kFrame0};
        add_bytes(drawn, TextRecord(7, 9, 0x1234U, MICROPIXEL_SYSTEM_FONT_LARGE, "Lap 2"));
        drawn.Add(Rect(0, 0, 1U, 1U, 0x07E0U));  // records after a TEXT still parse
        drawn.Finish();
        Require(service
                    .Submit(drawn.bytes.data(), static_cast<uint32_t>(drawn.bytes.size()), surfaces, nullptr, nullptr,
                            binding)
                    .has_value());
        Require(calls.validated == 1U && calls.drawn == 1U && calls.font == MICROPIXEL_SYSTEM_FONT_LARGE);
        Require(calls.text == "Lap 2" && calls.x == 7 && calls.y == 9 && calls.color == 0x1234U);
        Require(PixelAt(frame0, 7U, 9U) == 0x1234U && PixelAt(frame0, 0U, 0U) == 0x07E0U);
        std::memset(frame0.pixels, 0, kFrameBytes);
    }
    auto reject_text = [&](DrawList& broken, int32_t status) {
        broken.Finish();
        Require(service
                    .Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces, nullptr, nullptr,
                            binding)
                    .error()
                    .status == status);
        Require(calls.drawn == 1U);
        for (uint32_t index = 0U; index < kFrameBytes; ++index) {
            Require(frame0.pixels[index] == 0U);
        }
    };
    {
        DrawList broken{kFrame0};
        add_bytes(broken, TextRecord(0, 0, 0U, MICROPIXEL_SYSTEM_FONT_SMALL, "abc", 7U));  // length past the list
        reject_text(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        add_bytes(broken, TextRecord(0, 0, 0U, MICROPIXEL_SYSTEM_FONT_SMALL, "abcd", 1U));  // padding not zero
        reject_text(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        add_bytes(broken, TextRecord(0, 0, 0U, 0U, "abc"));  // no font
        reject_text(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        std::vector<uint8_t> empty = TextRecord(0, 0, 0U, MICROPIXEL_SYSTEM_FONT_SMALL, "");
        add_bytes(broken, empty);  // zero length
        reject_text(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        const uint32_t validated = calls.validated;
        add_bytes(broken, TextRecord(0, 0, 0U, MICROPIXEL_SYSTEM_FONT_SMALL, "a\xC3(b"));  // malformed UTF-8
        reject_text(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
        Require(calls.validated == validated);  // refused before the device is asked
    }
    {
        DrawList broken{kFrame0};
        calls.accept = false;  // the device rejects the font
        add_bytes(broken, TextRecord(0, 0, 0U, 99U, "abc"));
        reject_text(broken, MICROPIXEL_STATUS_INVALID_ARGUMENT);
        calls.accept = true;
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
        broken.Add(Sprite(0, 0, 4U, 4U, 0U, 0U, 13U, 0U, 4U, 4U));  // u0 + source_width past the texture
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
        broken.Header().record_count = 0U;  // no drawing records
        Require(
            service.Submit(broken.bytes.data(), static_cast<uint32_t>(broken.bytes.size()), surfaces).error().status ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    {
        DrawList broken{kFrame0};
        broken.Add(Column(0U, 0U, 1U, 0U, 0U, 0U, 0, 0));
        broken.Finish();
        broken.Header().flags = 1U;  // unknown header flag
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
    present.surface_handle = 1U;
    present.buffer_index = kFrame1;
    present.pitch = kPitch;
    present.source_width = kWidth;
    present.source_height = kHeight;
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

    // A list for the old surface must not write the replacement's buffer.
    Require(surfaces.Destroy(1U).has_value());
    auto replacement = surfaces.Create(create);
    Require(replacement.has_value() && replacement->surface_handle != 1U);
    Require(service.Submit(free_frame.bytes.data(), free_frame.bytes.size(), surfaces).error().status ==
            MICROPIXEL_STATUS_NOT_FOUND);
    micropixel::runtime::HostBufferView replacement_frame{};
    Require(surfaces.HostBuffer(replacement->surface_handle, 0U, replacement_frame) == MICROPIXEL_STATUS_OK);
    Require(PixelAt(replacement_frame, 0U, 0U) == 0U);
    free_frame.Header().surface_handle = replacement->surface_handle;
    Require(service.Submit(free_frame.bytes.data(), free_frame.bytes.size(), surfaces).has_value());

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
    RasterService service{true};
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
    palette.entries = kStaging + 2048U;
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
    Require(surfaces.HostBuffer(1U, 0U, frame) == MICROPIXEL_STATUS_OK);

    DrawList list{0U};
    list.Add(Column(0U, 0U, 0U, 0U, 3U, 5U, 0, 0));
    list.Add(Rect(1, 0, 1U, 1U, 0xF800U));
    list.Add(Sprite(2, 0, 1U, 1U, 0U, 0U, 1U, 0U, 1U, 1U, MICROPIXEL_RASTER_SPRITE_SOLID_COLOR, 0x07E0U));
    list.Finish();
    Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame, 0U, 0U) == Swap(Lit(3U, Texel(5U, 0U))));
    Require(PixelAt(frame, 1U, 0U) == Swap(0xF800U));
    Require(PixelAt(frame, 2U, 0U) == Swap(0x07E0U));
    fail_at = allocation_attempts + 1U;
    Require(service.UploadPalette(palette).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    fail_at = 0U;
    Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame, 0U, 0U) == Swap(Lit(3U, Texel(5U, 0U))));
    // A palette re-upload arrives canonical again and is converted anew.
    Require(service.UploadPalette(palette).has_value());
    Require(service.Submit(list.bytes.data(), static_cast<uint32_t>(list.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame, 0U, 0U) == Swap(Lit(3U, Texel(5U, 0U))));
    // So does one uploaded to another slot after the first was converted.
    palette.palette_slot = 1U;
    Require(service.UploadPalette(palette).has_value());
    auto other = Column(1U, 1U, 1U, 0U, 2U, 5U, 0, 0);
    other.palette_slot = 1U;
    DrawList two{0U};
    two.Add(Column(0U, 1U, 1U, 0U, 3U, 5U, 0, 0));
    two.Add(other);
    two.Finish();
    Require(service.Submit(two.bytes.data(), static_cast<uint32_t>(two.bytes.size()), surfaces).has_value());
    Require(PixelAt(frame, 0U, 1U) == Swap(Lit(3U, Texel(5U, 0U))));
    Require(PixelAt(frame, 1U, 1U) == Swap(Lit(2U, Texel(5U, 0U))));
    service.Shutdown();
    surfaces.Shutdown();
}

void TestDynamicTextures() {
    FakeGraphics backend;
    micropixel::device::GraphicsService graphics{backend, micropixel::device::DisplayInfo{}};
    EventQueue events;
    DirectSurfaceService surfaces{graphics, events, 0};
    const micropixel::runtime::GuestMemoryAccess access{.resolve = ResolveGuestMemory, .stable_base = true};
    surfaces.BindGuestMemory(access);
    micropixel_surface_create_request_t create{};
    create.size = sizeof(create);
    create.width = kWidth;
    create.height = kHeight;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    create.buffer_count = 1U;
    Require(surfaces.Create(create).has_value());
    micropixel::runtime::HostBufferView frame{};
    Require(surfaces.HostBuffer(1U, 0U, frame) == MICROPIXEL_STATUS_OK);
    const size_t baseline = allocations.size();
    RasterService service{true};
    service.BindGuestMemory(access);
    // Fixed little-endian upload fixture: slot 0, 512x256 INDEX8 column-major.
    // Keep literal offsets independent of the C struct layout.
    const uint8_t upload_wire[20] = {20, 0, 0, 0, 0, 2, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0};
    micropixel_raster_texture_upload_request_t upload{};
    std::memcpy(&upload, upload_wire, sizeof(upload));
    Require(upload.texture_slot == 0U && upload.width == 512U && upload.height == 256U);
    Require(upload.length == 512U * 256U);
    upload.pixels = kStaging;
    std::memset(g_guest_memory + kStaging, 7, upload.length);
    // New table allocation and then pixel allocation may each fail atomically.
    for (size_t stage = 1U; stage <= 2U; ++stage) {
        fail_at = allocation_attempts + stage;
        Require(service.UploadTexture(upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        Require(allocations.size() == baseline);
    }
    fail_at = 0U;
    Require(service.UploadTexture(upload).has_value());
    micropixel_raster_sprite_t sprite =
        Sprite(0, 0, 1U, 1U, 0U, 0U, 511U, 255U, 1U, 1U, MICROPIXEL_RASTER_SPRITE_SOLID_COLOR, 0x1234U);
    sprite.texture_slot = 0U;
    DrawList list{0U};
    list.Add(sprite);
    list.Finish();
    Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
    Require(PixelAt(frame, 0U, 0U) == 0x1234U);
    const size_t live = allocations.size();
    fail_at = allocation_attempts + 1U;
    Require(service.UploadTexture(upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    fail_at = 0U;
    Require(allocations.size() == live);
    Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
    auto bad = upload;
    bad.width = UINT16_MAX;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = upload;
    bad.pixels = sizeof(g_guest_memory) - 1U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);
    bad = upload;
    bad.width = 0U;
    Require(service.UploadTexture(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    // Growing the metadata preserves the existing slot on either allocation failure.
    const size_t before_growth = allocations.size();
    upload.texture_slot = 252U;
    for (size_t stage = 1U; stage <= 2U; ++stage) {
        fail_at = allocation_attempts + stage;
        Require(service.UploadTexture(upload).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        Require(allocations.size() == before_growth);
        const size_t before_draw = allocation_attempts;
        Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
        Require(allocation_attempts == before_draw);
    }
    fail_at = 0U;
    upload.texture_slot = 254U;
    upload.width = 1024U;
    upload.height = 1024U;
    upload.length = 1024U * 1024U;
    Require(service.UploadTexture(upload).has_value());  // exceeds the former entire 528 KiB pool
    upload.texture_slot = 253U;
    upload.width = 300U;
    upload.height = 200U;
    upload.length = 300U * 200U;
    Require(service.UploadTexture(upload).has_value());
    // Pixels are allocated only for uploaded slots; metadata grows with the highest slot.
    // Use and then reuse all 256 representable slots; there is no 32-slot quota.
    upload.width = 1U;
    upload.height = 1U;
    upload.length = 1U;
    for (uint32_t i = 0U; i < 256U; ++i) {
        upload.texture_slot = static_cast<uint8_t>(i);
        Require(service.UploadTexture(upload).has_value());
    }
    upload.texture_slot = 0U;
    upload.width = 512U;
    upload.height = 256U;
    upload.length = 512U * 256U;
    Require(service.UploadTexture(upload).has_value());
    Require(service.Submit(list.bytes.data(), list.bytes.size(), surfaces).has_value());
    service.Shutdown();
    sprite.texture_slot = 252U;
    DrawList missing{0U};
    missing.Add(sprite);
    missing.Finish();
    Require(service.Submit(missing.bytes.data(), missing.bytes.size(), surfaces).error().status ==
            MICROPIXEL_STATUS_NOT_FOUND);
    service.Shutdown();
    Require(allocations.size() == baseline);
    service.Shutdown();
    Require(allocations.size() == baseline);
    surfaces.Shutdown();
}

void TestArbitraryDimensions() {
    std::vector<uint8_t> texels(300U * 200U);
    for (size_t i = 0U; i < texels.size(); ++i) texels[i] = static_cast<uint8_t>(i % 251U);
    uint16_t lit[256]{};
    for (uint32_t i = 0U; i < 256U; ++i) lit[i] = static_cast<uint16_t>(i);
    uint16_t pixels[kWidth * kHeight]{};
    raster::Target target{
        .pixels = reinterpret_cast<uint8_t*>(pixels), .width = kWidth, .height = kHeight, .pitch = kPitch};
    raster::Texture texture{.pixels = texels.data(),
                            .width = 300U,
                            .height = 200U,
                            .log2_width = raster::Log2Exact(300U),
                            .log2_height = raster::Log2Exact(200U),
                            .layout = MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR};
    auto column = Column(0U, 0U, 47U, 0U, 0U, 299U, -5 * 65536, 65536);
    raster::DrawColumn(target, texture, lit, column);
    for (int32_t y = 0; y < 48; ++y) Require(pixels[y * kWidth] == texels[299U * 200U + (y - 5 + 200) % 200]);
    // Normalized UVs wrap through their fractional part for both texture sizes.
    texture.layout = MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR;
    raster::Texture other = texture;
    other.width = 200U;
    other.height = 300U;
    other.log2_width = raster::Log2Exact(200U);
    other.log2_height = raster::Log2Exact(300U);
    auto span = SpanPair(1U, 2U, 0U, kWidth - 1U, 0U, 1U, 0U, -32123, 12345, 7777, -8888);
    raster::DrawSpanPair(target, texture, other, lit, span);
    uint32_t s = static_cast<uint32_t>(span.s), t = static_cast<uint32_t>(span.t);
    for (uint32_t x = 0U; x < kWidth; ++x, s += span.ds, t += span.dt) {
        const auto sample = [&](uint32_t w, uint32_t h) {
            return texels[((t % 65536U) * h / 65536U) * w + (s % 65536U) * w / 65536U];
        };
        Require(pixels[kWidth + x] == sample(300U, 200U));
        Require(pixels[2U * kWidth + x] == sample(200U, 300U));
    }
    // Validate and execute the same arbitrary-size textures through wire records.
    raster::Texture textures[]{texture, other};
    const raster::Palette palette{.entries = lit, .light_levels = 1U};
    raster::Resources resources{.textures = textures, .texture_count = 2U, .palettes = &palette, .palette_count = 1U};
    auto wide = span;
    DrawList list{0U};
    list.Add(wide);
    list.Finish();
    micropixel_raster_header_t header{};
    Require(raster::ValidateDrawList(list.bytes.data(), list.bytes.size(), target, resources, header) ==
            MICROPIXEL_STATUS_OK);
    auto invalid_target = target;
    invalid_target.pitch = target.width * 2U - 2U;
    Require(raster::ValidateDrawList(list.bytes.data(), list.bytes.size(), invalid_target, resources, header) ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    raster::ExecuteDrawList(list.bytes.data(), header, target, resources);
    // Unknown padding and truncated records reject before any drawing.
    wide.reserved0[0] = 1U;
    DrawList invalid{0U};
    invalid.Add(wide);
    invalid.Finish();
    Require(raster::ValidateDrawList(invalid.bytes.data(), invalid.bytes.size(), target, resources, header) ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
    Require(raster::ValidateDrawList(list.bytes.data(), list.bytes.size() - 1U, target, resources, header) ==
            MICROPIXEL_STATUS_INVALID_ARGUMENT);
}

void TestDisabledPool() {
    FakeGraphics backend;
    micropixel::device::GraphicsService graphics{backend, micropixel::device::DisplayInfo{}};
    EventQueue events;
    DirectSurfaceService surfaces{graphics, events, 0};
    RasterService service{false};
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

void SharedTextureImageSamplingAndValidation() {
    using micropixel::device::BitmapView;
    uint8_t bgra[]{0, 0, 255, 255, 0, 255, 0, 0, 255, 0, 0, 128, 255, 255, 255, 255};
    BitmapView texture{bgra, sizeof(bgra), 2, 2, 8, MICROPIXEL_PIXEL_FORMAT_BGRA8888, MICROPIXEL_TEXTURE_FLAG_DYNAMIC};
    uint16_t pixels[6 * 4]{};
    raster::Target target{reinterpret_cast<uint8_t*>(pixels), 6, 4, 12, false};
    raster::Resources resources{};
    resources.texture_context = &texture;
    resources.resolve_texture = [](void* context, uint32_t handle, BitmapView& output) {
        if (handle != 77) return false;
        output = *static_cast<BitmapView*>(context);
        return true;
    };
    micropixel_raster_image_t image{};
    image.type = MICROPIXEL_RASTER_RECORD_IMAGE;
    image.opacity = 255;
    image.texture_handle = 77;
    image.width = image.height = 4;
    image.source_width = image.source_height = 2;
    const auto render = [&](micropixel_raster_image_t record) {
        DrawList list{0};
        list.Add(record);
        list.Finish();
        micropixel_raster_header_t header{};
        const auto status = raster::ValidateDrawList(list.bytes.data(), list.bytes.size(), target, resources, header);
        if (status == MICROPIXEL_STATUS_OK) raster::ExecuteDrawList(list.bytes.data(), header, target, resources);
        return status;
    };
    Require(render(image) == MICROPIXEL_STATUS_OK);
    Require(pixels[0] == 0xf800 && pixels[1] == 0xf800 && pixels[2] == 0 && pixels[3] == 0);
    Require(pixels[12] == 0x0010 && pixels[14] == 0xffff);
    std::fill_n(pixels, 24, 0);
    target.byte_swapped = true;
    image.x = -1;
    Require(render(image) == MICROPIXEL_STATUS_OK);
    Require(pixels[0] == 0x00f8 && pixels[1] == 0 && pixels[12] == 0x1000 && pixels[13] == 0xffff);
    image.texture_handle = 78;
    const auto saved = pixels[0];
    Require(render(image) == MICROPIXEL_STATUS_NOT_FOUND && pixels[0] == saved);
    image.texture_handle = 77;
    image.source_x = 1;
    Require(render(image) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    image.source_x = 0;
    texture.size = 1;
    Require(render(image) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    texture.size = sizeof(bgra);
    image.reserved0 = 1;
    Require(render(image) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    image.reserved0 = 0;

    // Unscaled RGB565 rows take the copy path: clipped on the left, converted
    // to panel order when the target is byte swapped, and identical to the
    // per-pixel result of a scaled draw with the same geometry otherwise.
    const uint16_t rgb565[]{0x1234, 0x5678, 0x9abc, 0xdef0, 0x0001, 0x0002};
    BitmapView canonical{
        reinterpret_cast<const uint8_t*>(rgb565), sizeof(rgb565), 3, 2, 6, MICROPIXEL_PIXEL_FORMAT_RGB565, 0};
    resources.texture_context = &canonical;
    image.x = -1;
    image.y = 1;
    image.width = image.source_width = 3;
    image.height = image.source_height = 2;
    std::fill_n(pixels, 24, 0);
    target.byte_swapped = true;
    Require(render(image) == MICROPIXEL_STATUS_OK);
    Require(pixels[6] == 0x7856 && pixels[7] == 0xbc9a && pixels[8] == 0 && pixels[12] == 0x0100 &&
            pixels[13] == 0x0200 && pixels[14] == 0);
    Require(pixels[0] == 0 && pixels[18] == 0);
    std::fill_n(pixels, 24, 0);
    target.byte_swapped = false;
    image.x = 0;
    Require(render(image) == MICROPIXEL_STATUS_OK);
    Require(pixels[6] == 0x1234 && pixels[8] == 0x9abc && pixels[12] == 0xdef0 && pixels[14] == 0x0002 &&
            pixels[9] == 0);
    // Scaling 3 source columns onto 5 destination columns samples floor(x * 3 / 5).
    std::fill_n(pixels, 24, 0);
    image.width = 5;
    Require(render(image) == MICROPIXEL_STATUS_OK);
    Require(pixels[6] == 0x1234 && pixels[7] == 0x1234 && pixels[8] == 0x5678 && pixels[9] == 0x5678 &&
            pixels[10] == 0x9abc);

    // A texture already stored in panel order (kRgb565ByteSwapped) copies
    // verbatim into a swapped target, swaps back into a canonical target and
    // blends from its canonical value.
    const uint16_t swapped_texels[]{0x3412, 0x7856, 0xbc9a, 0xf0de, 0x0100, 0x0200};
    BitmapView prepared{reinterpret_cast<const uint8_t*>(swapped_texels),
                        sizeof(swapped_texels),
                        3,
                        2,
                        6,
                        MICROPIXEL_PIXEL_FORMAT_RGB565,
                        micropixel::device::bitmap_flags::kRgb565ByteSwapped};
    resources.texture_context = &prepared;
    image.width = 3;
    std::fill_n(pixels, 24, 0);
    target.byte_swapped = true;
    Require(render(image) == MICROPIXEL_STATUS_OK);
    Require(pixels[6] == 0x3412 && pixels[8] == 0xbc9a && pixels[12] == 0xf0de && pixels[14] == 0x0200);
    std::fill_n(pixels, 24, 0);
    target.byte_swapped = false;
    Require(render(image) == MICROPIXEL_STATUS_OK);
    Require(pixels[6] == 0x1234 && pixels[8] == 0x9abc && pixels[12] == 0xdef0 && pixels[14] == 0x0002);
    std::fill_n(pixels, 24, 0xffff);
    image.opacity = 128;
    Require(render(image) == MICROPIXEL_STATUS_OK);
    // Half of white and 0x0001 (only the blue LSB set): the same value the
    // canonical texture would produce.
    resources.texture_context = &canonical;
    uint16_t expected_blend[24]{};
    std::fill_n(expected_blend, 24, 0xffff);
    {
        raster::Target reference{reinterpret_cast<uint8_t*>(expected_blend), 6, 4, 12, false};
        std::swap(target, reference);
        Require(render(image) == MICROPIXEL_STATUS_OK);
        std::swap(target, reference);
    }
    Require(std::equal(pixels, pixels + 24, expected_blend));
    image.opacity = 255;
}

// Opaque unscaled IMAGE records ride the device copy engine when one is bound:
// consecutive eligible records form one batch (at most kMaxCopyBlocks), any
// other record or an ineligible IMAGE flushes first so overlaps keep their
// order, and a failed batch is redrawn by the CPU so the result never differs
// from the CPU-only path.
void SharedCopyEngineBatching() {
    using micropixel::device::BitmapView;
    using micropixel::device::OpaqueCopyBlock;
    constexpr uint32_t kTextureWidth = 96U;
    constexpr uint32_t kTextureHeight = 96U;
    std::vector<uint16_t> texels(kTextureWidth * kTextureHeight);
    for (size_t i = 0U; i < texels.size(); ++i) texels[i] = static_cast<uint16_t>(i * 2654435761U >> 5);
    BitmapView texture{reinterpret_cast<const uint8_t*>(texels.data()),
                       static_cast<uint32_t>(texels.size() * 2U),
                       kTextureWidth,
                       kTextureHeight,
                       kTextureWidth * 2U,
                       MICROPIXEL_PIXEL_FORMAT_RGB565,
                       micropixel::device::bitmap_flags::kRgb565ByteSwapped};
    constexpr uint32_t kTargetWidth = 160U;
    constexpr uint32_t kTargetHeight = 120U;
    std::vector<uint16_t> hardware(kTargetWidth * kTargetHeight);
    std::vector<uint16_t> software(kTargetWidth * kTargetHeight);
    raster::Target target{reinterpret_cast<uint8_t*>(hardware.data()), kTargetWidth, kTargetHeight, kTargetWidth * 2U,
                          true};
    raster::Target reference{reinterpret_cast<uint8_t*>(software.data()), kTargetWidth, kTargetHeight,
                             kTargetWidth * 2U, true};

    struct Engine final {
        std::vector<uint32_t> batch_sizes;
        std::vector<OpaqueCopyBlock> blocks;
        uint32_t fail_batch = UINT32_MAX;  // index of the batch that reports failure
        bool fail_after_partial_write = false;
    } engine;
    raster::Resources resources{};
    resources.texture_context = &texture;
    resources.resolve_texture = [](void* context, uint32_t handle, BitmapView& output) {
        if (handle != 5U) return false;
        output = *static_cast<BitmapView*>(context);
        return true;
    };
    raster::CopyBatch batch{};
    resources.copy_context = &engine;
    resources.copy_batch = &batch;
    resources.copy_blocks = [](void* context, const raster::Target& destination, const OpaqueCopyBlock* blocks,
                               uint32_t count) {
        auto& self = *static_cast<Engine*>(context);
        const bool fail = self.batch_sizes.size() == self.fail_batch;
        self.batch_sizes.push_back(count);
        for (uint32_t index = 0U; index < count; ++index) {
            const OpaqueCopyBlock& block = blocks[index];
            self.blocks.push_back(block);
            Require(block.width >= raster::kMinCopyBlockWidth &&
                    static_cast<uint64_t>(block.width) * block.height >= raster::kMinCopyBlockPixels);
            Require(block.source_x + block.width <= block.source_picture_width &&
                    block.source_y + block.height <= block.source_picture_height);
            Require(block.destination_x + block.width <= destination.width &&
                    block.destination_y + block.height <= destination.height);
            if (fail && !self.fail_after_partial_write) return false;
            for (uint32_t row = 0U; row < block.height; ++row) {
                const uint8_t* source = block.source_pixels + (block.source_y + row) * block.source_stride +
                                        block.source_x * 2U;
                uint8_t* out = destination.pixels + (block.destination_y + row) * destination.pitch +
                               block.destination_x * 2U;
                // A failing engine scribbles the first row of each block and
                // gives up so the fallback has something to repair.
                if (fail) {
                    std::fill_n(out, block.width * 2U, 0xa5);
                    return false;
                }
                std::memcpy(out, source, block.width * 2U);
            }
        }
        return !fail;
    };
    raster::Resources cpu_only = resources;
    cpu_only.copy_blocks = nullptr;
    cpu_only.copy_context = nullptr;
    cpu_only.copy_batch = nullptr;

    const auto image_at = [](int32_t x, int32_t y, uint32_t size, uint8_t opacity) {
        micropixel_raster_image_t image{};
        image.type = MICROPIXEL_RASTER_RECORD_IMAGE;
        image.opacity = opacity;
        image.texture_handle = 5U;
        image.x = x;
        image.y = y;
        image.width = image.height = size;
        image.source_width = image.source_height = size;
        return image;
    };
    const auto run = [&](const DrawList& list, const raster::Resources& with, raster::Target& on) {
        micropixel_raster_header_t header{};
        Require(raster::ValidateDrawList(list.bytes.data(), list.bytes.size(), on, with, header) ==
                MICROPIXEL_STATUS_OK);
        raster::ExecuteDrawList(list.bytes.data(), header, on, with);
    };
    const auto same_result = [&] { return hardware == software; };

    // 35 overlapping 96x96 images (one partly off-screen), then a RECT that
    // overlaps them, then a small image that is below the pixel threshold and
    // draws on the CPU, then a translucent image, then a large one again.
    DrawList list{0U};
    for (uint32_t index = 0U; index < 35U; ++index) {
        list.Add(image_at(static_cast<int32_t>(index % 7U) * 8 - 16, static_cast<int32_t>(index / 7U) * 5, 96U, 255U));
    }
    micropixel_raster_rect_t rect{};
    rect.type = MICROPIXEL_RASTER_RECORD_RECT;
    rect.opacity = 255U;
    rect.x = 40;
    rect.y = 30;
    rect.width = 50U;
    rect.height = 50U;
    rect.color = 0x07e0;
    list.Add(rect);
    list.Add(image_at(50, 40, 32U, 255U));
    list.Add(image_at(60, 50, 96U, 255U));
    list.Add(image_at(70, 60, 96U, 200U));
    list.Add(image_at(0, 0, 96U, 255U));
    list.Finish();

    std::fill(hardware.begin(), hardware.end(), 0x1111);
    std::fill(software.begin(), software.end(), 0x1111);
    run(list, resources, target);
    run(list, cpu_only, reference);
    Require(same_result());
    // Batches: 32 + 3 (flushed by the RECT), then the 32x32 image flushes
    // nothing and draws on the CPU, one 96x96 flushed by the translucent
    // image, and the final one flushed at the end of the list.
    Require(engine.batch_sizes.size() == 4U);
    Require(engine.batch_sizes[0] == raster::kMaxCopyBlocks && engine.batch_sizes[1] == 3U &&
            engine.batch_sizes[2] == 1U && engine.batch_sizes[3] == 1U);
    Require(engine.blocks.size() == 37U);
    // The off-screen image was clipped before it reached the engine.
    Require(engine.blocks[0].destination_x == 0U && engine.blocks[0].source_x == 16U && engine.blocks[0].width == 80U);

    // A failing batch (with or without partial writes) is redrawn by the CPU.
    for (const bool partial : {false, true}) {
        for (const uint32_t failing : {0U, 1U, 3U}) {
            engine = {};
            engine.fail_batch = failing;
            engine.fail_after_partial_write = partial;
            std::fill(hardware.begin(), hardware.end(), 0x2222);
            std::fill(software.begin(), software.end(), 0x2222);
            run(list, resources, target);
            run(list, cpu_only, reference);
            Require(same_result());
            Require(engine.batch_sizes.size() == 4U);
        }
    }

    // A canonical texture on a swapped target stays on the CPU (byte order
    // mismatch), as does every image on a target with no engine bound.
    engine = {};
    texture.flags = 0U;
    std::fill(hardware.begin(), hardware.end(), 0x3333);
    std::fill(software.begin(), software.end(), 0x3333);
    run(list, resources, target);
    run(list, cpu_only, reference);
    Require(same_result() && engine.batch_sizes.empty());
    // ...and rides the engine on a canonical target.
    target.byte_swapped = false;
    reference.byte_swapped = false;
    std::fill(hardware.begin(), hardware.end(), 0x4444);
    std::fill(software.begin(), software.end(), 0x4444);
    run(list, resources, target);
    run(list, cpu_only, reference);
    Require(same_result() && engine.batch_sizes.size() == 4U);
}

int main() {
    SharedTextureImageSamplingAndValidation();
    SharedCopyEngineBatching();
    Require(raster::Log2Exact(64U) == 6U && raster::Log2Exact(8U) == 3U && raster::Log2Exact(300U) == UINT8_MAX);
    TestKernelsAgainstReference();
    TestPolygons();
    TestPolygonRecords();
    TestServiceUploadsAndDraws();
    TestSwappedPanel();
    TestDisabledPool();
    TestDynamicTextures();
    TestArbitraryDimensions();
    Require(allocations.empty());
    std::puts("Raster: palette slots, warp maps, polygons, arbitrary sampling, OOM rollback and release passed");
    return 0;
}
