list(APPEND micropixel_sources
    "platform/boards/esp32-s3-box-3/board_hardware.cpp"
    "platform/boards/esp32-s3-box-3/display_hardware.cpp"
    "platform/boards/esp32-s3-box-3/host_mute_control.cpp"
    "platform/boards/esp32-s3-box-3/i2s_audio_sink.cpp"
    "platform/boards/esp32-s3-box-3/platform.cpp"
    "platform/boards/esp32-s3-box-3/sensor_peripheral.cpp"
    "platform/drivers/sensors/icm42670.cpp"
    "platform/input/esp_lcd_touch_input.cpp"
    "platform/sensors/polled_inertial_sensor_peripheral.cpp"
    "platform/transports/development_display_control.cpp"
    "platform/transports/usb_serial_jtag_local_control.cpp"
)

list(APPEND micropixel_requires
    espressif__icm42670
    espressif__esp_lcd_ili9341
    espressif__esp_lcd_touch_gt911
    espressif__esp_lcd_touch_tt21100
)
