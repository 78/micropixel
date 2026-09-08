#ifndef MICROPIXEL_APP_REQUIREMENTS_H
#define MICROPIXEL_APP_REQUIREMENTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MICROPIXEL_APP_CAPABILITY_COUNT 8U
#define MICROPIXEL_APP_SERVICE_COUNT 11U
static const char* const kMicropixelAppCapabilities[MICROPIXEL_APP_CAPABILITY_COUNT] = {
    "input.touch",      "input.keys",          "audio.output", "sensor.acceleration",
    "sensor.gyroscope", "sensor.magnetometer", "haptics",      "gpio"};
static const char* const kMicropixelAppServices[MICROPIXEL_APP_SERVICE_COUNT] = {
    "system", "input", "graphics", "audio", "storage", "timer", "resource", "device", "sensor", "gpio", "haptics"};

typedef struct {
    uint32_t core_abi;
    uint16_t min_width;
    uint16_t min_height;
    uint32_t services[MICROPIXEL_APP_SERVICE_COUNT];
    uint8_t required;
    uint8_t optional;
    uint8_t any_of[8];
    uint8_t layouts;
    bool declared;
} micropixel_app_requirements_t;

typedef struct {
    uint32_t core_abi;
    uint32_t width;
    uint32_t height;
    uint32_t services[MICROPIXEL_APP_SERVICE_COUNT];
    uint8_t capabilities;
} micropixel_app_environment_t;

// Match Guest Runtime MakeDisplayTransform: requirements use a 720-unit
// short edge, never the physical pixel dimensions exposed by device services.
static inline void micropixel_app_set_logical_display(micropixel_app_environment_t* environment, uint32_t width,
                                                      uint32_t height) {
    environment->width = 0U;
    environment->height = 0U;
    if (width == 0U || height == 0U) return;
    const uint32_t short_edge = width < height ? width : height;
    environment->width = (uint32_t)(((uint64_t)width * 720U + short_edge / 2U) / short_edge);
    environment->height = (uint32_t)(((uint64_t)height * 720U + short_edge / 2U) / short_edge);
}

// Canonical numeric SemVer without overflow, including 0.x minor updates.
static inline bool micropixel_app_same_major_update(const char* current, const char* candidate) {
    if (current == NULL || candidate == NULL || strlen(current) > 31U || strlen(candidate) > 31U) return false;
    int comparison = 0;
    for (unsigned part = 0; part < 3U; ++part) {
        size_t a = 0, b = 0;
        while (current[a] >= '0' && current[a] <= '9') ++a;
        while (candidate[b] >= '0' && candidate[b] <= '9') ++b;
        if (a == 0U || b == 0U || (a > 1U && current[0] == '0') || (b > 1U && candidate[0] == '0')) return false;
        int order = a == b ? strncmp(candidate, current, a) : b > a ? 1 : -1;
        if (part == 0U && order != 0) return false;
        if (comparison == 0) comparison = order;
        if (current[a] != (part == 2U ? '\0' : '.') || candidate[b] != (part == 2U ? '\0' : '.')) return false;
        if (part < 2U) {
            current += a + 1U;
            candidate += b + 1U;
        }
    }
    return comparison > 0;
}

static inline bool micropixel_app_runtime_compatible(const micropixel_app_requirements_t* requirements,
                                                     const micropixel_app_environment_t* environment) {
    if (!requirements->declared) return true;
    if (environment == NULL || requirements->core_abi >> 16U != environment->core_abi >> 16U ||
        (requirements->core_abi & 65535U) > (environment->core_abi & 65535U))
        return false;
    for (uint32_t index = 0; index < MICROPIXEL_APP_SERVICE_COUNT; ++index) {
        if (requirements->services[index] != 0U &&
            (requirements->services[index] >> 16U != environment->services[index] >> 16U ||
             (requirements->services[index] & 65535U) > (environment->services[index] & 65535U)))
            return false;
    }
    return true;
}
// Adaptation is advisory for manual installation and startup. The scheduler
// can still use the full comparison before an unattended update.
static inline bool micropixel_app_is_compatible(const micropixel_app_requirements_t* requirements,
                                                const micropixel_app_environment_t* environment) {
    if (!micropixel_app_runtime_compatible(requirements, environment)) return false;
    if (!requirements->declared) return true;
    if (requirements->min_width > environment->width || requirements->min_height > environment->height ||
        (requirements->required & environment->capabilities) != requirements->required)
        return false;
    const uint8_t layout = environment->width == environment->height  ? 1U
                           : environment->width < environment->height ? 2U
                                                                      : 4U;
    if ((requirements->layouts & layout) == 0U) return false;
    for (uint32_t index = 0; index < 8; ++index) {
        if (requirements->any_of[index] != 0U && (requirements->any_of[index] & environment->capabilities) == 0U)
            return false;
    }
    return true;
}
#endif
