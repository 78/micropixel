#ifndef MICROPIXEL_SDK_GRAPHICS_HPP
#define MICROPIXEL_SDK_GRAPHICS_HPP

#include <stdint.h>

namespace micropixel {

class Application;
class Graphics;
class GraphicsFrame;
class Bitmap;

class Color final {
   public:
    [[nodiscard]] static constexpr Color Rgb(uint8_t red, uint8_t green, uint8_t blue) {
        return Color{(static_cast<uint32_t>(red) << 16U) | (static_cast<uint32_t>(green) << 8U) |
                     static_cast<uint32_t>(blue)};
    }

    [[nodiscard]] static constexpr Color Black() { return Rgb(0U, 0U, 0U); }
    [[nodiscard]] static constexpr Color White() { return Rgb(255U, 255U, 255U); }
    [[nodiscard]] static constexpr Color Green() { return Rgb(67U, 214U, 166U); }

    [[nodiscard]] constexpr uint8_t red() const { return static_cast<uint8_t>(rgb888_ >> 16U); }
    [[nodiscard]] constexpr uint8_t green() const { return static_cast<uint8_t>(rgb888_ >> 8U); }
    [[nodiscard]] constexpr uint8_t blue() const { return static_cast<uint8_t>(rgb888_); }

    [[nodiscard]] static constexpr Color Mix(Color foreground, Color background, uint8_t opacity) {
        const uint32_t inverse = 255U - opacity;
        return Rgb(static_cast<uint8_t>((foreground.red() * opacity + background.red() * inverse + 127U) / 255U),
                   static_cast<uint8_t>((foreground.green() * opacity + background.green() * inverse + 127U) / 255U),
                   static_cast<uint8_t>((foreground.blue() * opacity + background.blue() * inverse + 127U) / 255U));
    }

    [[nodiscard]] constexpr Color Darkened(uint8_t strength) const { return Mix(Black(), *this, strength); }
    [[nodiscard]] constexpr Color Lightened(uint8_t strength) const { return Mix(White(), *this, strength); }

    friend constexpr bool operator==(Color, Color) = default;

   private:
    explicit constexpr Color(uint32_t rgb888) : rgb888_(rgb888) {}
    uint32_t rgb888_{};

    friend class CommandBuffer;
};

struct Point final {
    int32_t x{};
    int32_t y{};
};

struct Rect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};

    [[nodiscard]] constexpr bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr int32_t center_x() const { return x + width / 2; }
    [[nodiscard]] constexpr int32_t center_y() const { return y + height / 2; }

    [[nodiscard]] constexpr bool contains(int32_t point_x, int32_t point_y) const {
        return !empty() && point_x >= x && point_y >= y &&
               static_cast<int64_t>(point_x) < static_cast<int64_t>(x) + width &&
               static_cast<int64_t>(point_y) < static_cast<int64_t>(y) + height;
    }

    [[nodiscard]] constexpr bool contains(Point point) const { return contains(point.x, point.y); }

    [[nodiscard]] constexpr Rect translated(int32_t delta_x, int32_t delta_y) const {
        return Rect{x + delta_x, y + delta_y, width, height};
    }

    [[nodiscard]] constexpr Rect inset(int32_t amount) const {
        return Rect{x + amount, y + amount, width - amount * 2, height - amount * 2};
    }

    [[nodiscard]] constexpr Rect intersection(Rect other) const {
        const int32_t left = x > other.x ? x : other.x;
        const int32_t top = y > other.y ? y : other.y;
        const int64_t this_right = static_cast<int64_t>(x) + width;
        const int64_t other_right = static_cast<int64_t>(other.x) + other.width;
        const int64_t this_bottom = static_cast<int64_t>(y) + height;
        const int64_t other_bottom = static_cast<int64_t>(other.y) + other.height;
        const int64_t right = this_right < other_right ? this_right : other_right;
        const int64_t bottom = this_bottom < other_bottom ? this_bottom : other_bottom;
        return right > left && bottom > top
                   ? Rect{left, top, static_cast<int32_t>(right - left), static_cast<int32_t>(bottom - top)}
                   : Rect{};
    }
};

class GraphicsInfo final {
   public:
    [[nodiscard]] constexpr uint32_t width() const { return width_; }
    [[nodiscard]] constexpr uint32_t height() const { return height_; }
    [[nodiscard]] constexpr uint32_t max_command_bytes() const { return max_command_bytes_; }
    [[nodiscard]] constexpr uint16_t max_commands() const { return max_commands_; }
    [[nodiscard]] constexpr uint16_t max_text_bytes() const { return max_text_bytes_; }
    [[nodiscard]] constexpr bool supports_surface_translation() const {
        return (capabilities_ & kSurfaceTranslationCapability) != 0U;
    }
    [[nodiscard]] constexpr bool supports_multi_submit_frames() const {
        return (capabilities_ & kMultiSubmitFrameCapability) != 0U;
    }
    [[nodiscard]] constexpr uint16_t max_frame_commands() const {
        return max_frame_commands_ == 0U ? max_commands_ : max_frame_commands_;
    }

