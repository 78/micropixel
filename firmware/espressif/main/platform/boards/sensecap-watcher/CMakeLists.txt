list(APPEND micropixel_sources
    "platform/boards/sensecap-watcher/board_power.cpp"
    "platform/boards/sensecap-watcher/display_hardware.cpp"
    "platform/boards/sensecap-watcher/i2s_audio_sink.cpp"
    "platform/boards/sensecap-watcher/knob_input.cpp"
    "platform/boards/sensecap-watcher/platform.cpp"
    "platform/boards/sensecap-watcher/touch_hardware.cpp"
    # The Host UI binds a device::Input, so the touch adapter is linked with the
    # board that drives it.
    "platform/input/esp_lcd_touch_input.cpp"
)
