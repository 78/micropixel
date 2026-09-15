// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>

#include "platform/lvgl/fonts/system_font_fallback.hpp"
int main() {
    lv_font_t terminal{}, proxy{}, language{};
    micropixel::platform::lvgl::SystemFontFallback chain;
    chain.Initialize(proxy, &terminal);
    chain.Activate(proxy, &language);
    assert(proxy.fallback == &language && language.fallback == &terminal && !terminal.fallback);
    lv_font_t replacement{};
    chain.Activate(proxy, &replacement);
    assert(proxy.fallback == &replacement && replacement.fallback == &terminal);
    chain.Activate(proxy, nullptr);
    assert(proxy.fallback == &terminal);
    chain.Activate(proxy, &language);
    const lv_font_t* current = &proxy;
    for (unsigned i = 0; current && i < 4; ++i) current = current->fallback;
    assert(!current);  // Icons absent from TTF terminate at the original built-in font.
}
