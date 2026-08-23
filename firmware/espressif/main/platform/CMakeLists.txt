# CMake selects exactly one implementation for every abstract backend. The
# rest of the firmware sees ConfiguredPlatform(), never CONFIG_* branches.
list(APPEND micropixel_sources
    "platform/random_backend.cpp"
)
list(APPEND micropixel_requires esp_hw_support)

# ESP-IDF expands component requirements before Kconfig values are available,
# so declare the union of P4 platform dependencies here. The source list below
# still selects exactly one implementation per backend.
list(APPEND micropixel_requires
    esp_driver_dma esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc
    esp_driver_ppa esp_driver_uart esp_driver_usb_serial_jtag
    button esp_lcd esp_lvgl_adapter esp_lcd_touch_gt911 esp_timer lvgl__lvgl
)

if(CONFIG_MICROPIXEL_METALIO_CLAW4_HOST_BACKEND)
    list(APPEND micropixel_sources
        "platform/metalio-claw4/board_hardware.cpp"
        "platform/metalio-claw4/display/esp_lcd_nv3051f.c"
        "platform/metalio-claw4/display/dirty_region_coalescer.cpp"
        "platform/metalio-claw4/display/png_cover_decoder.cpp"
        "platform/metalio-claw4/display/screen_capture.cpp"
        "platform/metalio-claw4/graphics_adapter.cpp"
        "platform/metalio-claw4/platform.cpp"
        "platform/metalio-claw4/system_ui_adapter.cpp"
        "platform/metalio-claw4/display/retained_scene.cpp"
        "platform/metalio-claw4/input/gt911_input.cpp"
        "platform/metalio-claw4/input/tca9555_power_key.cpp"
        "platform/graphics/command_stream.cpp"
    )
    if(CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION)
        list(APPEND micropixel_sources "platform/metalio-claw4/display/retained_surface.cpp")
    endif()
    if(CONFIG_MICROPIXEL_METALIO_CLAW4_AUDIO)
        list(APPEND micropixel_sources "platform/metalio-claw4/audio/synth_audio.cpp")
    else()
        list(APPEND micropixel_sources "platform/null/audio_backend.cpp")
    endif()
else()
    list(APPEND micropixel_sources
        "platform/null/audio_backend.cpp"
        "platform/null/graphics_backend.cpp"
    )
endif()
