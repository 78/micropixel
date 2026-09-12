#include <cassert>
#include <cstring>
#include <string>

#include "runtime/guest_failure_detail.hpp"

int main() {
    using micropixel::runtime::FormatGuestTrapDetail;
    const char* trap = "Exception: unreachable";
    assert(std::strcmp(FormatGuestTrapDetail(trap, "", "").data(), trap) == 0);
    const auto failure = FormatGuestTrapDetail(trap, "", "asset=2122773377: PNG bitmap output allocation failed");
    assert(std::strstr(failure.data(), "PNG bitmap output allocation failed") != nullptr);
    assert(std::strstr(failure.data(), trap) != nullptr);
    const auto panic = FormatGuestTrapDetail(trap, "panic: texture failed", "");
    assert(std::strstr(panic.data(), "panic: texture failed") == panic.data());
    const std::string long_text(500, 'x');
    const auto bounded = FormatGuestTrapDetail(trap, long_text.c_str(), long_text.c_str());
    assert(bounded.back() == '\0');
    assert(std::strstr(bounded.data(), trap) != nullptr);
    assert(std::strstr(bounded.data(), "last decode error:") != nullptr);
    return 0;
}
