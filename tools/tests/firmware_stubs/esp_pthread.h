#ifndef MICROPIXEL_TEST_STUB_ESP_PTHREAD_H
#define MICROPIXEL_TEST_STUB_ESP_PTHREAD_H

#include "esp_err.h"

struct esp_pthread_cfg_t final {
    int stack_size{};
    int prio{5};
    bool inherit_cfg{};
    const char* thread_name{};
    int pin_to_core{-1};
};

inline esp_pthread_cfg_t& TestPthreadConfiguration() {
    static thread_local esp_pthread_cfg_t configuration{};
    return configuration;
}

inline esp_pthread_cfg_t esp_pthread_get_default_config() { return {}; }

inline esp_err_t esp_pthread_get_cfg(esp_pthread_cfg_t* configuration) {
    if (configuration == nullptr) {
        return -1;
    }
    *configuration = TestPthreadConfiguration();
    return ESP_OK;
}

inline esp_err_t esp_pthread_set_cfg(const esp_pthread_cfg_t* configuration) {
    if (configuration == nullptr) {
        return -1;
    }
    TestPthreadConfiguration() = *configuration;
    return ESP_OK;
}

#endif
