#pragma once

#include <cstdint>

namespace micropixel::platform {

enum class UsbResetRequest : uint8_t {
    kNone,
    kApplication,
    kDownload,
};

// Recognizes esptool's boot-entry control-line signature without treating an
// ordinary terminal open/close as a reset request. The RTS-only pulse emitted
// by idf_monitor is reported as an application reset, while esptool's DTR/RTS
// swap is reported as a download reset. Both the atomic Unix transition and
// the intermediate (DTR, RTS) = (1, 1) emitted by sequential Windows
// control-line updates are accepted.
class UsbDownloadResetDetector {
   public:
    [[nodiscard]] UsbResetRequest Observe(bool dtr, bool rts, uint64_t now_us) {
        if (!dtr && rts) {
            armed_ = true;
            armed_at_us_ = now_us;
            return UsbResetRequest::kNone;
        }

        if (!armed_) {
            return UsbResetRequest::kNone;
        }

        if (now_us < armed_at_us_ || (now_us - armed_at_us_) > kSignatureWindowUs) {
            armed_ = false;
            return UsbResetRequest::kNone;
        }

        if (dtr && !rts) {
            armed_ = false;
            return UsbResetRequest::kDownload;
        }

        if (!dtr && !rts) {
            armed_ = false;
            return UsbResetRequest::kApplication;
        }

        // Sequential host APIs briefly assert both lines while swapping them.
        // Any other state cancels the signature so it cannot remain stale.
        if (!(dtr && rts)) {
            armed_ = false;
        }
        return UsbResetRequest::kNone;
    }

   private:
    static constexpr uint64_t kSignatureWindowUs = 500'000U;

    bool armed_{false};
    uint64_t armed_at_us_{0U};
};

}  // namespace micropixel::platform
