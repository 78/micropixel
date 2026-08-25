#ifndef MICROPIXEL_SDK_GRAPHICS_HPP
#define MICROPIXEL_SDK_GRAPHICS_HPP

#include <stdint.h>

#include "sdk/result.hpp"

namespace micropixel {

class Application;
class Renderer;
class Frame;
class Texture;
class StreamingTexture;
class TextureUpdateBatch;

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

    friend class Frame;
};

struct Point final {
    int32_t x{};
    int32_t y{};

    friend constexpr bool operator==(Point, Point) = default;
};

struct Size final {
    uint32_t width{};
    uint32_t height{};
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
        const int64_t translated_x = static_cast<int64_t>(x) + delta_x;
        const int64_t translated_y = static_cast<int64_t>(y) + delta_y;
        return translated_x >= INT32_MIN && translated_x <= INT32_MAX && translated_y >= INT32_MIN &&
                       translated_y <= INT32_MAX
                   ? Rect{static_cast<int32_t>(translated_x), static_cast<int32_t>(translated_y), width, height}
                   : Rect{};
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

    friend constexpr bool operator==(Rect, Rect) = default;
};

enum class PixelFormat : uint32_t {
    // Canonical bytes in Guest memory: B, G, R.
    kBgr888 = 1U,
    // Canonical bytes in Guest memory: B, G, R, A.
    kBgra8888 = 2U,
};

enum class SystemFont : uint16_t {
    kSmall = 1U,
    kMedium = 2U,
    kLarge = 3U,
    kTitle = 4U,
};

class RendererInfo final {
   public:
    [[nodiscard]] constexpr uint32_t width() const { return width_; }
    [[nodiscard]] constexpr uint32_t height() const { return height_; }
    [[nodiscard]] constexpr uint16_t max_draw_operations() const { return max_draw_operations_; }

   private:
    constexpr RendererInfo(uint32_t width, uint32_t height, uint32_t capabilities, uint16_t max_batch_commands,
                           uint16_t max_draw_operations, uint16_t max_frame_commands)
        : width_(width),
          height_(height),
          capabilities_(capabilities),
          max_batch_commands_(max_batch_commands),
          max_draw_operations_(max_draw_operations),
          max_frame_commands_(max_frame_commands == 0U ? max_batch_commands : max_frame_commands) {}

    [[nodiscard]] constexpr uint16_t max_batch_commands() const { return max_batch_commands_; }
    [[nodiscard]] constexpr uint16_t max_frame_commands() const { return max_frame_commands_; }
    [[nodiscard]] constexpr bool retained_translation_available() const { return (capabilities_ & (1U << 0U)) != 0U; }
    [[nodiscard]] constexpr bool multi_submit_available() const { return (capabilities_ & (1U << 1U)) != 0U; }

    uint32_t width_{};
    uint32_t height_{};
    uint32_t capabilities_{};
    uint16_t max_batch_commands_{};
    uint16_t max_draw_operations_{};
    uint16_t max_frame_commands_{};

    friend class Renderer;
    friend class Frame;
};

// Records one atomic display update. Frame owns its bounded command storage;
// transport batches and retained-translation acceleration are private details.
class Frame final {
   public:
    static constexpr uint32_t kCapacityBytes = 4096U;
    static constexpr uint32_t kCapacityCommands = 128U;
    static constexpr uint32_t kMaxStateDepth = 8U;

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&&) = delete;
    ~Frame();

    void Clear(Color color);
    void FillRect(Rect rect, Color color, uint8_t opacity = 255U);
    void DrawText(Point position, const char* text, Color color, SystemFont font = SystemFont::kMedium);
    void DrawTextCentered(int32_t center_x, int32_t y, const char* text, Color color,
                          SystemFont font = SystemFont::kMedium);
    void DrawTexture(Point position, const Texture& texture, uint8_t opacity = 255U);
    void DrawTexture(Point position, const Texture& texture, Rect source, uint8_t opacity = 255U);
    void DrawTexture(Rect destination, const Texture& texture, uint8_t opacity = 255U);
    void DrawTexture(Rect destination, const Texture& texture, Rect source, uint8_t opacity = 255U);
    void DrawTexture(Point position, const StreamingTexture& texture, uint8_t opacity = 255U);
    void DrawTexture(Point position, const StreamingTexture& texture, Rect source, uint8_t opacity = 255U);
    void DrawTexture(Rect destination, const StreamingTexture& texture, uint8_t opacity = 255U);
    void DrawTexture(Rect destination, const StreamingTexture& texture, Rect source, uint8_t opacity = 255U);

    void Save();
    void SetClipRect(Rect clip);
    void Translate(Point offset);
    void Restore();

    [[nodiscard]] Result<void> Present();

    [[nodiscard]] constexpr uint32_t draw_operation_count() const { return draw_operation_count_; }

   private:
    struct CapabilityToken {};
    Frame(CapabilityToken, const RendererInfo& info);

    [[nodiscard]] uint8_t* Append(uint32_t bytes);
    [[nodiscard]] uint8_t* AppendUnchecked(uint32_t bytes);
    [[nodiscard]] uint8_t* DiscardRecord(uint32_t bytes);
    void ResetBatch();
    [[nodiscard]] bool SubmitBatch();
    [[nodiscard]] bool StartHostFrame();
    void Fail(int32_t status);
    void Cancel();
    void ContinueStateInNewBatch();
    void EnsureStateEncoded();
    void CloseEncodedState();
    [[nodiscard]] Rect StateClip() const;
    [[nodiscard]] Rect EffectiveClip() const;
    [[nodiscard]] Point EffectiveTranslation() const;

    struct State final {
        Rect clip{};
        Rect clip_limit{};
        Point translation{};
        bool draw_started{};
    };

    alignas(4) uint8_t bytes_[kCapacityBytes]{};
    uint32_t size_{};
    uint32_t batch_command_count_{};
    uint32_t draw_operation_count_{};
    uint32_t frame_command_count_{};
    uint32_t max_batch_commands_{};
    uint32_t max_draw_operations_{};
    uint32_t max_frame_commands_{};
    Rect display_bounds_{};
    Rect retained_clip_{};
    Point retained_translation_{};
    State states_[kMaxStateDepth + 1U]{};
    uint32_t state_depth_{};
    uint32_t encoded_state_depth_{};
    uint32_t retained_scope_count_{};
    int32_t failure_status_{};
    alignas(4) uint8_t discard_record_[160U]{};
    bool retained_translation_available_{};
    bool multi_submit_available_{};
    bool retained_scope_selected_{};
    bool state_encoded_{};
    bool host_frame_active_{};
    bool presented_{};

    friend class Renderer;
};

class Renderer final {
   public:
    constexpr Renderer(const Renderer&) noexcept = default;
    constexpr Renderer& operator=(const Renderer&) noexcept = default;

    [[nodiscard]] RendererInfo info() const;
    [[nodiscard]] Frame BeginFrame() const;
    [[nodiscard]] Result<StreamingTexture> CreateStreamingTexture(Size size, PixelFormat pixel_format) const;
    [[nodiscard]] TextureUpdateBatch BeginTextureUpdateBatch() const;

   private:
    struct CapabilityToken {};
    explicit constexpr Renderer(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
