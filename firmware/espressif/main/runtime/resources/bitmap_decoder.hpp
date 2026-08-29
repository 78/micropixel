#ifndef MICROPIXEL_RUNTIME_RESOURCES_BITMAP_DECODER_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_BITMAP_DECODER_HPP

#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "runtime/bundle/bundle_reader.h"

namespace micropixel::runtime {

class DecodedBitmap final {
   public:
    DecodedBitmap() = default;
    DecodedBitmap(const DecodedBitmap&) = delete;
    DecodedBitmap& operator=(const DecodedBitmap&) = delete;
    ~DecodedBitmap();

    [[nodiscard]] bool valid() const { return view_.data != nullptr; }      // NOLINT(readability-identifier-naming)
    [[nodiscard]] const device::BitmapView& view() const { return view_; }  // NOLINT(readability-identifier-naming)
    void ReleaseOwnership();

   private:
    friend bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, DecodedBitmap& decoded);
    friend bool AllocateBitmap(uint32_t width, uint32_t height, uint32_t pixel_format, DecodedBitmap& bitmap,
                               uint32_t stride_alignment_pixels);
    device::BitmapView view_{};
};

[[nodiscard]] bool DecodeBitmap(const micropixel_bundle_asset_view_t& asset, DecodedBitmap& decoded);
[[nodiscard]] bool AllocateBitmap(uint32_t width, uint32_t height, uint32_t pixel_format, DecodedBitmap& bitmap,
                                  uint32_t stride_alignment_pixels = 1U);

}  // namespace micropixel::runtime

#endif
