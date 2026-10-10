# M5Stack Tab5 (ESP32-P4 rev v1.x). Display and touch bring-up sources; audio
# and power stages are added by later milestones.
list(APPEND micropixel_sources
    "platform/audio/es8388_i2s_audio_sink.cpp"
    "platform/audio/i2s_codec_audio_sink.cpp"
    "platform/boards/m5stack-tab5/battery_peripheral.cpp"
    "platform/boards/m5stack-tab5/board_hardware.cpp"
    "platform/boards/m5stack-tab5/display/dpi_framebuffers.cpp"
    "platform/boards/m5stack-tab5/display_hardware.cpp"
    "platform/boards/m5stack-tab5/ili9881c_init_data.c"
    "platform/boards/m5stack-tab5/inertial_axis.cpp"
    "platform/boards/m5stack-tab5/io_expander.cpp"
    "platform/boards/m5stack-tab5/platform.cpp"
    "platform/boards/m5stack-tab5/power_controller.cpp"
    "platform/boards/m5stack-tab5/presentation.cpp"
    "platform/boards/m5stack-tab5/rtc_clock.cpp"
    "platform/boards/m5stack-tab5/sd_card.cpp"
    "platform/drivers/power/ina226.cpp"
    "platform/drivers/rtc/rx8130.cpp"
    "platform/drivers/sensors/bmi270.cpp"
    "platform/input/gt911_input.cpp"
    "platform/sensors/polled_inertial_sensor_peripheral.cpp"
    "platform/sensors/polled_vector_sensor_peripheral.cpp"
    "platform/storage/sector_block_storage.cpp"
    "platform/wifi/esp_hosted_radio.cpp"
    "platform/transports/development_display_control.cpp"
    "platform/transports/usb_serial_jtag_local_control.cpp"
)

list(APPEND micropixel_requires esp_codec_dev esp_driver_sdmmc sdmmc)
