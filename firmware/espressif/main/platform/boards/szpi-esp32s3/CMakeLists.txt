list(APPEND micropixel_sources
    "platform/boards/szpi-esp32s3/board_hardware.cpp"
    "platform/boards/szpi-esp32s3/display_hardware.cpp"
    "platform/boards/szpi-esp32s3/i2s_audio_sink.cpp"
    "platform/boards/szpi-esp32s3/platform.cpp"
    "platform/boards/szpi-esp32s3/sensor_peripheral.cpp"
    "platform/drivers/sensors/qmi8658.cpp"
    "platform/input/esp_lcd_touch_input.cpp"
    "platform/input/gpio_key_input.cpp"
    "platform/sensors/polled_inertial_sensor_peripheral.cpp"
    "platform/transports/development_display_control.cpp"
    "platform/transports/usb_serial_jtag_local_control.cpp"
)

list(APPEND micropixel_requires
    espressif__esp_lcd_touch_ft5x06
)
