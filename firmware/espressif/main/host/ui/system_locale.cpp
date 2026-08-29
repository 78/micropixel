#include "host/ui/system_locale.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace micropixel::host_ui {
namespace {

struct ParsedLocale final {
    std::string_view language{};
    std::string_view script{};
    std::string_view region{};
};

bool IsAsciiAlpha(char value) { return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'); }

bool IsAsciiDigit(char value) { return value >= '0' && value <= '9'; }

char LowerAscii(char value) { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; }

char UpperAscii(char value) { return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value; }

bool AllAlpha(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), IsAsciiAlpha);
}

bool AllDigits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), IsAsciiDigit);
}

bool ParseNormalized(std::string_view tag, ParsedLocale& parsed) {
    const size_t first_separator = tag.find('-');
    parsed.language = tag.substr(0U, first_separator);
    if (parsed.language.size() < 2U || parsed.language.size() > 3U) {
        return false;
    }
    if (first_separator == std::string_view::npos) {
        return true;
    }

    const size_t second_start = first_separator + 1U;
    const size_t second_separator = tag.find('-', second_start);
    const std::string_view second = tag.substr(second_start, second_separator - second_start);
    if (second.size() == 4U) {
        parsed.script = second;
    } else if (second.size() == 2U || second.size() == 3U) {
        parsed.region = second;
    } else {
        return false;
    }
    if (second_separator == std::string_view::npos) {
        return true;
    }
    if (parsed.script.empty()) {
        return false;
    }
    parsed.region = tag.substr(second_separator + 1U);
    return !parsed.region.empty() && parsed.region.find('-') == std::string_view::npos;
}

std::string_view LikelyScript(const ParsedLocale& locale) {
    if (locale.language != "zh") {
        return {};
    }
    if (!locale.script.empty()) {
        return locale.script;
    }
    if (locale.region == "TW" || locale.region == "HK" || locale.region == "MO") {
        return "Hant";
    }
    if (locale.region == "CN" || locale.region == "SG") {
        return "Hans";
    }
    return {};
}

bool SameLanguageAndScript(const ParsedLocale& requested, const ParsedLocale& available) {
    if (requested.language != available.language) {
        return false;
    }
    const std::string_view requested_script = LikelyScript(requested);
    const std::string_view available_script = LikelyScript(available);
    return !requested_script.empty() && requested_script == available_script;
}

void CopyTag(std::string_view source, LocaleTagBuffer& destination) {
    destination.fill('\0');
    std::copy(source.begin(), source.end(), destination.begin());
}

std::string_view BufferView(const LocaleTagBuffer& buffer) {
    const auto end = std::find(buffer.begin(), buffer.end(), '\0');
    return {buffer.data(), static_cast<size_t>(end - buffer.begin())};
}

}  // namespace

bool NormalizeLocaleTag(std::string_view input, LocaleTagBuffer& output) {
    if (input.empty() || input.size() > MICROPIXEL_LOCALE_TAG_MAX_BYTES || input.front() == '-' ||
        input.back() == '-') {
        return false;
    }

    std::array<std::string_view, 3U> subtags{};
    size_t subtag_count = 0U;
    size_t start = 0U;
    while (start <= input.size()) {
        if (subtag_count >= subtags.size()) {
            return false;
        }
        const size_t separator = input.find('-', start);
        const size_t end = separator == std::string_view::npos ? input.size() : separator;
        if (end == start) {
            return false;
        }
        subtags[subtag_count++] = input.substr(start, end - start);
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }

    if (subtags[0].size() < 2U || subtags[0].size() > 3U || !AllAlpha(subtags[0])) {
        return false;
    }
    bool has_script = false;
    bool has_region = false;
    for (size_t index = 1U; index < subtag_count; ++index) {
        const std::string_view subtag = subtags[index];
        if (!has_script && !has_region && subtag.size() == 4U && AllAlpha(subtag)) {
            has_script = true;
            continue;
        }
        const bool region = (subtag.size() == 2U && AllAlpha(subtag)) || (subtag.size() == 3U && AllDigits(subtag));
        if (!has_region && region) {
            has_region = true;
            continue;
        }
        return false;
    }

    output.fill('\0');
    size_t output_index = 0U;
    for (size_t index = 0U; index < subtag_count; ++index) {
        if (index != 0U) {
            output[output_index++] = '-';
        }
        for (size_t character = 0U; character < subtags[index].size(); ++character) {
            char value = subtags[index][character];
            if (index == 0U) {
                value = LowerAscii(value);
            } else if (subtags[index].size() == 4U) {
                value = character == 0U ? UpperAscii(value) : LowerAscii(value);
            } else if (subtags[index].size() == 2U) {
                value = UpperAscii(value);
            }
            output[output_index++] = value;
        }
    }
    return true;
}

SystemLocaleState::SystemLocaleState() {
    CopyTag(kDefaultLocale, requested_);
    CopyTag(kDefaultLocale, effective_);
}

bool SystemLocaleState::SetRequested(std::string_view tag) {
    LocaleTagBuffer normalized{};
    if (!NormalizeLocaleTag(tag, normalized)) {
        return false;
    }
    requested_ = normalized;
    return true;
}

void SystemLocaleState::ResolveEffective(std::span<const std::string_view> available_locales) {
    const std::string_view requested_tag = requested();

    auto normalized_available = [&](std::string_view candidate, LocaleTagBuffer& normalized) {
        return NormalizeLocaleTag(candidate, normalized);
    };
    for (std::string_view candidate : available_locales) {
        LocaleTagBuffer normalized{};
        if (normalized_available(candidate, normalized) && BufferView(normalized) == requested_tag) {
            effective_ = normalized;
            return;
        }
    }

    ParsedLocale requested_parts{};
    (void)ParseNormalized(requested_tag, requested_parts);
    for (std::string_view candidate : available_locales) {
        LocaleTagBuffer normalized{};
        if (!normalized_available(candidate, normalized)) {
            continue;
        }
        ParsedLocale available_parts{};
        const std::string_view available_tag = BufferView(normalized);
        if (ParseNormalized(available_tag, available_parts) &&
            SameLanguageAndScript(requested_parts, available_parts)) {
            effective_ = normalized;
            return;
        }
    }
    for (std::string_view candidate : available_locales) {
        LocaleTagBuffer normalized{};
        if (!normalized_available(candidate, normalized)) {
            continue;
        }
        ParsedLocale available_parts{};
        if (ParseNormalized(BufferView(normalized), available_parts) &&
            available_parts.language == requested_parts.language && available_parts.script.empty() &&
            available_parts.region.empty()) {
            effective_ = normalized;
            return;
        }
    }
    CopyTag(kDefaultLocale, effective_);
}

std::string_view SystemLocaleState::requested() const { return BufferView(requested_); }

std::string_view SystemLocaleState::effective() const { return BufferView(effective_); }

const char* LocaleDisplayName(std::string_view tag) {
    if (tag == "en") {
        return "English";
    }
    if (tag == "zh-Hans") {
        return "Simplified Chinese";
    }
    if (tag == "zh-Hant") {
        return "Traditional Chinese";
    }
    if (tag == "ja") {
        return "Japanese";
    }
    if (tag == "ko") {
        return "Korean";
    }
    return "English";
}

}  // namespace micropixel::host_ui
