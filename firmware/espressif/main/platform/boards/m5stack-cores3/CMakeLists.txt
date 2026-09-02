list(APPEND micropixel_sources
    "platform/boards/m5stack-cores3/battery_peripheral.cpp"
    "platform/boards/m5stack-cores3/board_hardware.cpp"
    "platform/boards/m5stack-cores3/display_hardware.cpp"
    "platform/boards/m5stack-cores3/i2s_audio_sink.cpp"
    "platform/boards/m5stack-cores3/platform.cpp"
    "platform/drivers/power/axp2101.cpp"
    "platform/drivers/sensors/bmi270.cpp"
    "platform/input/esp_lcd_touch_input.cpp"
    "platform/sensors/polled_inertial_sensor_peripheral.cpp"
    "platform/transports/development_display_control.cpp"
    "platform/transports/usb_serial_jtag_local_control.cpp"
)

list(APPEND micropixel_requires
    espressif__esp_lcd_ili9341
    espressif__esp_lcd_touch_ft5x06
)
