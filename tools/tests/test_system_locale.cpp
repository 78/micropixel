#include <array>
#include <cassert>
#include <cstring>
#include <ctime>
#include <string_view>

#include "host/time/system_time.hpp"
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

using micropixel::firmware::system_time::FormatBeijingClock;
using micropixel::firmware::system_time::IsTrustedUtcTime;

void TestSystemTime() {
    assert(!IsTrustedUtcTime(static_cast<std::time_t>(1704067199)));
    assert(IsTrustedUtcTime(static_cast<std::time_t>(1704067200)));
    assert(IsTrustedUtcTime(static_cast<std::time_t>(4102444799)));
    assert(!IsTrustedUtcTime(static_cast<std::time_t>(4102444800)));

    assert(std::strcmp(FormatBeijingClock(0).data(), "") == 0);
    assert(std::strcmp(FormatBeijingClock(static_cast<std::time_t>(1704067200)).data(), "08:00") == 0);
    assert(std::strcmp(FormatBeijingClock(static_cast<std::time_t>(1704124799)).data(), "23:59") == 0);
    assert(std::strcmp(FormatBeijingClock(static_cast<std::time_t>(1704124800)).data(), "00:00") == 0);
}

}  // namespace

int main() {
    TestNormalization();
    TestMatching();
    TestCurrentFirmwareAvailability();
    TestSystemTime();
    return 0;
}
