# Seeed reTerminal D1001 (ESP32-P4). Display, touch, power key and the local
# control bridge; audio, SD and sensors are added by later milestones.
list(APPEND micropixel_sources
    "platform/audio/es8311_i2s_audio_sink.cpp"
    "platform/audio/i2s_codec_audio_sink.cpp"
    "platform/boards/reterminal-d1001/board_hardware.cpp"
    "platform/boards/reterminal-d1001/display/dpi_framebuffers.cpp"
    "platform/boards/reterminal-d1001/display/jd9365_init_data.c"
    "platform/boards/reterminal-d1001/display_hardware.cpp"
    "platform/boards/reterminal-d1001/platform.cpp"
    "platform/boards/reterminal-d1001/power_controller.cpp"
    "platform/boards/reterminal-d1001/power_key.cpp"
    "platform/boards/reterminal-d1001/presentation.cpp"
    "platform/boards/reterminal-d1001/touch_hardware.cpp"
    "platform/input/esp_lcd_touch_input.cpp"
    "platform/wifi/esp_hosted_radio.cpp"
    "platform/transports/development_display_control.cpp"
    "platform/transports/usb_serial_jtag_local_control.cpp"
)

list(APPEND micropixel_requires esp_lcd_jd9365 esp_lcd_touch_gsl3670 esp_io_expander_pca9535 esp_lcd_touch
     esp_io_expander esp_codec_dev)