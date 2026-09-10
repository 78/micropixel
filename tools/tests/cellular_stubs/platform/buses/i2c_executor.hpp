#pragma once
#include "esp_err.h"
namespace micropixel::platform::buses {
class I2cExecutor {
   public:
    enum class Priority { kHigh };
    esp_err_t Invoke(Priority, esp_err_t (*operation)(void*), void* context) { return operation(context); }
};
}  // namespace micropixel::platform::buses
