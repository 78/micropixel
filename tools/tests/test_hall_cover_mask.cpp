#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "host/ui/lvgl/square_common/hall_cover_mask.hpp"

using micropixel::host_ui::lvgl::square_common::MaskHallCoverRgb888;

void Check(bool condition) {
    if (!condition) {
        std::cerr << "Hall cover mask boundary regression failed\n";
        std::exit(1);
    }
}

void CheckStride(uint32_t size, uint32_t stride, uint32_t radius) {
    constexpr size_t kGuard = 8192U;
    constexpr uint8_t kUntouched = 0xa5U;
    const size_t bytes = static_cast<size_t>(stride) * size;
    std::vector<uint8_t> storage(kGuard + bytes + kGuard, kUntouched);
    auto* pixels = storage.data() + kGuard;
    MaskHallCoverRgb888(pixels, size, stride, radius, 0x123456U, 0x111214U);
    Check(std::all_of(storage.begin(), storage.begin() + kGuard, [](uint8_t b) { return b == kUntouched; }));
    Check(std::all_of(storage.begin() + kGuard + bytes, storage.end(), [](uint8_t b) { return b == kUntouched; }));
    for (uint32_t y = 0U; y < size; ++y) {
        for (uint32_t x = size * 3U; x < stride; ++x) {
            Check(pixels[static_cast<size_t>(y) * stride + x] == kUntouched);
        }
    }
    Check(pixels[0] == 0x56U && pixels[1] == 0x34U && pixels[2] == 0x12U);
    const size_t bottom_right = static_cast<size_t>(size - 1U) * stride + (size - 1U) * 3U;
    Check(pixels[bottom_right] == 0x14U && pixels[bottom_right + 1U] == 0x12U && pixels[bottom_right + 2U] == 0x11U);
    Check(pixels[static_cast<size_t>(size / 2U) * stride + (size / 2U) * 3U] == kUntouched);
}

int main() {
    CheckStride(202U, 606U, 22U);  // Packed P4 transition cover: original overrun.
    CheckStride(202U, 624U, 22U);  // LVGL cover with 48-byte row alignment.
    CheckStride(135U, 405U, 15U);  // Packed 480-square cover.
    CheckStride(135U, 432U, 15U);
    std::cout << "Hall cover mask tests passed (packed/padded boundaries, padding, corners, center).\n";
}
