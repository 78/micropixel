#include "platform/boards/esp-mosaico/function_button.hpp"

#include "platform/boards/esp-mosaico/board_config.hpp"

namespace micropixel::platform::esp_mosaico {

FunctionButton::FunctionButton()
    : button_({.pin = board::kFunctionButton,
               .code = device::KeyCode::kConfirm,
               .log_tag = "mosaico_button",
               .task_name = "mosaico_button",
               .task_core = 0}) {}

esp_err_t FunctionButton::Initialize(device::Input& input) { return button_.Initialize(input); }

}  // namespace micropixel::platform::esp_mosaico
