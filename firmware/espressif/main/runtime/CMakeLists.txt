# Guest Runtime sources and their ESP-IDF dependencies stay isolated from
# board selection. Platform-specific drivers must not be added here.
list(APPEND micropixel_sources
    "runtime/abi/guest_abi.cpp"
    "runtime/abi/native_symbols.c"
    "runtime/abi/service_endpoints.cpp"
    "runtime/abi/service_registry.cpp"
    "runtime/bundlefs/bundlefs.cpp"
    "runtime/bundle/aot_package.cpp"
    "runtime/bundle/bundle_reader.c"
    "runtime/bundle/app_store.cpp"
    "runtime/app_runtime.cpp"
    "runtime/app_session.cpp"
    "runtime/audio/audio_playback_service.cpp"
    "runtime/event_queue.cpp"
    "runtime/guest_context.cpp"
    "runtime/key_event_bridge.cpp"
    "runtime/resources/bitmap_decoder.cpp"
    "runtime/resources/bitmap_store.cpp"
    "runtime/resources/resource_service.cpp"
    "runtime/services/storage_service.cpp"
    "runtime/services/timer_service.cpp"
    "runtime/touch_event_bridge.cpp"
    "runtime/wamr/diagnostics.c"
    "runtime/wamr/watchdog.c"
    "runtime/wamr/wamr_runtime.cpp"
)
list(APPEND micropixel_requires
    esp_new_jpeg esp_partition esp_psram esp_timer esphome__micro-opus espressif__libpng
    heap mbedtls nvs_flash spi_flash wasm-micro-runtime
)
