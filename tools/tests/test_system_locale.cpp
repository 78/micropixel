#include <array>
#include <cassert>
#include <string_view>

#include "host/ui/system_locale.hpp"

namespace {

using micropixel::host_ui::LocaleTagBuffer;
using micropixel::host_ui::NormalizeLocaleTag;
using micropixel::host_ui::SystemLocaleState;

std::string_view Normalize(std::string_view input) {
    static LocaleTagBuffer output;
    output = {};
    assert(NormalizeLocaleTag(input, output));
    return output.data();
}

void TestNormalization() {
    assert(Normalize("EN") == "en");
    assert(Normalize("en-us") == "en-US");
    assert(Normalize("ZH-hANS-cn") == "zh-Hans-CN");
    assert(Normalize("es-419") == "es-419");

    LocaleTagBuffer output{};
    assert(!NormalizeLocaleTag("", output));
    assert(!NormalizeLocaleTag("e", output));
    assert(!NormalizeLocaleTag("en_US", output));
    assert(!NormalizeLocaleTag("en--US", output));
    assert(!NormalizeLocaleTag("en-US-extra", output));
    assert(!NormalizeLocaleTag("zh-CN-Hans", output));
    assert(!NormalizeLocaleTag("abcd", output));
}

void TestMatching() {
    SystemLocaleState locale;
    assert(locale.requested() == "en");
    assert(locale.effective() == "en");

    constexpr std::array<std::string_view, 4U> available{"en", "zh-Hans", "zh-Hant", "ja"};
    assert(locale.SetRequested("zh-cn"));
    assert(locale.requested() == "zh-CN");
    locale.ResolveEffective(available);
    assert(locale.effective() == "zh-Hans");

    assert(locale.SetRequested("zh-HK"));
    locale.ResolveEffective(available);
    assert(locale.effective() == "zh-Hant");

    assert(locale.SetRequested("ja-JP"));
    locale.ResolveEffective(available);
    assert(locale.effective() == "ja");

    assert(locale.SetRequested("fr-FR"));
    locale.ResolveEffective(available);
    assert(locale.requested() == "fr-FR");
    assert(locale.effective() == "en");

    assert(!locale.SetRequested("not_a_locale"));
    assert(locale.requested() == "fr-FR");
}

void TestCurrentFirmwareAvailability() {
    SystemLocaleState locale;
    constexpr std::array<std::string_view, 1U> available{"en"};
    assert(locale.SetRequested("zh-Hans"));
    locale.ResolveEffective(available);
    assert(locale.requested() == "zh-Hans");
    assert(locale.effective() == "en");
}

}  // namespace

int main() {
    TestNormalization();
    TestMatching();
    TestCurrentFirmwareAvailability();
    return 0;
}
