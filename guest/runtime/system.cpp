#include "runtime/service_binding.hpp"
#include "sdk/clock.hpp"
#include "sdk/launch_arguments.hpp"
#include "sdk/localization.hpp"
#include "sdk/random.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::OpenService;
using micropixel::runtime::RequireOk;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache random_service;
ServiceCache system_service;
micropixel_system_launch_arguments_response_t cached_launch_arguments{};
bool launch_arguments_loaded{};
void LoadLaunchArguments() {
    if (launch_arguments_loaded) {
        return;
    }
    RequireOk(OpenService(system_service, MICROPIXEL_SERVICE_SYSTEM, MICROPIXEL_SYSTEM_INTERFACE_MAJOR,
                          MICROPIXEL_SYSTEM_INTERFACE_MINOR),
              "system.open");
    uint32_t response_size = 0U;
    RequireOk(CallService(system_service, MICROPIXEL_SYSTEM_METHOD_GET_LAUNCH_ARGUMENTS, nullptr, 0U,
                          &cached_launch_arguments, sizeof(cached_launch_arguments), response_size),
              "system.launch_arguments");
    if (response_size < sizeof(cached_launch_arguments) ||
        cached_launch_arguments.size < sizeof(cached_launch_arguments) || cached_launch_arguments.reserved0 != 0U ||
        cached_launch_arguments.count > MICROPIXEL_LAUNCH_ARGUMENT_MAX_COUNT ||
        cached_launch_arguments.bytes_length > MICROPIXEL_LAUNCH_ARGUMENT_MAX_BYTES) {
        micropixel::runtime::Panic("system.launch_arguments.invalid", MICROPIXEL_STATUS_INTERNAL);
    }
    for (uint32_t index = 0U; index < cached_launch_arguments.count; ++index) {
        const uint32_t offset = cached_launch_arguments.offsets[index];
        if (offset >= cached_launch_arguments.bytes_length) {
            micropixel::runtime::Panic("system.launch_arguments.offset", MICROPIXEL_STATUS_INTERNAL);
        }
        uint32_t end = offset;
        while (end < cached_launch_arguments.bytes_length && cached_launch_arguments.bytes[end] != '\0') {
            ++end;
        }
        if (end == cached_launch_arguments.bytes_length) {
            micropixel::runtime::Panic("system.launch_arguments.termination", MICROPIXEL_STATUS_INTERNAL);
        }
    }
    launch_arguments_loaded = true;
}

bool TextEquals(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

}  // namespace

namespace micropixel {

TimePoint Clock::Now() const { return TimePoint{micropixel_clock_now()}; }

uint32_t Random::U32() const {
    RequireOk(OpenService(random_service, MICROPIXEL_SERVICE_RANDOM, MICROPIXEL_RANDOM_INTERFACE_MAJOR,
                          MICROPIXEL_RANDOM_INTERFACE_MINOR),
              "random.open");
    micropixel_random_u32_response_t response{};
    uint32_t response_size = 0U;
    RequireOk(CallService(random_service, MICROPIXEL_RANDOM_METHOD_GET_U32, nullptr, 0U, &response, sizeof(response),
                          response_size),
              "random.u32");
    if (response_size < sizeof(response) || response.size < sizeof(response)) {
        runtime::Panic("random.u32.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return response.value;
}

uint32_t Random::Below(uint32_t upper_bound) const {
    if (upper_bound == 0U) {
        runtime::Panic("random.below.upper_bound", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    // Reject the short prefix that would make `% upper_bound` favor some
    // outcomes when 2^32 is not evenly divisible by upper_bound.
    const uint32_t rejection_threshold = static_cast<uint32_t>(0U - upper_bound) % upper_bound;
    uint32_t value = 0U;
    do {
        value = U32();
    } while (value < rejection_threshold);
    return value % upper_bound;
}

Locale Localization::CurrentLocale() const {
    Locale locale{};
    RequireOk(OpenService(system_service, MICROPIXEL_SERVICE_SYSTEM, MICROPIXEL_SYSTEM_INTERFACE_MAJOR,
                          MICROPIXEL_SYSTEM_INTERFACE_MINOR),
              "system.open");
    micropixel_system_locale_response_t wire{};
    uint32_t response_size = 0U;
    RequireOk(CallService(system_service, MICROPIXEL_SYSTEM_METHOD_GET_LOCALE, nullptr, 0U, &wire, sizeof(wire),
                          response_size),
              "system.locale");
    if (response_size < sizeof(wire) || wire.size < sizeof(wire) || wire.tag_length == 0U ||
        wire.tag_length > MICROPIXEL_LOCALE_TAG_MAX_BYTES || wire.tag[wire.tag_length] != '\0') {
        runtime::Panic("system.locale.invalid", MICROPIXEL_STATUS_INTERNAL);
    }
    CopyBytes(locale.tag_, wire.tag, wire.tag_length + 1U);
    return locale;
}

uint32_t LaunchArguments::count() const {
    LoadLaunchArguments();
    return cached_launch_arguments.count;
}

const char* LaunchArguments::Get(uint32_t index) const {
    LoadLaunchArguments();
    if (index >= cached_launch_arguments.count) {
        return nullptr;
    }
    return cached_launch_arguments.bytes + cached_launch_arguments.offsets[index];
}

const char* LaunchArguments::FindValue(const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    const uint32_t argument_count = count();
    for (uint32_t index = 0U; index < argument_count; ++index) {
        const char* argument = Get(index);
        if (TextEquals(argument, name)) {
            return index + 1U < argument_count ? Get(index + 1U) : nullptr;
        }
        const char* option = argument;
        const char* expected = name;
        while (*expected != '\0' && *option == *expected) {
            ++option;
            ++expected;
        }
        if (*expected == '\0' && *option == '=') {
            return option + 1U;
        }
    }
    return nullptr;
}

}  // namespace micropixel
