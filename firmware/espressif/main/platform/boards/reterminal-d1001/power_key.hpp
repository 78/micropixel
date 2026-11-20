// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_timer.h"

namespace micropixel::platform::reterminal_d1001 {

class D1001PowerController;

// Green power button on GPIO3 (active-high). A short release is a click and a
// one-second hold is the power-off request. Polling from a periodic esp_timer
// keeps the path free of a detached task and never touches I2C from a timer.
class D1001PowerKey final {
   public:
    explicit D1001PowerKey(D1001PowerController& power) : power_(power) {}
    D1001PowerKey(const D1001PowerKey&) = delete;
    D1001PowerKey& operator=(const D1001PowerKey&) = delete;
    ~D1001PowerKey();

    [[nodiscard]] esp_err_t Initialize();

   private:
    static void Timer(void* context);
    void Poll();

    D1001PowerController& power_;
    esp_timer_handle_t timer_{};
    bool pressed_{};
    bool long_fired_{};
    uint64_t pressed_at_us_{};
};

}  // namespace micropixel::platform::reterminal_d1001