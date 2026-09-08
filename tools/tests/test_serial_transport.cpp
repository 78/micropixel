#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>

#include "platform/boards/esp-mosaico/usb_cdc_early_log_buffer.hpp"
#include "platform/boards/esp-mosaico/usb_download_reset_detector.hpp"
#include "platform/transports/ascii_line_framer.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestAsciiLineFramer() {
    micropixel::platform::transports::AsciiLineFramer framer;
    char storage[16]{};
    framer.Bind(storage, sizeof(storage));
    uint32_t lines = 0U;
    char latest[16]{};
    const auto receive = [&](const char* line) {
        ++lines;
        std::strncpy(latest, line, sizeof(latest) - 1U);
    };

    const uint8_t first[] = {'M', 'P', 'X'};
    const uint8_t second[] = {'1', ' ', '1', '\r', '\n'};
    framer.Consume(first, sizeof(first), receive);
    framer.Consume(second, sizeof(second), receive);
    Check(lines == 1U && std::strcmp(latest, "MPX1 1") == 0, "fragmented CRLF line must be reconstructed");

    const uint8_t overlong[] = "0123456789abcdef trailing\nOK\n";
    framer.Consume(overlong, sizeof(overlong) - 1U, receive);
    Check(lines == 2U && std::strcmp(latest, "OK") == 0,
          "overlong input must be discarded through newline and then resynchronize");

    const uint8_t invalid[] = {'B', 'A', 'D', 0x01U, 'X', '\n', 'N', 'E', 'X', 'T', '\n'};
    framer.Consume(invalid, sizeof(invalid), receive);
    Check(lines == 3U && std::strcmp(latest, "NEXT") == 0,
          "non-printable input must discard the complete affected line");
}

using micropixel::platform::UsbDownloadResetDetector;
using micropixel::platform::UsbResetRequest;

void OrdinaryTerminalTransitionsDoNotReset() {
    UsbDownloadResetDetector detector;
    Check(detector.Observe(false, false, 0U) == UsbResetRequest::kNone, "idle lines must not reset");
    Check(detector.Observe(true, false, 10U) == UsbResetRequest::kNone, "opening a terminal with DTR must not reset");
    Check(detector.Observe(true, true, 20U) == UsbResetRequest::kNone, "asserting both terminal lines must not reset");
    Check(detector.Observe(false, false, 30U) == UsbResetRequest::kNone, "closing a terminal must not reset");
}

void IdfMonitorRtsPulseRestartsApplication() {
    UsbDownloadResetDetector detector;
    Check(detector.Observe(false, true, 100U) == UsbResetRequest::kNone, "RTS assertion must only arm reset detection");
    Check(detector.Observe(false, false, 100'100U) == UsbResetRequest::kApplication,
          "RTS release must request an application reset");
    Check(detector.Observe(false, false, 100'200U) == UsbResetRequest::kNone, "one RTS pulse must fire only once");
}

void UnixEsptoolSignatureResets() {
    UsbDownloadResetDetector detector;
    Check(detector.Observe(false, true, 100U) == UsbResetRequest::kNone, "RTS assertion must only arm download reset");
    Check(detector.Observe(true, false, 100'100U) == UsbResetRequest::kDownload,
          "atomic esptool line swap must request download reset");
    Check(detector.Observe(true, false, 100'200U) == UsbResetRequest::kNone, "one signature must fire only once");
}

void SequentialEsptoolSignatureResets() {
    UsbDownloadResetDetector detector;
    Check(detector.Observe(false, true, 200U) == UsbResetRequest::kNone, "classic reset must arm on RTS");
    Check(detector.Observe(true, true, 50'200U) == UsbResetRequest::kNone,
          "the sequential both-high transition must remain armed");
    Check(detector.Observe(true, false, 100'200U) == UsbResetRequest::kDownload,
          "classic sequential line swap must request download reset");
}

void StaleOrCancelledSignaturesDoNotReset() {
    UsbDownloadResetDetector detector;
    Check(detector.Observe(false, true, 0U) == UsbResetRequest::kNone, "signature must arm");
    Check(detector.Observe(true, false, 500'001U) == UsbResetRequest::kNone, "an expired signature must not reset");
    Check(detector.Observe(true, false, 500'002U) == UsbResetRequest::kNone,
          "an expired signature must stay cancelled");
}

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

void TestUsbCdcEarlyLogBuffer() {
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
}

}  // namespace

int main() {
    TestAsciiLineFramer();
    OrdinaryTerminalTransitionsDoNotReset();
    IdfMonitorRtsPulseRestartsApplication();
    UnixEsptoolSignatureResets();
    SequentialEsptoolSignatureResets();
    StaleOrCancelledSignaturesDoNotReset();
    TestUsbCdcEarlyLogBuffer();
    return 0;
}
