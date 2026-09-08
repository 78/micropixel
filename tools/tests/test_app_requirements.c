#include <assert.h>
#include <stdbool.h>

#include "app_requirements_fixture.h"
#include "runtime/bundle/app_requirements.h"
int main(void) {
    for (unsigned i = 0; i < MICROPIXEL_APP_CAPABILITY_COUNT; ++i)
        assert(strcmp(kMicropixelAppCapabilities[i], fixture_capabilities[i]) == 0);
    for (unsigned i = 0; i < MICROPIXEL_APP_SERVICE_COUNT; ++i)
        assert(strcmp(kMicropixelAppServices[i], fixture_services[i]) == 0);
    for (unsigned i = 0; i < sizeof(fixture_versions) / sizeof(fixture_versions[0]); ++i)
        assert(micropixel_app_same_major_update(fixture_versions[i].current, fixture_versions[i].candidate) ==
               fixture_versions[i].update);
    assert(micropixel_app_same_major_update("0.9.0", "0.10.0"));
    assert(micropixel_app_same_major_update("0.1.0", "0.2.0"));
    assert(micropixel_app_same_major_update("1.1.0", "1.1.1"));
    assert(!micropixel_app_same_major_update("0.1.0", "1.0.0"));
    assert(!micropixel_app_same_major_update("1.2.0", "1.1.9"));
    assert(!micropixel_app_same_major_update("1.2.0", "1.2.0"));
    assert(!micropixel_app_same_major_update("1.0.0.", "1.1.0"));
    assert(!micropixel_app_same_major_update("1.0.0", "1.01.0"));
    micropixel_app_requirements_t requirement = {.core_abi = 131072,
                                                 .min_width = 320,
                                                 .min_height = 320,
                                                 .services = {0, 65536},
                                                 .any_of = {3},
                                                 .layouts = 7,
                                                 .declared = true};
    micropixel_app_environment_t environment = {
        .core_abi = 131072, .width = 320, .height = 320, .services = {0, 65536}, .capabilities = 2};
    assert(micropixel_app_is_compatible(&requirement, &environment));
    environment.capabilities = 0;
    assert(!micropixel_app_is_compatible(&requirement, &environment));
    environment.capabilities = 1;
    environment.services[1] = 131072;
    assert(!micropixel_app_is_compatible(&requirement, &environment));
    environment.services[1] = 65536;
    environment.width = 240;
    assert(!micropixel_app_is_compatible(&requirement, &environment));
    assert(!micropixel_app_is_compatible(&requirement, NULL));
    requirement.required = 255;
    requirement.layouts = 0;
    environment.capabilities = 0;
    assert(micropixel_app_runtime_compatible(&requirement, &environment));
    environment.core_abi = 65536;
    assert(!micropixel_app_runtime_compatible(&requirement, &environment));
    environment.core_abi = 131072;
    environment.services[1] = 0;
    assert(!micropixel_app_runtime_compatible(&requirement, &environment));
    assert(!micropixel_app_runtime_compatible(&requirement, NULL));
    return 0;
}
