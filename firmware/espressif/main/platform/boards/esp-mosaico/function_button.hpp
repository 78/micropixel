#pragma once

#include "device/contracts/input.hpp"
#include "esp_err.h"
#include "platform/input/gpio_key_input.hpp"

namespace micropixel::platform::esp_mosaico {

class FunctionButton final {
   public:
    FunctionButton();

    [[nodiscard]] esp_err_t Initialize(device::Input& input);

   private:
    input::GpioKeyInput button_;
};

}  // namespace micropixel::platform::esp_mosaico
