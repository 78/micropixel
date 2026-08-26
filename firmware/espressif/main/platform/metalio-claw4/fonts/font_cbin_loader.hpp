#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include "lvgl.h"

namespace micropixel::platform::metalio_claw4 {

class FontCbinLoaderAccess;

enum class FontCbinError : uint8_t {
    kNone,
    kInvalidArgument,
    kTruncated,
    kHeaderMismatch,
    kAbiMismatch,
    kHashUnavailable,
    kHashMismatch,
    kLimitExceeded,
    kInvalidFont,
    kInvalidGlyphs,
    kInvalidCmap,
    kInvalidKerning,
    kOutOfMemory,
};

class LoadedCbinFont final {
   public:
    LoadedCbinFont(const LoadedCbinFont&) = delete;
    LoadedCbinFont& operator=(const LoadedCbinFont&) = delete;
    LoadedCbinFont(LoadedCbinFont&&) = delete;
    LoadedCbinFont& operator=(LoadedCbinFont&&) = delete;
    ~LoadedCbinFont() = default;

    [[nodiscard]] const lv_font_t* font() const { return &font_; }
    [[nodiscard]] std::span<const uint8_t> payload() const { return {payload_, payload_size_}; }
    [[nodiscard]] uint32_t glyph_count() const { return glyph_count_; }
    [[nodiscard]] uint16_t size() const { return size_; }

   private:
    friend class FontCbinLoaderAccess;
    friend std::expected<std::unique_ptr<LoadedCbinFont>, FontCbinError> LoadFontCbin(std::span<const uint8_t> package);

    static constexpr size_t kMaxCmaps = 64U;

    LoadedCbinFont() = default;

    lv_font_t font_{};
    lv_font_fmt_txt_dsc_t descriptor_{};
    std::array<lv_font_fmt_txt_cmap_t, kMaxCmaps> cmaps_{};
    lv_font_fmt_txt_kern_pair_t kern_pairs_{};
    lv_font_fmt_txt_kern_classes_t kern_classes_{};
    const uint8_t* payload_{};
    size_t payload_size_{};
    uint32_t glyph_count_{};
    uint16_t size_{};
};

// package must remain mapped and byte-stable for the returned object's full
// lifetime. FontRegistry keeps the loaded object within the owning AppSession;
// the Bundle mapping is released only after GraphicsBackend guest resources.
[[nodiscard]] std::expected<std::unique_ptr<LoadedCbinFont>, FontCbinError> LoadFontCbin(
    std::span<const uint8_t> package);

}  // namespace micropixel::platform::metalio_claw4
