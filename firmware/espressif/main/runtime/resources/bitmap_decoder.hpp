#ifndef MICROPIXEL_RUNTIME_RESOURCES_BITMAP_DECODER_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_BITMAP_DECODER_HPP

#include <array>
#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "runtime/bundle/bundle_reader.h"

namespace micropixel::runtime {

class BundleSectionReader;

struct PngDecodeOptions final {
    uint32_t preferred_opaque_format{MICROPIXEL_PIXEL_FORMAT_BGR888};
    uint32_t scale_numerator{1U};
    uint32_t scale_denominator{1U};
    uint32_t stride_alignment_pixels{1U};
};

class DecodedBitmap final {
   public:
    DecodedBitmap() = default;
    DecodedBitmap(const DecodedBitmap&) = delete;
    DecodedBitmap& operator=(const DecodedBitmap&) = delete;
    ~DecodedBitmap();

    [[nodiscard]] bool valid() const { return view_.data != nullptr; }      // NOLINT(readability-identifier-naming)
    [[nodiscard]] const device::BitmapView& view() const { return view_; }  // NOLINT(readability-identifier-naming)
    void ReleaseOwnership();
    [[nodiscard]] const char* FailureDetail() const { return failure_detail_.data(); }

   private:
    friend bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, uint32_t preferred_opaque_format,
                             DecodedBitmap& decoded);
    friend bool DecodePngBitmap(const micropixel_bundle_asset_view_t& asset, const PngDecodeOptions& options,
                                DecodedBitmap& decoded);
    friend bool DecodePngBitmap(BundleSectionReader& reader, const PngDecodeOptions& options, DecodedBitmap& decoded);
    friend bool AllocateBitmap(uint32_t width, uint32_t height, uint32_t pixel_format, DecodedBitmap& bitmap,
                               uint32_t stride_alignment_pixels);
    device::BitmapView view_{};
    std::array<char, 96U> failure_detail_{};
};

[[nodiscard]] bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, DecodedBitmap& decoded);
[[nodiscard]] bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, uint32_t preferred_opaque_format,
                                DecodedBitmap& decoded);
// Non-interlaced PNGs: retain only the final bitmap and one decoded source row.
// Nearest sampling preserves both endpoints; a one-pixel extent selects source zero.
[[nodiscard]] bool DecodePngBitmap(const micropixel_bundle_asset_view_t& asset, const PngDecodeOptions& options,
                                   DecodedBitmap& decoded);
// The reader verifies the entire section before decoded becomes valid. It outlives all libpng longjmp frames.
[[nodiscard]] bool DecodePngBitmap(BundleSectionReader& reader, const PngDecodeOptions& options,
                                   DecodedBitmap& decoded);
[[nodiscard]] bool AllocateBitmap(uint32_t width, uint32_t height, uint32_t pixel_format, DecodedBitmap& bitmap,
                                  uint32_t stride_alignment_pixels = 1U);

}  // namespace micropixel::runtime

#endif
