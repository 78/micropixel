#ifndef MICROPIXEL_TEST_STUB_ESP_LOG_H
#define MICROPIXEL_TEST_STUB_ESP_LOG_H

#include <stdarg.h>

static inline void micropixel_test_log(const char* tag, const char* format, ...) {
    (void)tag;
    (void)format;
}

#define ESP_LOGI(...) micropixel_test_log(__VA_ARGS__)
#define ESP_LOGW(...) micropixel_test_log(__VA_ARGS__)
#define ESP_LOGE(...) micropixel_test_log(__VA_ARGS__)

#endif
