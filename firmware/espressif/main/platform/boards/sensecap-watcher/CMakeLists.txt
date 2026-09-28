list(APPEND micropixel_sources
    # The PC-facing port on this board is an external USB-UART bridge, so the
    # protocol rides the shared MPX1 line framer over UART0 instead of the SoC's
    # USB peripheral the other boards use, and the display shadow supplies the
    # complete frame that partial-buffer capture cannot.
    "platform/boards/esp32-s3-common/display_shadow.cpp"
    "platform/boards/sensecap-watcher/board_power.cpp"
    "platform/boards/sensecap-watcher/display_hardware.cpp"
    "platform/boards/sensecap-watcher/i2s_audio_sink.cpp"
    "platform/boards/sensecap-watcher/knob_input.cpp"
    "platform/boards/sensecap-watcher/platform.cpp"
    "platform/boards/sensecap-watcher/touch_hardware.cpp"
    "platform/boards/sensecap-watcher/uart_local_control.cpp"
    # The Host UI binds a device::Input, so the touch adapter is linked with the
    # board that drives it.
    "platform/input/esp_lcd_touch_input.cpp"
    "platform/transports/development_display_control.cpp"
)
