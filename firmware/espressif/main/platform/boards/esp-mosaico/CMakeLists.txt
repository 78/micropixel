list(APPEND micropixel_sources
    "platform/boards/esp-mosaico/i2s_audio_sink.cpp"
    "platform/boards/esp-mosaico/battery_peripheral.cpp"
    "platform/boards/esp-mosaico/display/brightness_controller.cpp"
    "platform/boards/esp-mosaico/display/display_pipeline.cpp"
    "platform/boards/esp-mosaico/display/panel_transition_compositor.cpp"
    "platform/boards/esp-mosaico/function_button.cpp"
    "platform/boards/esp-mosaico/haptic_actuator.cpp"
    "platform/boards/esp-mosaico/platform.cpp"
    "platform/boards/esp-mosaico/power_controller.cpp"
    "platform/boards/esp-mosaico/presentation.cpp"
    "platform/boards/esp-mosaico/sensor_peripheral.cpp"
    "platform/boards/esp-mosaico/status_led.cpp"
    "platform/boards/esp-mosaico/usb_cdc_console.cpp"
    "platform/drivers/power/bq27220.cpp"
    "platform/drivers/sensors/bmi270.cpp"
    "platform/drivers/sensors/bmm150.cpp"
    "platform/input/esp_lcd_touch_input.cpp"
    "platform/input/gpio_key_input.cpp"
    "platform/transports/development_display_control.cpp"
    "platform/transports/tinyusb_cdc_local_control.cpp"
)

list(APPEND micropixel_requires esp_codec_dev esp_lcd_co5300)
