#ifndef MICROPIXEL_SDK_UI_TEXT_OVERFLOW_HPP
#define MICROPIXEL_SDK_UI_TEXT_OVERFLOW_HPP

#include <stdint.h>

#include "sdk/fixed_string.hpp"
#include "sdk/ui/button.hpp"

namespace micropixel::ui {

enum class TextOverflow : uint8_t { kClip, kReject };

inline constexpr uint32_t kTextOverflowDiagnosticBytes = 320U;

[[nodiscard]] inline constexpr const char* TextOverflowName(TextOverflow overflow) {
    switch (overflow) {
        case TextOverflow::kClip:
            return "clip";
        case TextOverflow::kReject:
            return "reject";
    }
    return "unknown";
}

[[nodiscard]] inline bool TextExceedsBounds(Rect bounds, const TextMetrics& metrics) {
    return !bounds.empty() && metrics.width != 0U && metrics.height != 0U &&
           (metrics.width > static_cast<uint32_t>(bounds.width) ||
            metrics.height > static_cast<uint32_t>(bounds.height));
}

[[nodiscard]] inline Result<Point> TextPosition(Rect bounds, const TextMetrics& metrics, TextOverflow overflow) {
    if (overflow == TextOverflow::kReject) {
        return CenteredLabelPosition(bounds, metrics);
    }
    if (overflow != TextOverflow::kClip || bounds.empty() || metrics.width == 0U || metrics.height == 0U ||
        metrics.width > static_cast<uint32_t>(INT32_MAX) || metrics.height > static_cast<uint32_t>(INT32_MAX)) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    const int64_t center_x = static_cast<int64_t>(bounds.x) + bounds.width / 2;
    const int64_t top = static_cast<int64_t>(bounds.y) +
                        (static_cast<int64_t>(bounds.height) - static_cast<int64_t>(metrics.height)) / 2;
    if (center_x < INT32_MIN || center_x > INT32_MAX || top < INT32_MIN || top > INT32_MAX) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    return Point{static_cast<int32_t>(center_x), static_cast<int32_t>(top)};
}

namespace detail {

inline void AppendDiagnosticText(FixedString<kTextOverflowDiagnosticBytes>& destination, const char* text) {
    if (text == nullptr) {
        destination.Append("<null>");
        return;
    }
    for (uint32_t index = 0U; text[index] != '\0'; ++index) {
        const uint8_t byte = static_cast<uint8_t>(text[index]);
        const char character[] = {static_cast<char>(byte < 0x20U || byte == 0x7fU ? '?' : byte), '\0'};
        if (!destination.Append(character)) {
            return;
        }
    }
}

inline void AppendTextControlState(FixedString<kTextOverflowDiagnosticBytes>& diagnostic, Rect bounds,
                                   const TextMetrics& metrics, const char* text, TextOverflow overflow, bool clipped) {
    diagnostic.Append(" bounds=(x=");
    diagnostic.AppendInt(bounds.x);
    diagnostic.Append(",y=");
    diagnostic.AppendInt(bounds.y);
    diagnostic.Append(",w=");
    diagnostic.AppendInt(bounds.width);
    diagnostic.Append(",h=");
    diagnostic.AppendInt(bounds.height);
    diagnostic.Append(") measured=(w=");
    diagnostic.AppendUint(metrics.width);
    diagnostic.Append(",h=");
    diagnostic.AppendUint(metrics.height);
    diagnostic.Append(") overflow=");
    diagnostic.Append(TextOverflowName(overflow));
    diagnostic.Append(" clipped=");
    diagnostic.Append(clipped ? "true" : "false");
    diagnostic.Append(" text=[");
    AppendDiagnosticText(diagnostic, text);
    diagnostic.Append("]");
}

}  // namespace detail

[[nodiscard]] inline FixedString<kTextOverflowDiagnosticBytes> FormatTextControlDescription(
    const char* control, Rect bounds, const TextMetrics& metrics, const char* text, TextOverflow overflow,
    bool clipped) {
    FixedString<kTextOverflowDiagnosticBytes> diagnostic;
    diagnostic.Append(control);
    detail::AppendTextControlState(diagnostic, bounds, metrics, text, overflow, clipped);
    return diagnostic;
}

[[nodiscard]] inline FixedString<kTextOverflowDiagnosticBytes> FormatTextOverflowDiagnostic(
    const char* control, const char* action, Rect bounds, const TextMetrics& metrics, const char* text,
    TextOverflow overflow, bool clipped) {
    FixedString<kTextOverflowDiagnosticBytes> diagnostic;
    diagnostic.Append(control);
    diagnostic.Append(" overflow: action=");
    diagnostic.Append(action);
    detail::AppendTextControlState(diagnostic, bounds, metrics, text, overflow, clipped);
    return diagnostic;
}

}  // namespace micropixel::ui

#endif
