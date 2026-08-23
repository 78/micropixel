# Conformance-only Host hooks are compiled only in the dedicated test build.
if(CONFIG_MICROPIXEL_ENABLE_CONFORMANCE_TEST_HOOKS)
    list(APPEND micropixel_sources "conformance/guest_test_hooks.cpp")
endif()