   private:
    static constexpr uint32_t kSurfaceTranslationCapability = 1U << 0U;
    static constexpr uint32_t kMultiSubmitFrameCapability = 1U << 1U;

    constexpr GraphicsInfo(uint32_t width, uint32_t height, uint32_t capabilities, uint32_t max_command_bytes,
                           uint16_t max_commands, uint16_t max_text_bytes, uint16_t max_frame_commands)
        : width_(width),
          height_(height),
          max_command_bytes_(max_command_bytes),
          capabilities_(capabilities),
          max_commands_(max_commands),
          max_text_bytes_(max_text_bytes),
          max_frame_commands_(max_frame_commands) {}

    uint32_t width_{};
    uint32_t height_{};
    uint32_t max_command_bytes_{};
    uint32_t capabilities_{};
    uint16_t max_commands_{};
    uint16_t max_text_bytes_{};
    uint16_t max_frame_commands_{};

    friend class Graphics;
};

class CommandBuffer final {
   public:
    static constexpr uint32_t kCapacityBytes = 4096U;
    static constexpr uint32_t kCapacityCommands = 128U;

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer(CommandBuffer&&) = default;
    CommandBuffer& operator=(CommandBuffer&&) = default;

    void Reset();
    void Clear(Color color);
    void FillRect(Rect rect, Color color);
    void BlendRect(Rect rect, Color color, uint8_t opacity);
    void DrawText(int32_t x, int32_t y, const char* text, Color color, uint16_t font_size_px = 24U);
    void DrawTextCentered(int32_t center_x, int32_t y, const char* text, Color color, uint16_t font_size_px = 24U);
    void DrawBitmap(int32_t x, int32_t y, const Bitmap& bitmap);
    void DrawBitmapRegion(int32_t x, int32_t y, const Bitmap& bitmap, Rect source);
    void BlendBitmap(int32_t x, int32_t y, const Bitmap& bitmap, uint8_t opacity);
    void BlendBitmapRegion(int32_t x, int32_t y, const Bitmap& bitmap, Rect source, uint8_t opacity);
    void BeginSurface(Rect bounds, Point translation, bool translation_active);
    void EndSurface();
    void Submit();

    [[nodiscard]] constexpr uint32_t size_bytes() const { return size_; }
    [[nodiscard]] constexpr uint32_t command_count() const { return logical_command_count_; }

   private:
    struct CapabilityToken {};
    explicit CommandBuffer(CapabilityToken, uint32_t max_commands, uint32_t max_frame_commands, bool auto_submit)
        : max_commands_(max_commands < kCapacityCommands ? max_commands : kCapacityCommands),
          max_frame_commands_(max_frame_commands),
          auto_submit_(auto_submit) {
        Reset();
    }

    [[nodiscard]] uint8_t* Append(uint32_t bytes);
    [[nodiscard]] uint8_t* AppendUnchecked(uint32_t bytes);
    void SubmitBatch();
    void ResetBatch();
    void ContinueSurfaceInNewBatch();

    alignas(4) uint8_t bytes_[kCapacityBytes]{};
    uint32_t size_{};
    uint32_t batch_command_count_{};
    uint32_t logical_command_count_{};
    uint32_t frame_command_count_{};
    uint32_t max_commands_{};
    uint32_t max_frame_commands_{};
    Rect surface_bounds_{};
    Point surface_translation_{};
    bool surface_translation_active_{};
    bool surface_active_{};
    bool auto_submit_{};
    bool submitted_{};
    friend class Graphics;
    friend class GraphicsFrame;
};

class GraphicsFrame final {
   public:
    GraphicsFrame(const GraphicsFrame&) = delete;
    GraphicsFrame& operator=(const GraphicsFrame&) = delete;
    GraphicsFrame(GraphicsFrame&& other) noexcept;
    GraphicsFrame& operator=(GraphicsFrame&&) = delete;
    ~GraphicsFrame();

    [[nodiscard]] CommandBuffer CreateCommandBuffer(const GraphicsInfo& graphics_info) const;
    void Commit();

   private:
    struct CapabilityToken {};
    explicit constexpr GraphicsFrame(CapabilityToken) : active_(true) {}
    bool active_{};

    friend class Graphics;
};

class Graphics final {
   public:
    constexpr Graphics(const Graphics&) noexcept = default;
    constexpr Graphics& operator=(const Graphics&) noexcept = default;

    [[nodiscard]] GraphicsInfo info() const;
    [[nodiscard]] CommandBuffer CreateCommandBuffer() const;
    [[nodiscard]] CommandBuffer CreateCommandBuffer(const GraphicsInfo& info) const;
    [[nodiscard]] GraphicsFrame BeginFrame() const;

   private:
    struct CapabilityToken {};
    explicit constexpr Graphics(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
