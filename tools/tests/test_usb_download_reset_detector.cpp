#include <cstdlib>
#include <iostream>

#include "platform/boards/esp-mosaico/usb_download_reset_detector.hpp"

namespace {

using micropixel::platform::UsbDownloadResetDetector;
using micropixel::platform::UsbResetRequest;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

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

}  // namespace

int main() {
    OrdinaryTerminalTransitionsDoNotReset();
    IdfMonitorRtsPulseRestartsApplication();
    UnixEsptoolSignatureResets();
    SequentialEsptoolSignatureResets();
    StaleOrCancelledSignaturesDoNotReset();
    std::cout << "usb_download_reset_detector tests passed: 5 cases\n";
    return 0;
}
