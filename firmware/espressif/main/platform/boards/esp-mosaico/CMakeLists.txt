list(APPEND micropixel_sources
    "platform/boards/esp-mosaico/battery_backend.cpp"
    "platform/boards/esp-mosaico/display/panel_transition_compositor.cpp"
    "platform/boards/esp-mosaico/platform.cpp"
    "platform/boards/esp-mosaico/usb_cdc_console.cpp"
    "platform/common/i2c_executor.cpp"
    "platform/drivers/input/esp_lcd_touch_input.cpp"
    "platform/drivers/power/bq27220.cpp"
    "platform/transports/tinyusb_cdc_local_control.cpp"
)

list(APPEND micropixel_requires esp_lcd_co5300 waveshare__esp_lcd_touch_cst9217)
