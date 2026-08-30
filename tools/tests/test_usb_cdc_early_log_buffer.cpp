#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>

#include "platform/boards/esp-mosaico/usb_cdc_early_log_buffer.hpp"

namespace {

using micropixel::platform::esp_mosaico::UsbCdcEarlyLogBuffer;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string Contents(const UsbCdcEarlyLogBuffer& buffer) {
    std::string result;
    for (std::span<const char> segment : buffer.Segments()) {
        result.append(segment.data(), segment.size());
    }
    return result;
}

}  // namespace

int main() {
    std::array<char, 8U> storage{};
    UsbCdcEarlyLogBuffer buffer(storage);
    buffer.Append(std::span<const char>("abcde", 5U));
    Require(Contents(buffer) == "abcde", "initial bytes should remain in order");
    Require(buffer.dropped_bytes() == 0U, "initial append should not drop bytes");

    buffer.Append(std::span<const char>("FGHI", 4U));
    Require(Contents(buffer) == "bcdeFGHI", "wrapped buffer should retain the newest bytes in order");
    Require(buffer.dropped_bytes() == 1U, "wrap should report the displaced byte");

    buffer.Append(std::span<const char>("0123456789", 10U));
    Require(Contents(buffer) == "23456789", "oversized append should retain its newest capacity bytes");
    Require(buffer.dropped_bytes() == 11U, "oversized append should count old and excess bytes as dropped");

    buffer.Clear();
    Require(Contents(buffer).empty(), "clear should remove buffered bytes");
    Require(buffer.dropped_bytes() == 0U, "clear should reset the dropped-byte counter");

    buffer.AccountDropped(3U);
    Require(buffer.dropped_bytes() == 3U, "external formatting loss should be included in the dropped-byte counter");

    buffer.Release();
    Require(!buffer.has_storage(), "release should detach the caller-owned storage");
    buffer.Append(std::span<const char>("ignored", 7U));
    Require(Contents(buffer).empty(), "released buffer should ignore later appends");

    std::cout << "USB CDC early log buffer tests passed\n";
    return 0;
}
