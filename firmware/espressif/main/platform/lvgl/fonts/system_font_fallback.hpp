// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"
namespace micropixel::platform::lvgl {
// Capture the terminal before publishing the system proxy. Looking it up again
// through the registry after initialization would return the proxy itself.
class SystemFontFallback final {
   public:
    void Initialize(lv_font_t& proxy, const lv_font_t* terminal) {
        terminal_ = terminal;
        proxy.fallback = terminal_;
    }
    void Activate(lv_font_t& proxy, lv_font_t* language) const {
        if (language) language->fallback = terminal_;
        proxy.fallback = language ? language : terminal_;
    }

   private:
    const lv_font_t* terminal_{};
};
}  // namespace micropixel::platform::lvgl
