list(APPEND micropixel_sources
    "platform/boards/metalio-claw4/battery_backend.cpp"
    "platform/boards/metalio-claw4/board_hardware.cpp"
    "platform/boards/metalio-claw4/device_catalog.cpp"
    "platform/boards/metalio-claw4/display/screen_capture.cpp"
    "platform/boards/metalio-claw4/display/display_pipeline.cpp"
    "platform/boards/metalio-claw4/gpio_backend.cpp"
    "platform/boards/metalio-claw4/haptics_backend.cpp"
    "platform/boards/metalio-claw4/input/tca9555_power_key.cpp"
    "platform/boards/metalio-claw4/platform.cpp"
    "platform/boards/metalio-claw4/sensor_backend.cpp"
    "platform/common/i2c_executor.cpp"
    "platform/radio/esp_hosted_radio.cpp"
    "platform/transports/usb_serial_jtag_local_control.cpp"
)

if(CONFIG_MICROPIXEL_METALIO_CLAW4_AUDIO)
    list(APPEND micropixel_sources
        "platform/boards/metalio-claw4/audio/i2s_audio_sink.cpp"
        "platform/boards/metalio-claw4/audio/synth_audio.cpp"
    )
else()
    list(APPEND micropixel_sources "platform/boards/metalio-claw4/audio/null_audio.cpp")
endif()
